#ifndef MOCK_GLEDITOR_GL_GL_H
#define MOCK_GLEDITOR_GL_GL_H

#include <gmock/gmock.h>

#include "../../src/gl/GL.hpp"

class GLMock : public GL {
public:

  MOCK_METHOD(void, texSubImage2D,
              (GLenum target, GLint level, GLint xoffset, GLint yoffset,
               GLsizei width, GLsizei height, GLenum format, GLenum type,
               const void *pixels),
              (override));
  MOCK_METHOD(void, texSubImage3D,
              (GLenum target, GLint level, GLint xoffset, GLint yoffset,
               GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
               GLenum format, GLenum type, const void *pixels),
              (override));
  MOCK_METHOD(void, getIntegerv, (GLenum pname, GLint *params), (override));
  MOCK_METHOD(void, pixelStorei, (GLenum pname, GLint param), (override));
};

#endif /* MOCK_GLEDITOR_GL_GL_H */
