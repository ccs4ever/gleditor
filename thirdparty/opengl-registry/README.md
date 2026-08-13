Two files from the Khronos OpenGL/OpenGL ES XML API Registry
(https://github.com/KhronosGroup/OpenGL-Registry and
https://github.com/KhronosGroup/EGL-Registry), vendored verbatim rather than
pulled in as a submodule because two header files do not need one:

- `GL/glcorearb.h` -- typedefs and enum values for OpenGL 3.3 core and later,
  used by `include/gleditor/render/gl/gl_api.hpp` for its entry-point table.
  Declares no prototypes and creates no link-time dependency; the actual
  entry points are resolved through SDL at run time.
- `KHR/khrplatform.h` -- included by glcorearb.h itself, for `khronos_float_t`
  and friends.

Both are MIT-licensed by the Khronos Group; see the copyright notice at the
top of each file.

This exists because macOS has nowhere else for the build to get them from:
Linux distributions ship these headers as part of their GL loader's -dev
package (found here through `gl.pc`), and MinGW puts them where the compiler
already looks -- but macOS's own OpenGL.framework predates this registry
layout and was never going to grow it. See the `GL_CFLAGS` fallback in the
top-level Makefile.
