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

# xgi and xi are each one self-contained executable: the static libraries below (xgi_common, xi_lib,
# xgi_lib, yaml-cpp) are build intermediates linked into them, so keep them out of the folder the
# executables land in.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

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
    src/common/hub.cpp
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
    target_link_libraries(xgi_common PUBLIC ws2_32 advapi32)
endif()

# ---------------------------------------------------------------- web pages
# The XGI console and the XI page are plain HTML/CSS/JS, embedded into the binaries at build time
# (cmake/EmbedAssets.cmake) so each stays one file. Entries are files under ROOT, or
# "<served name>=<absolute path>" for a file elsewhere.
function(xgi_embed_ui OUT NAMESPACE HEADER ROOT)
    set(deps "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake")
    foreach(entry IN LISTS ARGN)
        if(entry MATCHES "^([^=]+)=(.+)$")
            list(APPEND deps "${CMAKE_MATCH_2}")
        else()
            list(APPEND deps "${ROOT}/${entry}")
        endif()
    endforeach()
    list(JOIN ARGN "," list)
    add_custom_command(
        OUTPUT "${OUT}"
        COMMAND "${CMAKE_COMMAND}" "-DROOT=${ROOT}" "-DFILES=${list}" "-DOUT=${OUT}" "-DNAMESPACE=${NAMESPACE}"
                "-DHEADER=${HEADER}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake"
        DEPENDS ${deps}
        COMMENT "Embedding web page files for ${NAMESPACE}"
        VERBATIM)
endfunction()

# ---------------------------------------------------------------- XI (node agent)
set(XI_UI_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/xi_ui_assets.cpp")
xgi_embed_ui("${XI_UI_CPP}" xi::ui xi/ui_assets.hpp "${CMAKE_CURRENT_SOURCE_DIR}/src/xi/ui"
    index.html xi.js "app.css=${CMAKE_CURRENT_SOURCE_DIR}/src/xgi/ui/app.css" xi.css)

if(WIN32)
    set(XI_PROCESS_SRC src/xi/process_windows.cpp)
else()
    set(XI_PROCESS_SRC src/xi/process_posix.cpp)
endif()
add_library(xi_lib STATIC
    src/xi/agent.cpp
    src/xi/commands.cpp
    src/xi/downloads.cpp
    src/xi/model_files.cpp
    src/xi/process.cpp
    src/xi/runtimes.cpp
    src/xi/settings.cpp
    "${XI_UI_CPP}"
    ${XI_PROCESS_SRC}
)
target_link_libraries(xi_lib PUBLIC xgi_common)

add_executable(xi src/xi/main.cpp)
target_link_libraries(xi PRIVATE xi_lib)

# ---------------------------------------------------------------- XGI (coordinator + router)
set(XGI_UI_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/ui_assets.cpp")
xgi_embed_ui("${XGI_UI_CPP}" xgi::ui xgi/ui_assets.hpp "${CMAKE_CURRENT_SOURCE_DIR}/src/xgi/ui"
    index.html app.css app.js)

add_library(xgi_lib STATIC
    src/xgi/config_store.cpp
    src/xgi/console.cpp
    "${XGI_UI_CPP}"
    src/xgi/coordinator.cpp
    src/xgi/download_cli.cpp
    src/xgi/placement.cpp
    src/xgi/router.cpp
    src/xgi/scheduling.cpp
)
# XGI reuses XI's download manager for downloads onto its own machine (ADR-011); only the objects
# it needs are linked in, and xgi stays one executable.
target_link_libraries(xgi_lib PUBLIC xgi_common xi_lib)

add_executable(xgi src/xgi/main.cpp)
target_link_libraries(xgi PRIVATE xgi_lib)
# Put a starter cluster.yaml next to the executables (only if none is there, so edits survive rebuilds).
add_custom_command(TARGET xgi POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/examples/cluster.yaml"
            "-DDST=$<TARGET_FILE_DIR:xgi>/cluster.yaml" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyIfMissing.cmake"
    VERBATIM)

# ---------------------------------------------------------------- tests
if(XGI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- install / package
install(TARGETS xi xgi RUNTIME DESTINATION bin)
install(FILES examples/cluster.yaml DESTINATION .)
install(FILES scripts/install-xi-task.ps1 scripts/install-xi-service.sh DESTINATION scripts)
