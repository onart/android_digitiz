#ifdef _WIN32

#include "screen/FrameSource.hpp"

#include <algorithm>
#include <chrono>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

namespace {

using Microsoft::WRL::ComPtr;

core::Recti to_recti(const RECT& r) {
    return core::Recti{r.left, r.top, r.right - r.left, r.bottom - r.top};
}

class DxgiFrameSource final : public IFrameSource {
public:
    ~DxgiFrameSource() override { stop(); }

    bool start() override {
        stop();
        if (!create_device()) {
            return false;
        }
        return create_duplication();
    }

    void stop() override {
        release_frame();
        staging_.Reset();
        duplication_.Reset();
        output_.Reset();
        context_.Reset();
        device_.Reset();
    }

    core::Recti bounds() const override { return bounds_; }

    bool next(FrameUpdate& out, int timeout_ms) override {
        if (!duplication_) {
            return false;
        }
        release_frame();

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(static_cast<UINT>(timeout_ms), &info,
                                                    &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return false; // a still screen, which is the normal case
        }
        if (FAILED(hr)) {
            // A mode change, a fullscreen app taking over, or the session
            // locking. All of them are recoverable by starting again, and all
            // of them mean the guest's copy is worthless.
            DZ_WARN("capture: AcquireNextFrame failed (0x%08lX); rebuilding duplication",
                    static_cast<unsigned long>(hr));
            recover();
            return false;
        }

        holding_ = true;
        ++frame_;

        out = FrameUpdate{};
        out.frame = frame_;
        out.accumulated = info.AccumulatedFrames;

        // A mouse-only update carries no new desktop image at all. Taking it
        // as a frame would mean re-sending tiles that did not change.
        if (info.LastPresentTime.QuadPart == 0) {
            release_frame();
            return false;
        }

        if (FAILED(resource.As(&frame_texture_))) {
            release_frame();
            return false;
        }

        // Take our own copy immediately, on the GPU, and read from that
        // instead. An acquired frame is only readable while it is held, and a
        // still screen never delivers another one -- so anything not read
        // during the tick it arrived on used to be unreadable until the
        // desktop happened to move again. With a copy of our own the pixels
        // are there whenever they are wanted.
        //
        // It is also the surface a compute encoder will read: the point of
        // this is eventually to compress here and carry blocks across the bus
        // rather than pixels.
        if (!ensure_desktop()) {
            release_frame();
            return false;
        }
        context_->CopyResource(desktop_.Get(), frame_texture_.Get());
        desktop_valid_ = true;

        if (!read_metadata(info, out)) {
            // No metadata is not a failure, it just means nothing was said.
            out.full = true;
            out.dirty.clear();
            out.moves.clear();
        }
        if (first_frame_) {
            first_frame_ = false;
            out.full = true; // the guest has nothing yet
        }
        return true;
    }

    bool read(core::Recti rect, std::vector<std::uint8_t>& out, int& stride) override {
        // Deliberately not tied to holding a frame; see the copy in next().
        if (!desktop_valid_ || !desktop_ || !context_) {
            return false;
        }

        // Clip to what was actually captured, and to whole pixels.
        const std::int32_t x0 = std::max(rect.x, bounds_.x);
        const std::int32_t y0 = std::max(rect.y, bounds_.y);
        const std::int32_t x1 = std::min(rect.x + rect.w, bounds_.x + bounds_.w);
        const std::int32_t y1 = std::min(rect.y + rect.h, bounds_.y + bounds_.h);
        if (x1 <= x0 || y1 <= y0) {
            return false;
        }
        const UINT w = static_cast<UINT>(x1 - x0);
        const UINT h = static_cast<UINT>(y1 - y0);

        if (!ensure_staging(w, h)) {
            return false;
        }

        // Source coordinates are relative to the output, not the desktop.
        D3D11_BOX box{};
        box.left = static_cast<UINT>(x0 - bounds_.x);
        box.top = static_cast<UINT>(y0 - bounds_.y);
        box.right = box.left + w;
        box.bottom = box.top + h;
        box.front = 0;
        box.back = 1;
        // Timed apart because they cost for different reasons and only one of
        // them can be designed around. The copy is queued and returns at once;
        // the map is where the CPU waits for the GPU to have actually done it,
        // along with everything else queued in front of it.
        const auto copy_started = std::chrono::steady_clock::now();
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, desktop_.Get(), 0, &box);
        const auto copy_done = std::chrono::steady_clock::now();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        const auto map_done = std::chrono::steady_clock::now();

        copy_us_ += std::chrono::duration<double, std::micro>(copy_done - copy_started).count();
        map_us_ += std::chrono::duration<double, std::micro>(map_done - copy_done).count();
        ++reads_;
        if (map_done - reported_ > std::chrono::seconds(1)) {
            reported_ = map_done;
            DZ_DEBUG("capture: %d read(s), copy %.0f us, map %.0f us, %u x %u", reads_,
                     copy_us_ / reads_, map_us_ / reads_, w, h);
            copy_us_ = 0.0;
            map_us_ = 0.0;
            reads_ = 0;
        }

