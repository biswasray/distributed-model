cmake_minimum_required(VERSION 3.24)

project(xgi
    VERSION 0.1.0
    DESCRIPTION "Qwen Cluster: XGI (coordinator + router) and XI (node agent)"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "XGI/XI are Windows-native (see adr/ADR-007). Build with MSVC on Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: xi.exe / xgi.exe can be copied to 50 PCs without installing the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(XGI_BUILD_TESTS "Build unit tests, mock llama.cpp binaries and the e2e test" ON)

include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------- common
add_library(xgi_common STATIC
    src/common/cli.cpp
    src/common/config.cpp
    src/common/http_util.cpp
    src/common/log.cpp
    src/common/sysinfo.cpp
    src/common/util.cpp
)
target_include_directories(xgi_common PUBLIC src)
target_compile_definitions(xgi_common PUBLIC
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    XGI_VERSION="${PROJECT_VERSION}")
target_compile_options(xgi_common PUBLIC /W4 /permissive- /utf-8 /EHsc /bigobj /wd4251 /wd4275)
target_link_libraries(xgi_common PUBLIC
    httplib::httplib nlohmann_json::nlohmann_json yaml-cpp
    ws2_32 iphlpapi)

# ---------------------------------------------------------------- XI (node agent)
add_library(xi_lib STATIC
    src/xi/agent.cpp
    src/xi/commands.cpp
    src/xi/process.cpp
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
install(FILES scripts/install-xi-task.ps1 DESTINATION scripts)
