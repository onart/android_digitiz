#include "text/TextRenderer.hpp"

#include <cmath>
#include <vector>

#include <game-activity/GameActivity.h>

#include <digitiz/core/log.hpp>

#include "render/Shader.hpp"

namespace digitiz::guest {

namespace {

// Beyond this the cache is cleared wholesale rather than evicted one at a
// time. Milestone 1 uses a handful of fixed labels; a build that starts
// churning strings will say so in the log rather than leaking textures.
constexpr std::size_t kMaxCachedStrings = 192;

constexpr const char* kVertexSrc = R"(#version 300 es
uniform highp vec2 uViewport;
uniform highp vec4 uRect;   // x, y, w, h  (top-left origin, surface px)

out highp vec2 vUv;

void main() {
    vec2 uv = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = uv;

    vec2 surface = uRect.xy + uv * uRect.zw;
    vec2 ndc = vec2(surface.x / uViewport.x * 2.0 - 1.0,
                    1.0 - surface.y / uViewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

constexpr const char* kFragmentSrc = R"(#version 300 es
precision mediump float;

uniform sampler2D uTexture;
uniform vec4 uColor;

in highp vec2 vUv;
out vec4 fragColor;

void main() {
    // The glyph texture holds coverage only; the colour comes from the caller.
    float coverage = texture(uTexture, vUv).r;
    if (coverage <= 0.0) {
        discard;
    }
    fragColor = vec4(uColor.rgb, uColor.a * coverage);
}
)";

std::string cache_key(std::string_view text, float size_px, bool bold) {
    // Rounded to whole pixels: sizes differing by a fraction would otherwise
    // each get their own texture for no visible gain.
    std::string key;
    key.reserve(text.size() + 8);
    key += std::to_string(static_cast<int>(std::lround(size_px)));
    key += bold ? "b|" : "n|";
    key.append(text);
    return key;
}

} // namespace

TextRenderer::~TextRenderer() {
    shutdown();
}

bool TextRenderer::init(GameActivity* activity) {
    if (activity == nullptr || activity->vm == nullptr) {
        DZ_ERROR("text: no activity or VM");
        return false;
    }
    vm_ = activity->vm;

    // android_main runs on its own thread, which the VM does not know about.
    if (vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) != JNI_OK) {
        if (vm_->AttachCurrentThread(&env_, nullptr) != JNI_OK) {
            DZ_ERROR("text: could not attach the render thread to the VM");
            return false;
        }
        attached_ = true;
    }

    activity_ = env_->NewGlobalRef(activity->javaGameActivity);
    jclass local_class = env_->GetObjectClass(activity_);
    activity_class_ = static_cast<jclass>(env_->NewGlobalRef(local_class));
    env_->DeleteLocalRef(local_class);

    rasterize_method_ =
        env_->GetStaticMethodID(activity_class_, "rasterizeText", "(Ljava/lang/String;FZ)[I");
    localized_method_ = env_->GetMethodID(activity_class_, "localizedString",
                                          "(Ljava/lang/String;)Ljava/lang/String;");

    if (rasterize_method_ == nullptr || localized_method_ == nullptr) {
        env_->ExceptionClear();
        DZ_ERROR("text: MainActivity is missing the rasterizer hooks");
        return false;
    }

