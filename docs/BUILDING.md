# Building poll-dancer

## Prerequisites

### All Platforms
- CMake 3.14 or later
- C11 compiler (GCC 4.9+, Clang 3.4+, MSVC 2015+)

### Linux
- GCC or Clang
- pthreads library

### macOS
- Xcode Command Line Tools or Clang
- pthreads library (included in system)

### Windows
- Visual Studio 2015 or later, or MinGW-w64
- Windows SDK

## Quick Build

### Linux/macOS

```bash
mkdir build && cd build
cmake ..
make
sudo make install
```

### Windows (Visual Studio)

```cmd
mkdir build && cd build
cmake ..
cmake --build . --config Release
cmake --install . --config Release
```

### Windows (MinGW)

```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
mingw32-make
mingw32-make install
```

## Build Options

### POLL_DANCER_BUILD_TESTS
Build the test suite. Default: `ON`

```bash
cmake -DPOLL_DANCER_BUILD_TESTS=OFF ..
```

### POLL_DANCER_BUILD_EXAMPLES
Build example programs. Default: `OFF`

```bash
cmake -DPOLL_DANCER_BUILD_EXAMPLES=ON ..
```

### POLL_DANCER_BUILD_SHARED
Build shared library instead of static. Default: `OFF`

```bash
cmake -DPOLL_DANCER_BUILD_SHARED=ON ..
```

### POLL_DANCER_THREAD_SAFE
Enable thread safety. Default: `ON`

```bash
cmake -DPOLL_DANCER_THREAD_SAFE=OFF ..
```

## Building Tests

```bash
mkdir build && cd build
cmake -DPOLL_DANCER_BUILD_TESTS=ON ..
make
ctest --output-on-failure
```

## Building Examples

```bash
mkdir build && cd build
cmake -DPOLL_DANCER_BUILD_EXAMPLES=ON ..
make
```

Examples will be built in `build/examples/`:
- `simple_server` - TCP echo server
- `timer_example` - Timer demonstration

## Installation

### Default Installation

```bash
sudo make install
```

Installs to:
- `/usr/local/include/poll-dancer/` - Headers
- `/usr/local/lib/libpoll_dancer.a` - Static library
- `/usr/local/lib/cmake/poll-dancer/` - CMake config files

### Custom Installation Prefix

```bash
cmake -DCMAKE_INSTALL_PREFIX=/path/to/install ..
make
make install
```

## Using in Your Project

### CMake (find_package)

```cmake
find_package(poll-dancer REQUIRED)
target_link_libraries(your_target poll_dancer::poll_dancer)
```

### CMake (subdirectory)

```cmake
add_subdirectory(poll-dancer)
target_link_libraries(your_target poll_dancer)
```

### Makefile

```makefile
CFLAGS += -I/usr/local/include
LDFLAGS += -L/usr/local/lib -lpoll_dancer -pthread
```

### Visual Studio

1. Add `/path/to/poll-dancer/include` to include directories
2. Add `/path/to/poll-dancer/lib` to library directories
3. Link with `poll_dancer.lib` and `ws2_32.lib` (Windows only)

## Platform-Specific Notes

### Linux
- Requires glibc 2.17+ for `epoll_create1()`
- Link with `-pthread` if using thread safety

### macOS
- Works on macOS 10.12+
- Link with `-pthread` if using thread safety

### Windows
- Link with `ws2_32.lib` and `mswsock.lib`
- Call `WSAStartup()` before using the library
- Call `WSACleanup()` before process exit

## Cross-Compiling

### For Windows from Linux

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/windows-toolchain.cmake ..
make
```

### For ARM from x86

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/arm-toolchain.cmake ..
make
```

## Debug Builds

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```

## Release Builds

```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

## Package Config

poll-dancer provides a CMake package config file for easy integration:

```cmake
find_package(poll-dancer 0.1.0 REQUIRED)
target_link_libraries(myapp poll_dancer::poll_dancer)
```

This automatically:
- Adds the correct include directories
- Links the correct library
- Sets up compile definitions
- Handles platform-specific dependencies

## Build Troubleshooting

### CMake not found
Install CMake 3.14+ from https://cmake.org/

### pthreads not found (Linux/macOS)
Install pthreads development package:
- Ubuntu/Debian: `sudo apt-get install pthreads-dev`
- Fedora/RHEL: `sudo dnf install pthreads-devel`

### Winsock errors (Windows)
- Link with `ws2_32.lib` and `mswsock.lib`
- Call `WSAStartup(MAKEWORD(2, 2), &wsaData)` before using

### Linker errors
- Ensure you're linking with the correct library name
- On Windows, use `poll_dancer.lib` not `libpoll_dancer.a`

### Platform detection fails
The library only supports:
- Linux (epoll)
- BSD/macOS (kqueue)
- Windows (IOCP)

Other platforms are not supported.

## CI Integration

### GitHub Actions

```yaml
- name: Build
  run: |
    mkdir build && cd build
    cmake -DPOLL_DANCER_BUILD_TESTS=ON ..
    make
    ctest --output-on-failure
```

### Travis CI

```yaml
script:
  - mkdir build && cd build
  - cmake -DPOLL_DANCER_BUILD_TESTS=ON ..
  - make
  - ctest --output-on-failure
```

### GitLab CI

```yaml
build:
  script:
    - mkdir build && cd build
    - cmake -DPOLL_DANCER_BUILD_TESTS=ON ..
    - make
    - ctest --output-on-failure
```

## Distribution

### Creating a Release Package

```bash
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make package
```

This creates platform-specific packages:
- Linux: `.tar.gz` archive
- macOS: `.dmg` or `.tar.gz`
- Windows: `.zip` archive