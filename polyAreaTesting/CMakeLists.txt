cmake_minimum_required(VERSION 3.15)
project(poly_latency_bench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# On macOS with Homebrew, curl/openssl are keg-only and not on the default
# search path. Harmless no-op on Linux where these paths don't exist.
list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/curl /opt/homebrew/opt/openssl@3
                               /usr/local/opt/curl /usr/local/opt/openssl@3)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# --- IXWebSocket, pulled at configure time -----------------------------
# Not reliably packaged via apt/brew at a fixed version, so we fetch a
# pinned release instead of depending on whatever the OS repo happens to
# carry. Requires internet access when you first run `cmake ..`.
include(FetchContent)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(USE_SECURE_TRANSPORT OFF CACHE BOOL "" FORCE)
set(USE_MBED_TLS OFF CACHE BOOL "" FORCE)
set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(USE_TEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v12.0.1
)
FetchContent_MakeAvailable(ixwebsocket)

add_executable(poly_latency_bench
  src/main.cpp
  src/http_bench.cpp
  src/ws_bench.cpp
)

target_include_directories(poly_latency_bench PRIVATE src)
target_link_libraries(poly_latency_bench PRIVATE
  CURL::libcurl
  ixwebsocket
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)
