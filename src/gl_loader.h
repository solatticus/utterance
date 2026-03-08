#ifndef GL_LOADER_H
#define GL_LOADER_H

#include <GL/glcorearb.h>

#define GL_FUNCS \
    X(void, GenVertexArrays, GLsizei n, GLuint *arrays) \
    X(void, BindVertexArray, GLuint array) \
    X(void, GenBuffers, GLsizei n, GLuint *buffers) \
    X(void, BindBuffer, GLenum target, GLuint buffer) \
    X(void, BufferData, GLenum target, GLsizeiptr size, const void *data, GLenum usage) \
    X(void, BufferSubData, GLenum target, GLintptr offset, GLsizeiptr size, const void *data) \
    X(void, VertexAttribPointer, GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) \
    X(void, EnableVertexAttribArray, GLuint index) \
    X(GLuint, CreateShader, GLenum type) \
    X(void, ShaderSource, GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) \
    X(void, CompileShader, GLuint shader) \
    X(GLuint, CreateProgram, void) \
    X(void, AttachShader, GLuint program, GLuint shader) \
    X(void, LinkProgram, GLuint program) \
    X(void, UseProgram, GLuint program) \
    X(GLint, GetUniformLocation, GLuint program, const GLchar *name) \
    X(void, UniformMatrix4fv, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) \
    X(void, Uniform1i, GLint location, GLint v0) \
    X(void, Uniform1f, GLint location, GLfloat v0) \
    X(void, Uniform3f, GLint location, GLfloat v0, GLfloat v1, GLfloat v2) \
    X(void, DrawArrays, GLenum mode, GLint first, GLsizei count) \
    X(void, GetShaderiv, GLuint shader, GLenum pname, GLint *params) \
    X(void, GetShaderInfoLog, GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) \
    X(void, GetProgramiv, GLuint program, GLenum pname, GLint *params) \
    X(void, GetProgramInfoLog, GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) \
    X(void, DeleteShader, GLuint shader) \
    X(void, ActiveTexture, GLenum texture) \
    X(void, GenTextures, GLsizei n, GLuint *textures) \
    X(void, BindTexture, GLenum target, GLuint texture) \
    X(void, TexImage2D, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) \
    X(void, TexParameteri, GLenum target, GLenum pname, GLint param) \
    X(void, Enable, GLenum cap) \
    X(void, Disable, GLenum cap) \
    X(void, BlendFunc, GLenum sfactor, GLenum dfactor) \
    X(void, Clear, GLbitfield mask) \
    X(void, ClearColor, GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) \
    X(void, Viewport, GLint x, GLint y, GLsizei width, GLsizei height) \
    X(void, DeleteProgram, GLuint program) \
    X(void, DeleteVertexArrays, GLsizei n, const GLuint *arrays) \
    X(void, DeleteBuffers, GLsizei n, const GLuint *buffers) \
    X(void, DeleteTextures, GLsizei n, const GLuint *textures) \
    X(void, PixelStorei, GLenum pname, GLint param) \
    X(void, TexSubImage2D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) \
    X(void, GenFramebuffers, GLsizei n, GLuint *framebuffers) \
    X(void, BindFramebuffer, GLenum target, GLuint framebuffer) \
    X(void, FramebufferTexture2D, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) \
    X(GLenum, CheckFramebufferStatus, GLenum target) \
    X(void, DeleteFramebuffers, GLsizei n, const GLuint *framebuffers) \
    X(void, Uniform2f, GLint location, GLfloat v0, GLfloat v1)

/* Declare function pointers as gl* */
#define X(ret, name, ...) extern ret (APIENTRY *gl##name)(__VA_ARGS__);
GL_FUNCS
#undef X

void gl_load_all(void);

#ifdef GL_LOADER_IMPLEMENTATION

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>

#define X(ret, name, ...) ret (APIENTRY *gl##name)(__VA_ARGS__) = NULL;
GL_FUNCS
#undef X

void gl_load_all(void) {
#define X(ret, name, ...) \
    gl##name = (ret (APIENTRY *)(__VA_ARGS__))glfwGetProcAddress("gl" #name); \
    if (!gl##name) { fprintf(stderr, "gl_loader: failed to load gl" #name "\n"); exit(1); }
    GL_FUNCS
#undef X
}

#endif /* GL_LOADER_IMPLEMENTATION */
#endif /* GL_LOADER_H */
