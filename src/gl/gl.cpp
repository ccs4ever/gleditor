#include <gleditor/gl/gl.hpp> // IWYU pragma: associated

void GL::texSubImage2D(const GLenum target, const GLint level,
                       const GLint xoffset, const GLint yoffset,
                       const GLsizei width, const GLsizei height,
                       const GLenum format, const GLenum type,
                       const void *pixels) {

  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type,
                  pixels);
}
void GL::texSubImage3D(const GLenum target, const GLint level,
                       const GLint xoffset, const GLint yoffset,
                       const GLint zoffset, const GLsizei width,
                       const GLsizei height, const GLsizei depth,
                       const GLenum format, const GLenum type,
                       const void *pixels) {
  glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height,
                  depth, format, type, pixels);
}

void GL::getIntegerv(const GLenum pname, GLint * const params) {
  glGetIntegerv(pname, params);
}

void GL::pixelStorei(const GLenum pname, const GLint param) {
  glPixelStorei(pname, param);
}
