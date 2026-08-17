#pragma once

#include <GLES3/gl3.h>

namespace digitiz::guest {

// Returns 0 on failure; the reason is logged.
GLuint compile_program(const char* vertex_src, const char* fragment_src);

} // namespace digitiz::guest
