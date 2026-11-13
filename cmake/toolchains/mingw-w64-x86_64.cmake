# Cross-compile Windows x64 binaries with MinGW-w64 GCC from Linux or macOS:
#   cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
# Needs the "posix" threading variant (Debian/Ubuntu: apt install mingw-w64).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(MINGW_CXX NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ REQUIRED)
find_program(MINGW_RC NAMES x86_64-w64-mingw32-windres)
set(CMAKE_CXX_COMPILER ${MINGW_CXX})
if(MINGW_RC)
    set(CMAKE_RC_COMPILER ${MINGW_RC})
endif()

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Self-contained .exe files: no libstdc++/libgcc/winpthread DLLs to ship.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
