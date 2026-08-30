# The Android native build: everything the Makefile's `lib` and `gleditor`
# targets produce, as one shared library instead of two, since an APK has no
# equivalent of the desktop's install prefix for a library other programs on
# the same machine might also want. Nothing else in this repository is built
# through CMake -- see packaging/android/app/build.gradle, which is what
# actually invokes this, and packaging/android/README.md for why it lives
# here.
#
# At the repository root and not under packaging/android/app/ (which is where
# it lived at first): add_subdirectory(thirdparty/SDL) below needs SDL's
# source to be a real descendant of wherever this file sits, since AGP's
# CMake File API consumer fails to parse the result -- "There was an error
# parsing CMake File API result", with no further detail -- when a
# subdirectory it walks resolves to a path outside this file's own tree.
# thirdparty/SDL and packaging/android/app have no common ancestor closer
# than the repository root, so this is as deep as the file can live and still
# have both.
cmake_minimum_required(VERSION 3.22.1)
project(gleditor_android CXX)

if(NOT ANDROID)
  message(FATAL_ERROR "This CMakeLists.txt is built through packaging/android's Gradle project, which sets ANDROID; it is not meant to be configured directly, and nothing else in this repository builds through CMake.")
endif()

set(GLEDITOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# -- version -------------------------------------------------------------
#
# Same rule the Makefile applies to GLEDITOR_VERSION: a tag if HEAD has one,
# VERSION otherwise. Kept in the build rather than trusted to app/build.gradle
# because config.h is what apps/gleditor/main.cpp actually reads.
set(GLEDITOR_VERSION "")
if(EXISTS "${GLEDITOR_ROOT}/VERSION")
  file(STRINGS "${GLEDITOR_ROOT}/VERSION" GLEDITOR_VERSION LIMIT_COUNT 1)
endif()
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*.[0-9]*.[0-9]*" HEAD
    WORKING_DIRECTORY "${GLEDITOR_ROOT}"
    OUTPUT_VARIABLE GLEDITOR_VERSION_DESCRIBED
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GLEDITOR_VERSION_DESCRIBE_RESULT)
  if(GLEDITOR_VERSION_DESCRIBE_RESULT EQUAL 0 AND NOT GLEDITOR_VERSION_DESCRIBED STREQUAL "")
    string(REGEX REPLACE "^v" "" GLEDITOR_VERSION_DESCRIBED "${GLEDITOR_VERSION_DESCRIBED}")
    string(REPLACE "-" "." GLEDITOR_VERSION_DESCRIBED "${GLEDITOR_VERSION_DESCRIBED}")
    set(GLEDITOR_VERSION "${GLEDITOR_VERSION_DESCRIBED}")
  endif()
endif()

file(READ "${GLEDITOR_ROOT}/src/config.h.in" GLEDITOR_CONFIG_H_IN)
string(REPLACE "@@VERS@@" "${GLEDITOR_VERSION}" GLEDITOR_CONFIG_H_CONTENT "${GLEDITOR_CONFIG_H_IN}")
file(WRITE "${CMAKE_BINARY_DIR}/generated/src/config.h" "${GLEDITOR_CONFIG_H_CONTENT}")

# -- sources ---------------------------------------------------------------
#
# The same tree the Makefile's LIB_SRCS walks (thirdparty/Choreograph/src and
# src/, recursively), minus the AccessKit backend: accessibility is compiled
# out for Android rather than cross-built, so src/a11y/platform_none.cpp -- a
# no-op that answers the same interface -- is what actually links, same as a
# desktop build with GLEDITOR_ENABLE_A11Y=0. Vulkan's src/render/vulkan/*.cpp
# is always included, unlike the Makefile's conditional, since this build
# exists specifically to carry that backend.
file(GLOB_RECURSE GLEDITOR_LIB_SOURCES CONFIGURE_DEPENDS "${GLEDITOR_ROOT}/src/*.cpp")
list(FILTER GLEDITOR_LIB_SOURCES EXCLUDE REGEX "/src/a11y/platform_accesskit\\.cpp$")
file(GLOB_RECURSE GLEDITOR_CHOREOGRAPH_SOURCES CONFIGURE_DEPENDS "${GLEDITOR_ROOT}/thirdparty/Choreograph/src/*.cpp")