        stride = static_cast<int>(w) * 4;
        out.resize(static_cast<std::size_t>(stride) * h);
        const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
        for (UINT row = 0; row < h; ++row) {
            std::copy_n(src + static_cast<std::size_t>(row) * mapped.RowPitch,
                        static_cast<std::size_t>(stride),
                        out.data() + static_cast<std::size_t>(row) * stride);
        }
        context_->Unmap(staging_.Get(), 0);
        return true;
    }

private:
    bool create_device() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                            D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL got{};
        const HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, static_cast<UINT>(std::size(levels)),
                                               D3D11_SDK_VERSION, &device_, &got, &context_);
        if (FAILED(hr)) {
            DZ_ERROR("capture: D3D11CreateDevice failed (0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    bool create_duplication() {
        ComPtr<IDXGIDevice> dxgi_device;
        if (FAILED(device_.As(&dxgi_device))) {
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgi_device->GetAdapter(&adapter))) {
            return false;
        }

        // The primary output for now. Following the guest's requested region
        // onto whichever output holds it is a later problem, and one the
        // interface already allows for through bounds().
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, &output))) {
            DZ_ERROR("capture: no output 0 on this adapter");
            return false;
        }
        if (FAILED(output.As(&output_))) {
            return false;
        }

        DXGI_OUTPUT_DESC desc{};
        output_->GetDesc(&desc);
        bounds_ = to_recti(desc.DesktopCoordinates);

        const HRESULT hr = output_->DuplicateOutput(device_.Get(), &duplication_);
        if (FAILED(hr)) {
            // E_ACCESSDENIED here usually means something else already holds
            // the duplication, or a fullscreen exclusive app owns the display.
            DZ_ERROR("capture: DuplicateOutput failed (0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }

        first_frame_ = true;
        DZ_INFO("capture: duplicating %dx%d at (%d, %d)", bounds_.w, bounds_.h, bounds_.x,
                bounds_.y);
        return true;
    }

    void recover() {
        release_frame();
        staging_.Reset();
        duplication_.Reset();
        output_.Reset();
        create_duplication();
    }

    void release_frame() {
        frame_texture_.Reset();
        if (holding_ && duplication_) {
            duplication_->ReleaseFrame();
        }
        holding_ = false;
    }

    bool read_metadata(const DXGI_OUTDUPL_FRAME_INFO& info, FrameUpdate& out) {
        if (info.TotalMetadataBufferSize == 0) {
            return false;
        }
        metadata_.resize(info.TotalMetadataBufferSize);

        UINT move_bytes = 0;
        if (FAILED(duplication_->GetFrameMoveRects(
                static_cast<UINT>(metadata_.size()),
                reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data()), &move_bytes))) {
            return false;
        }
        const auto* moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data());
        const std::size_t move_count = move_bytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
        out.moves.reserve(move_count);
        for (std::size_t i = 0; i < move_count; ++i) {
            MoveRect m;
            m.to = to_recti(moves[i].DestinationRect);
            m.to.x += bounds_.x;
            m.to.y += bounds_.y;
            m.from_x = moves[i].SourcePoint.x + bounds_.x;
            m.from_y = moves[i].SourcePoint.y + bounds_.y;
            out.moves.push_back(m);
        }

        UINT dirty_bytes = 0;
        if (FAILED(duplication_->GetFrameDirtyRects(
                static_cast<UINT>(metadata_.size() - move_bytes),
                reinterpret_cast<RECT*>(metadata_.data() + move_bytes), &dirty_bytes))) {
            return false;
        }
        const auto* dirty = reinterpret_cast<const RECT*>(metadata_.data() + move_bytes);
        const std::size_t dirty_count = dirty_bytes / sizeof(RECT);
        out.dirty.reserve(dirty_count);
        for (std::size_t i = 0; i < dirty_count; ++i) {
            core::Recti r = to_recti(dirty[i]);
            r.x += bounds_.x;
            r.y += bounds_.y;
            out.dirty.push_back(r);
        }
        return true;
    }

    // Matches whatever the duplication hands over, so CopyResource is always
    // legal. Bound as a shader resource because that is what the compute
    // encoder will need, and it costs nothing to ask for now.
    bool ensure_desktop() {
        if (desktop_) {
            return true;
        }
        D3D11_TEXTURE2D_DESC desc{};
        frame_texture_->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &desktop_))) {
            DZ_ERROR("capture: could not create the desktop copy");
            return false;
        }
        return true;
    }

    bool ensure_staging(UINT w, UINT h) {
        if (staging_ && staging_w_ >= w && staging_h_ >= h) {
            return true;
        }
        staging_.Reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) {
            return false;
        }
        staging_w_ = w;
        staging_h_ = h;
        return true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> frame_texture_;
    // Ours, refreshed on every acquire, readable at any time.
    ComPtr<ID3D11Texture2D> desktop_;
    bool desktop_valid_ = false;
    ComPtr<ID3D11Texture2D> staging_;
    UINT staging_w_ = 0;
    UINT staging_h_ = 0;

    // Rolling, reported once a second: the readback is the piece the design is
    // trying to get rid of, so what it costs and why should be visible.
    double copy_us_ = 0.0;
    double map_us_ = 0.0;
    int reads_ = 0;
    std::chrono::steady_clock::time_point reported_{};

    std::vector<std::uint8_t> metadata_;
    core::Recti bounds_{};
    std::uint64_t frame_ = 0;
    bool holding_ = false;
    bool first_frame_ = true;
};

} // namespace

std::unique_ptr<IFrameSource> make_frame_source() {
    return std::make_unique<DxgiFrameSource>();
}

} // namespace digitiz::host

#endif // _WIN32
