# gleditor


OpenGL Text Editor

[![C/C++ CI](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml)


Still a work in progress at this time.

## Building

### Prerequisites

To run and test, the following packages are needed:

* SPDLog
* pangomm/cairomm
* SDL2
* SDL2\_image
* GLew
* clang or gcc (defaults to using clang)
* GNU Make
* Google Mock (gmock)
* Google Test (gtest)

To build documentation, you will need:

* Doxygen

On Ubuntu run:

```
apt-get install doxygen libspdlog-dev libpangomm-2.48-dev libsdl2-dev libsdl2-image-dev libglew-dev clang make libgmock-dev google-mock libgtest-dev
```

### Build executable

```
make gleditor
```

### Build Test Executable

```
make gleditor_test
```

### Run Tests

```
make test
```

### Generate Docs

```
make doc
```

### Run a Coverage Profile

```
make profile
```

### Clean a Previous Build

```
make clean
```

### Build compile\_commands.json

To build compile\_commands.json, which is used by clangd and other tools to show warnings, formatting, and semantic highlighting in LSP-enabled editors like Neovim, run:

```
make
```

Or

```
make compile_commands.json
```