add_library(main SHARED
  ${GLEDITOR_LIB_SOURCES}
  ${GLEDITOR_CHOREOGRAPH_SOURCES}
  "${GLEDITOR_ROOT}/apps/gleditor/main.cpp"
)

target_compile_options(main PRIVATE -std=c++23 -Wall -Wextra)

target_include_directories(main PRIVATE
  "${CMAKE_BINARY_DIR}/generated/src"
  "${GLEDITOR_ROOT}/include"
  "${GLEDITOR_ROOT}/apps"
  "${GLEDITOR_ROOT}/thirdparty/Choreograph/src"
  "${GLEDITOR_ROOT}/thirdparty/argparse/include"
  # GL/glcorearb.h and KHR/khrplatform.h, for the typedefs and enum values
  # gl_api.hpp reads. The NDK has GLES headers but not this one, the same gap
  # pkg-config's `gl` package fills in on desktop Linux; see the Makefile's
  # own comment on GL_CFLAGS for why this is vendored rather than found.
  "${GLEDITOR_ROOT}/thirdparty/opengl-registry"
)

target_compile_definitions(main PRIVATE
  GLEDITOR_SDL_MAJOR=3
  GLEDITOR_ENABLE_VULKAN=1
  GLM_ENABLE_EXPERIMENTAL
  # No command line exists to pass --backend on, and desktop GL is not a
  # backend Android has; see src/app.cpp for where this is read.
  GLEDITOR_DEFAULT_BACKEND="opengles"
)

# -- SDL ---------------------------------------------------------------
#
# Built from source into libSDL3.so: there is no system SDL on Android to
# find the way desktop does through pkg-config. The cache variables below are
# SDL's own build options (see thirdparty/SDL/CMakeLists.txt), set before
# add_subdirectory so they take effect for its build rather than a prior one.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLEDITOR_ROOT}/thirdparty/SDL" "${CMAKE_BINARY_DIR}/SDL3")

# -- freetype/harfbuzz/fribidi/unibreak/fontconfig, glm ------------------------
#
# All from vcpkg (see packaging/android/vcpkg.json), statically linked -- the
# arm64-android and x64-android triplets set VCPKG_LIBRARY_LINKAGE static, so
# this is one fewer set of .so files Gradle would otherwise need telling to
# package. Found through pkg-config, same as the desktop Makefile, since that
# is the form vcpkg's own meson/cmake ports install; vcpkg's toolchain file
# points pkg-config at the triplet's install tree on its own. libunibreak is
# the exception -- its vcpkg port installs a CMake package config instead of a
# .pc file, so it is found through find_package() and linked as an imported
# target rather than folded into TEXT_DEPS.
#
# The _STATIC_ variables, not the plain ones: vcpkg's triplets build
# everything static, and only `pkg-config --static` walks the full transitive
# dependency graph and adds the system libraries (-pthread and the like) a static link needs.
find_package(PkgConfig REQUIRED)
pkg_check_modules(TEXT_DEPS REQUIRED freetype2 harfbuzz fribidi fontconfig)
find_package(libunibreak CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

# libvulkan.so is the NDK's own stub for the platform loader, present from
# API 24 on; nothing here is vcpkg's to provide.
find_library(GLEDITOR_VULKAN_LIB vulkan REQUIRED)

target_include_directories(main PRIVATE ${TEXT_DEPS_STATIC_INCLUDE_DIRS})
target_compile_options(main PRIVATE ${TEXT_DEPS_STATIC_CFLAGS_OTHER})
target_link_directories(main PRIVATE ${TEXT_DEPS_STATIC_LIBRARY_DIRS})

target_link_libraries(main PRIVATE
  SDL3::SDL3
  ${TEXT_DEPS_STATIC_LIBRARIES}
  ${TEXT_DEPS_STATIC_LDFLAGS_OTHER}
  libunibreak::libunibreak
  glm::glm
  ${GLEDITOR_VULKAN_LIB}
)
