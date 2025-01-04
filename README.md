# gleditor


OpenGL Text Editor

[![C/C++ CI](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml)


Still a work in progress at this time.

## Building

### Prerequisites

To run and test, the following packages are needed:

* GLM
* SPDLog (temporarily removed, investigating libc++ compat)
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
apt-get install doxygen libglm-dev libspdlog-dev libpangomm-2.48-dev libsdl2-dev libsdl2-image-dev libglew-dev clang make libgmock-dev google-mock libgtest-dev
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

### Run a Test Coverage Profile

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

## Running with different Sanitizers enabled

### AddressSanitizer

```
make clean
make sanitize/address
./gleditor
```

### ThreadSanitizer

```
make clean
make sanitize/thread
./gleditor
```

### MemorySanitizer

```
make clean
make sanitize/memory
./gleditor
```

## Usage

### Running

To run the application just do:

```
./gleditor
```

You will be greeted with a black screen at first. Press `n` to create a new "page"--just a blank white rectangle for now. 
Each new page will be created below the last one. 

### Movement and Commands

To move, use the following keys:


            Keys        How they move
             e               up
          s d/D f   left  zoomout/in  right
             c              down

The following keys are currently bound to actions other than movement:

| Key | Action |
| --- | ------ |
| n   | Create a new page |
| r   | Reset view back to start |
| q   | Quit the application |
| g   | Increment fov by 1 (max 360) |
| G   | Decrement fov by 1 (min 1) |

