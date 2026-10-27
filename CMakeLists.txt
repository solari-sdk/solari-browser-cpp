cmake_minimum_required(VERSION 3.16)
project(solari_browser CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SOLARI_WITH_WS "Build the IXWebSocket raw-CDP escape hatch" ON)
# Tests are on by default only when this is the top-level project, so a
# FetchContent / add_subdirectory consumer never pulls in doctest.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_solari_tests_default ON)
else()
  set(_solari_tests_default OFF)
endif()
option(SOLARI_BUILD_TESTS "Build the offline test suite" ${_solari_tests_default})

include(FetchContent)

# --- nlohmann/json ----------------------------------------------------------
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

# --- libcurl (system) + threads ---------------------------------------------
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(SOLARI_BROWSER_SOURCES
  src/errors.cpp
  src/http.cpp
  src/types.cpp
  src/client.cpp
)

# --- IXWebSocket (optional; TLS via OpenSSL) --------------------------------
if(SOLARI_WITH_WS)
  set(USE_TLS ON CACHE BOOL "" FORCE)
  set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    # v11.4.5, pinned to the commit so a moved tag cannot change what an unattended build compiles
    GIT_TAG c5a02f1066fb0fde48f80f51178429a27f689a39
  )
  FetchContent_MakeAvailable(ixwebsocket)
  list(APPEND SOLARI_BROWSER_SOURCES src/cdp.cpp src/launch.cpp)
endif()

add_library(solari_browser ${SOLARI_BROWSER_SOURCES})
target_include_directories(solari_browser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(solari_browser PUBLIC
  nlohmann_json::nlohmann_json
  CURL::libcurl
  Threads::Threads
)
if(SOLARI_WITH_WS)
  target_link_libraries(solari_browser PUBLIC ixwebsocket)
  target_compile_definitions(solari_browser PUBLIC SOLARI_WITH_WS=1)
endif()

# --- tests (doctest, offline) -----------------------------------------------
if(SOLARI_BUILD_TESTS)
  FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
    URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
  )
  FetchContent_MakeAvailable(doctest)

  add_executable(solari_browser_tests tests/test_main.cpp)
  target_link_libraries(solari_browser_tests PRIVATE solari_browser doctest::doctest)

  enable_testing()
  add_test(NAME solari_browser_tests COMMAND solari_browser_tests)
endif()
