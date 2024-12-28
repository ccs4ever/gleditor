#ifndef GLEDITOR_GL_GL_H
#define GLEDITOR_GL_GL_H

#include <GL/glew.h>
#include "../log.hpp"

/* Interface for mocking and portability */
class GL : public Loggable {
public:
  ~GL() override = default;
  /*GL() = default;
  GL(const GL&) = default;
  GL& operator =(const GL&) = default;*/

  virtual void texSubImage2D(GLenum target, GLint level, GLint xoffset,
                               GLint yoffset, GLsizei width, GLsizei height,
                               GLenum format, GLenum type, const void *pixels);

  virtual void texSubImage3D(GLenum target, GLint level, GLint xoffset,
                               GLint yoffset, GLint zoffset, GLsizei width,
                               GLsizei height, GLsizei depth, GLenum format,
                               GLenum type, const void *pixels);

  virtual void getIntegerv(GLenum pname, GLint *params);

  virtual void pixelStorei(GLenum pname, GLint param);
};

#endif /* GLEDITOR_GL_GL_H */
