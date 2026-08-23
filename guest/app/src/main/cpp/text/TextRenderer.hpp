#pragma once

// Text for the GL UI.
//
// Rasterization is done by Android's Paint through JNI rather than by a font
// library here. That costs a small JNI surface but means the platform's font
// stack handles the work: Korean and anything else render correctly, and the
// APK does not have to carry a CJK font that would weigh several megabytes.
//
// Each distinct string+size is rasterized once and kept as a texture, so the
// per-frame cost is a textured quad. Only the alpha channel is stored; colour
// comes from the draw call.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include <GLES3/gl3.h>
#include <jni.h>

#include "render/UiRenderer.hpp"

struct GameActivity;

namespace digitiz::guest {

enum class TextAlign : std::uint8_t { Left, Center, Right };

class TextRenderer {
public:
    ~TextRenderer();

    // `activity` supplies the VM and the activity object. The calling thread is
    // attached to the VM and stays attached until shutdown().
    bool init(GameActivity* activity);
    void shutdown();

    // Rebuilds only the GL side. The surface can go away and come back while
    // the process lives, and the JNI attachment outlives that.
    bool init_gl();
    void release_gl();

    bool attached() const noexcept { return env_ != nullptr; }

    bool ready() const noexcept { return program_ != 0; }

    void begin(int surface_w, int surface_h);
    void end();

    // Restricts drawing to a rectangle, in surface pixels. Needed by anything
    // that scrolls: a partially scrolled item has to be cut off at the edge of
    // its container instead of spilling across the screen. Not nestable --
    // set it, draw, clear it.
    void set_clip(Rect rect);
    void clear_clip();

    // `y` is the top of the line. Returns the advance width in pixels.
    float draw(std::string_view text, float x, float y, float size_px, Color color,
               TextAlign align = TextAlign::Left, bool bold = false);

    float measure(std::string_view text, float size_px, bool bold = false);
    float line_height(float size_px, bool bold = false);

    // Reads a string resource by name so native labels share strings.xml.
    std::string localized(const char* resource_name);

private:
    struct Entry {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
        int baseline = 0;
    };

    const Entry* entry_for(std::string_view text, float size_px, bool bold);
    Entry rasterize(const std::string& text, float size_px, bool bold);
    JNIEnv* env() const noexcept { return env_; }

    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
    jobject activity_ = nullptr;   // global ref
    jclass activity_class_ = nullptr; // global ref
    jmethodID rasterize_method_ = nullptr;
    jmethodID localized_method_ = nullptr;

    GLuint program_ = 0;
    GLuint vao_ = 0;
    int surface_h_ = 0;
    GLint u_viewport_ = -1;
    GLint u_rect_ = -1;
    GLint u_color_ = -1;
    GLint u_texture_ = -1;

    std::unordered_map<std::string, Entry> cache_;
};

} // namespace digitiz::guest
