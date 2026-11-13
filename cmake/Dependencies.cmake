# Third-party dependencies, fetched at configure time and pinned by version + SHA-256.
# Offline builds: pre-download the archives and point FETCHCONTENT_SOURCE_DIR_<NAME> at the
# extracted folders, e.g. -DFETCHCONTENT_SOURCE_DIR_HTTPLIB=C:/deps/cpp-httplib-0.18.3
# See adr/ADR-007-cpp-implementation-xgi-xi.md for why these three were chosen.

include(FetchContent)

# yaml-cpp 0.8.0 declares an old cmake_minimum_required; CMake 4 needs this to configure it.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# --- cpp-httplib: header-only HTTP/1.1 server + client (plain HTTP; the cluster is LAN-only)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL                  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
    URL_HASH SHA256=a0567bcd6c3fe5cef1b329b96245119047f876b49e06cc129a36a7a8dffe173e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# --- nlohmann/json: JSON
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# --- yaml-cpp: cluster.yaml
set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL       OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_MSVC_SHARED_RT    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
    URL      https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
    URL_HASH SHA256=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(httplib json yaml-cpp)