    return init_gl();
}

bool TextRenderer::init_gl() {
    if (program_ != 0) {
        return true;
    }

    program_ = compile_program(kVertexSrc, kFragmentSrc);
    if (program_ == 0) {
        return false;
    }
    glGenVertexArrays(1, &vao_);

    u_viewport_ = glGetUniformLocation(program_, "uViewport");
    u_rect_ = glGetUniformLocation(program_, "uRect");
    u_color_ = glGetUniformLocation(program_, "uColor");
    u_texture_ = glGetUniformLocation(program_, "uTexture");

    DZ_INFO("text renderer ready");
    return true;
}

void TextRenderer::release_gl() {
    for (auto& [key, entry] : cache_) {
        if (entry.texture != 0) {
            glDeleteTextures(1, &entry.texture);
        }
    }
    cache_.clear();

    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

void TextRenderer::shutdown() {
    release_gl();

    if (env_ != nullptr) {
        if (activity_class_ != nullptr) {
            env_->DeleteGlobalRef(activity_class_);
            activity_class_ = nullptr;
        }
        if (activity_ != nullptr) {
            env_->DeleteGlobalRef(activity_);
            activity_ = nullptr;
        }
    }
    if (attached_ && vm_ != nullptr) {
        vm_->DetachCurrentThread();
        attached_ = false;
    }
    env_ = nullptr;
    vm_ = nullptr;
}

std::string TextRenderer::localized(const char* resource_name) {
    if (env_ == nullptr || localized_method_ == nullptr) {
        return resource_name;
    }

    jstring name = env_->NewStringUTF(resource_name);
    auto value = static_cast<jstring>(env_->CallObjectMethod(activity_, localized_method_, name));
    env_->DeleteLocalRef(name);

    if (env_->ExceptionCheck()) {
        env_->ExceptionClear();
        return resource_name;
    }
    if (value == nullptr) {
        return resource_name;
    }

    const char* utf8 = env_->GetStringUTFChars(value, nullptr);
    std::string out = utf8 != nullptr ? utf8 : resource_name;
    if (utf8 != nullptr) {
        env_->ReleaseStringUTFChars(value, utf8);
    }
    env_->DeleteLocalRef(value);
    return out;
}

TextRenderer::Entry TextRenderer::rasterize(const std::string& text, float size_px, bool bold) {
    Entry entry;
    if (env_ == nullptr || rasterize_method_ == nullptr) {
        return entry;
    }

    jstring jtext = env_->NewStringUTF(text.c_str());
    auto packed = static_cast<jintArray>(env_->CallStaticObjectMethod(
        activity_class_, rasterize_method_, jtext, size_px, static_cast<jboolean>(bold)));
    env_->DeleteLocalRef(jtext);

    if (env_->ExceptionCheck()) {
        env_->ExceptionClear();
        DZ_WARN("text: rasterizing \"%s\" threw", text.c_str());
        return entry;
    }
    if (packed == nullptr) {
        return entry;
    }

    const jsize count = env_->GetArrayLength(packed);
    jint* data = env_->GetIntArrayElements(packed, nullptr);
    if (data == nullptr || count < 3) {
        env_->DeleteLocalRef(packed);
        return entry;
    }

    const int width = data[0];
    const int height = data[1];
    const int baseline = data[2];

    if (width > 0 && height > 0 && count >= 3 + width * height) {
        // Only the alpha channel matters, so the texture is one byte per pixel
        // instead of four.
        std::vector<std::uint8_t> coverage(static_cast<std::size_t>(width) *
                                           static_cast<std::size_t>(height));
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            coverage[i] = static_cast<std::uint8_t>(
                (static_cast<std::uint32_t>(data[3 + i]) >> 24) & 0xFFu);
        }

        glGenTextures(1, &entry.texture);
        glBindTexture(GL_TEXTURE_2D, entry.texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE,
                     coverage.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        entry.width = width;
        entry.height = height;
        entry.baseline = baseline;
    }

    env_->ReleaseIntArrayElements(packed, data, JNI_ABORT);
    env_->DeleteLocalRef(packed);
    return entry;
}

const TextRenderer::Entry* TextRenderer::entry_for(std::string_view text, float size_px,
                                                   bool bold) {
    if (text.empty()) {
        return nullptr;
    }

    const std::string key = cache_key(text, size_px, bold);
    if (auto it = cache_.find(key); it != cache_.end()) {
        return &it->second;
    }

    if (cache_.size() >= kMaxCachedStrings) {
        DZ_WARN("text: cache full at %zu strings, clearing", cache_.size());
        for (auto& [_, entry] : cache_) {
            if (entry.texture != 0) {
                glDeleteTextures(1, &entry.texture);
            }
        }
        cache_.clear();
    }

    Entry entry = rasterize(std::string(text), size_px, bold);
    if (entry.texture == 0) {
        return nullptr;
    }
    auto [it, _] = cache_.emplace(key, entry);
    return &it->second;
}

float TextRenderer::measure(std::string_view text, float size_px, bool bold) {
    const Entry* entry = entry_for(text, size_px, bold);
    return entry != nullptr ? static_cast<float>(entry->width) : 0.0f;
}

float TextRenderer::line_height(float size_px, bool bold) {
    // Any string gives the same line box; a digit avoids depending on locale.
    const Entry* entry = entry_for("0", size_px, bold);
    return entry != nullptr ? static_cast<float>(entry->height) : size_px;
}

void TextRenderer::begin(int surface_w, int surface_h) {
    if (program_ == 0) {
        return;
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(program_);
    glBindVertexArray(vao_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(u_texture_, 0);
    glUniform2f(u_viewport_, static_cast<float>(surface_w), static_cast<float>(surface_h));
}

void TextRenderer::end() {
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

float TextRenderer::draw(std::string_view text, float x, float y, float size_px, Color color,
                         TextAlign align, bool bold) {
    const Entry* entry = entry_for(text, size_px, bold);
    if (entry == nullptr || program_ == 0) {
        return 0.0f;
    }

    const auto w = static_cast<float>(entry->width);
    const auto h = static_cast<float>(entry->height);

    float left = x;
    if (align == TextAlign::Center) {
        left = x - w * 0.5f;
    } else if (align == TextAlign::Right) {
        left = x - w;
    }
    // Snap to whole pixels: half-pixel placement makes small text look soft.
    left = std::round(left);

    glBindTexture(GL_TEXTURE_2D, entry->texture);
    glUniform4f(u_rect_, left, std::round(y), w, h);
    glUniform4f(u_color_, color.r, color.g, color.b, color.a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return w;
}

} // namespace digitiz::guest
