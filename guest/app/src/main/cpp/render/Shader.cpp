#include "render/Shader.hpp"

#include <string>
#include <vector>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

GLuint compile_stage(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) {
        return shader;
    }

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> log(static_cast<std::size_t>(length > 0 ? length : 1), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    DZ_ERROR("%s shader failed to compile: %s",
             type == GL_VERTEX_SHADER ? "vertex" : "fragment", log.data());

    glDeleteShader(shader);
    return 0;
}

} // namespace

GLuint compile_program(const char* vertex_src, const char* fragment_src) {
    const GLuint vertex = compile_stage(GL_VERTEX_SHADER, vertex_src);
    if (vertex == 0) {
        return 0;
    }
    const GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_src);
    if (fragment == 0) {
        glDeleteShader(vertex);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    // The program keeps its own reference once linked.
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok == GL_TRUE) {
        return program;
    }

    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> log(static_cast<std::size_t>(length > 0 ? length : 1), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    DZ_ERROR("program failed to link: %s", log.data());

    glDeleteProgram(program);
    return 0;
}

} // namespace digitiz::guest
