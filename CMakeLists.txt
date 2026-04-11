cmake_minimum_required(VERSION 3.20)
# AGENT-CTX: Both C and CXX are required — uSockets is written in C, the rest in C++.
project(anjeer CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build type defaults
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# AGENT-CTX: Store fetched dependency sources in .deps/ at the project root,
# NOT inside build/. This means `make clean` (which wipes build/) never
# deletes downloaded sources. Re-downloads only happen on `make clean-all`.
# .deps/ is gitignored. If a dep fails to configure, delete its subdirectory
# inside .deps/ and re-run cmake — the source will be re-fetched only for that dep.
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH
    "Root directory for FetchContent downloads" FORCE)

# nlohmann/json — header-only JSON library for config parsing
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

# uWebSockets — async WebSocket/HTTP server.
# AGENT-CTX: uWebSockets v20 bundles uSockets as a subdirectory (uSockets/).
# We build uSockets from that bundled copy — no separate usockets FetchContent needed.
# If upgrading uWebSockets, verify the bundled uSockets path hasn't changed.
FetchContent_Declare(
  uwebsockets
  GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
  GIT_TAG        v20.62.0
  GIT_SHALLOW    TRUE
)

# Catch2 v3 — unit testing framework
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.4
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json Catch2 uwebsockets)

# ---------------------------------------------------------------------------
# uSockets — built from the copy bundled inside uWebSockets
# ---------------------------------------------------------------------------
# AGENT-CTX: uSockets has no CMakeLists.txt; we compile it manually.
# Source files are listed explicitly (not glob) to avoid accidentally including
# quic.c (needs QUIC libs), io_uring/ (Linux 5.1+ only), and bsd.c variations.
# Targeting Linux (epoll) + OpenSSL. Update this list if the uSockets layout
# changes on a uWebSockets version upgrade.
set(USOCKETS_DIR "${uwebsockets_SOURCE_DIR}/uSockets")

add_library(uSockets STATIC
  ${USOCKETS_DIR}/src/bsd.c
  ${USOCKETS_DIR}/src/context.c
  ${USOCKETS_DIR}/src/loop.c
  ${USOCKETS_DIR}/src/socket.c
  ${USOCKETS_DIR}/src/udp.c
  ${USOCKETS_DIR}/src/eventing/epoll_kqueue.c
  ${USOCKETS_DIR}/src/crypto/openssl.c
  # AGENT-CTX: sni_tree.cpp is C++ and defines sni_new/sni_find/sni_add/sni_free
  # which openssl.c calls for Server Name Indication (SNI) routing. Required even
  # when SSL is not actively used — openssl.c unconditionally references these symbols.
  ${USOCKETS_DIR}/src/crypto/sni_tree.cpp
)

target_include_directories(uSockets PUBLIC ${USOCKETS_DIR}/src)
target_compile_definitions(uSockets PUBLIC LIBUS_USE_OPENSSL)
target_link_libraries(uSockets PUBLIC ssl crypto z)

# uWebSockets is header-only — expose as an interface target
add_library(uWebSockets INTERFACE)
target_include_directories(uWebSockets INTERFACE ${uwebsockets_SOURCE_DIR}/src)
target_link_libraries(uWebSockets INTERFACE uSockets)

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
add_subdirectory(engine)
add_subdirectory(server)

# ---------------------------------------------------------------------------
# CTest
# ---------------------------------------------------------------------------
enable_testing()
