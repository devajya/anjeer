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

# Expose nlohmann_json's binary dir so jwt-cpp's internal find_package(nlohmann_json)
# resolves to the already-fetched copy instead of printing "Could NOT find" each configure.
FetchContent_GetProperties(nlohmann_json)
list(APPEND CMAKE_PREFIX_PATH "${nlohmann_json_BINARY_DIR}")

# ---------------------------------------------------------------------------
# System dependencies (must be installed on the build machine)
# ---------------------------------------------------------------------------
# AGENT-CTX: These are NOT FetchContent deps — they are system packages that must
# be present before cmake configure runs. Run `make install-deps` on Ubuntu/Debian.
# libpq (PostgreSQL C client) is required at configure time because libpqxx links
# against it. libcurl is used by the OAuth provider implementations to exchange
# authorization codes with GitHub/Google/LinkedIn APIs.
# If either find_package fails: run `make install-deps`, then `make clean build`.
find_package(PostgreSQL REQUIRED)
find_package(CURL       REQUIRED)

# ---------------------------------------------------------------------------
# Crow v1.2.0 — async C++ HTTP/REST server (Slice 5+)
# ---------------------------------------------------------------------------
# AGENT-CTX: Crow is header-only; target is Crow::Crow (INTERFACE).
# Crow does NOT bundle Asio — it calls find_package(asio) at configure time.
# We fetch standalone Asio below and set ASIO_INCLUDE_DIR before crow's
# FetchContent_MakeAvailable runs so Crow's Findasio.cmake resolves it without
# requiring a system package. CROW_ENABLE_SSL is OFF: TLS termination is nginx's
# job in production (Slice 16).
FetchContent_Declare(
  crow
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG        v1.2.0
  GIT_SHALLOW    TRUE
)
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CROW_ENABLE_SSL     OFF CACHE BOOL "" FORCE)

# standalone Asio — required by Crow; not bundled, not Boost
# AGENT-CTX: Must be declared and made available BEFORE crow's FetchContent_MakeAvailable.
# ASIO_INCLUDE_DIR points to the directory containing asio.hpp (asio/include/).
# Crow's Findasio.cmake uses find_path(ASIO_INCLUDE_DIR NAMES asio.hpp ...) and
# will find it there. No Boost, no system asio package required.
FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG        asio-1-28-2
  GIT_SHALLOW    TRUE
)

# jwt-cpp v0.7.0 — header-only JWT signing/validation (Slice 5+)
# AGENT-CTX: jwt-cpp uses its own bundled picojson internally. We leave
# JWT_DISABLE_PICOJSON OFF to keep jwt-cpp self-contained — coupling its
# internals to nlohmann adds complexity with no benefit. Server auth code
# uses OpenSSL-backed HMAC-SHA256 via jwt-cpp. Target: jwt-cpp::jwt-cpp.
FetchContent_Declare(
  jwt_cpp
  GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
  GIT_TAG        v0.7.0
  GIT_SHALLOW    TRUE
)
set(JWT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# libpqxx v7.9.2 — C++ PostgreSQL client (Slice 5+)
# AGENT-CTX: libpqxx wraps the system libpq C driver located via find_package(PostgreSQL).
# It is processed in its own FetchContent_MakeAvailable call with BUILD_TESTING temporarily
# OFF to suppress libpqxx's own test suite (which requires a live DB connection and would
# fail in CI). BUILD_TESTING is restored to ON immediately after so our Catch2 tests
# are unaffected. Target is libpqxx::pqxx (alias in libpqxx CMakeLists).
# If a version upgrade renames the alias, update target_link_libraries in server/CMakeLists.txt.
FetchContent_Declare(
  libpqxx
  GIT_REPOSITORY https://github.com/jtv/libpqxx.git
  GIT_TAG        7.9.2
  GIT_SHALLOW    TRUE
)
set(PQXX_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PQXX_INSTALL      OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(asio)
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include" CACHE PATH "" FORCE)
FetchContent_MakeAvailable(crow jwt_cpp)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libpqxx)
set(BUILD_TESTING ON  CACHE BOOL "" FORCE)

# moodycamel/readerwriterqueue — lock-free SPSC queue for NetEvent/GameEvent
# AGENT-CTX: Header-only; exposes readerwriterqueue.h directly from source dir.
# Used to decouple WsServer (uWS event loop thread) from GameSession (game loop
# thread) without any mutex — the SPSC guarantee holds because exactly one
# producer and one consumer exist per direction per session.
# GIT_TAG pinned to a known-good commit; bump only when upgrading intentionally.
FetchContent_Declare(
  readerwriterqueue
  GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
  GIT_TAG        v1.0.6
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(readerwriterqueue)

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
