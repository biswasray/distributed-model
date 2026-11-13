cmake_minimum_required(VERSION 3.24)

project(xgi
    VERSION 0.2.0
    DESCRIPTION "Qwen Cluster: XGI (coordinator + router) and XI (node agent)"
    LANGUAGES CXX)

# Windows, Linux and macOS with MSVC, GCC, Clang (incl. clang-cl) or MinGW-w64. See adr/ADR-008.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    # std::format/to_chars for floating point need macOS 13.3+ in libc++.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version" FORCE)
endif()

option(XGI_BUILD_TESTS "Build unit tests, mock llama.cpp binaries and the e2e test" ON)
option(XGI_STATIC_RUNTIME "Link the C++ runtime statically where the toolchain supports it" ON)
option(XGI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Static C++ runtime: copy xi/xgi to a node without installing a runtime (MSVC /MT, GCC/MinGW
# -static-libstdc++). Apple and libc++ toolchains keep the system runtime.
if(XGI_STATIC_RUNTIME AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)
include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------- warnings / platform flags
add_library(xgi_build_flags INTERFACE)
if(MSVC)
    target_compile_options(xgi_build_flags INTERFACE /W4 /permissive- /utf-8 /EHsc /bigobj /wd4251 /wd4275
        $<$<BOOL:${XGI_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(xgi_build_flags INTERFACE -Wall -Wextra -Wno-unused-parameter
        $<$<BOOL:${XGI_WARNINGS_AS_ERRORS}>:-Werror>)
    if(MINGW)
        target_compile_options(xgi_build_flags INTERFACE -Wa,-mbig-obj)
    endif()
    if(XGI_STATIC_RUNTIME AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(xgi_build_flags INTERFACE -static-libstdc++ -static-libgcc)
    endif()
endif()
if(WIN32)
    target_compile_definitions(xgi_build_flags INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

# ---------------------------------------------------------------- common
set(XGI_SYSINFO_SRC)
if(WIN32)
    set(XGI_SYSINFO_SRC src/common/sysinfo_windows.cpp)
elseif(APPLE)
    set(XGI_SYSINFO_SRC src/common/sysinfo_macos.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(XGI_SYSINFO_SRC src/common/sysinfo_linux.cpp)
else()
    message(FATAL_ERROR "Unsupported OS ${CMAKE_SYSTEM_NAME}: add src/common/sysinfo_<os>.cpp")
endif()

add_library(xgi_common STATIC
    src/common/cli.cpp
    src/common/config.cpp
    src/common/http_util.cpp
    src/common/log.cpp
    src/common/net.cpp
    src/common/shutdown.cpp
    src/common/util.cpp
    ${XGI_SYSINFO_SRC}
)
target_include_directories(xgi_common PUBLIC src)
target_compile_definitions(xgi_common PUBLIC XGI_VERSION="${PROJECT_VERSION}")
target_link_libraries(xgi_common PUBLIC xgi_build_flags
    httplib::httplib nlohmann_json::nlohmann_json yaml-cpp Threads::Threads)
if(WIN32)
    target_link_libraries(xgi_common PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------- XI (node agent)
if(WIN32)
    set(XI_PROCESS_SRC src/xi/process_windows.cpp)
else()
    set(XI_PROCESS_SRC src/xi/process_posix.cpp)
endif()
add_library(xi_lib STATIC
    src/xi/agent.cpp
    src/xi/commands.cpp
    src/xi/process.cpp
    ${XI_PROCESS_SRC}
)
target_link_libraries(xi_lib PUBLIC xgi_common)

add_executable(xi src/xi/main.cpp)
target_link_libraries(xi PRIVATE xi_lib)

# ---------------------------------------------------------------- XGI (coordinator + router)
add_library(xgi_lib STATIC
    src/xgi/coordinator.cpp
    src/xgi/placement.cpp
    src/xgi/router.cpp
    src/xgi/scheduling.cpp
)
target_link_libraries(xgi_lib PUBLIC xgi_common)

add_executable(xgi src/xgi/main.cpp)
target_link_libraries(xgi PRIVATE xgi_lib)

# ---------------------------------------------------------------- tests
if(XGI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- install / package
install(TARGETS xi xgi RUNTIME DESTINATION bin)
install(FILES examples/cluster.yaml DESTINATION .)
install(FILES scripts/install-xi-task.ps1 scripts/install-xi-service.sh DESTINATION scripts)
