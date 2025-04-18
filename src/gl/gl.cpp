#include <gleditor/gl/gl.hpp> // IWYU pragma: associated
#include <iostream>  // for operator<<, basic_ostream, cerr

void GL::texSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                       GLsizei width, GLsizei height, GLenum format,
                       GLenum type, const void *pixels) {

  ::glTexSubImage2D(target, level, xoffset, yoffset, width, height, format,
                    type, pixels);
}
void GL::texSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                       GLint zoffset, GLsizei width, GLsizei height,
                       GLsizei depth, GLenum format, GLenum type,
                       const void *pixels) {

  std::cerr << "glTexSubImage3D should not be called!!!\n";
  ::glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height,
                    depth, format, type, pixels);
}

void GL::getIntegerv(GLenum pname, GLint *params) {
  ::glGetIntegerv(pname, params);
}

void GL::pixelStorei(GLenum pname, GLint param) {
  ::glPixelStorei(pname, param);
}
