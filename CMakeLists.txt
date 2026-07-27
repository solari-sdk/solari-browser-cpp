cmake_minimum_required(VERSION 3.16)
project(solari_browser CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SOLARI_WITH_WS "Build the IXWebSocket raw-CDP escape hatch" ON)
option(SOLARI_BUILD_TESTS "Build the offline test suite" ON)

include(FetchContent)

# --- nlohmann/json ----------------------------------------------------------
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
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
    GIT_TAG v11.4.5
  )
  FetchContent_MakeAvailable(ixwebsocket)
  list(APPEND SOLARI_BROWSER_SOURCES src/cdp.cpp)
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
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
  )
  FetchContent_MakeAvailable(doctest)

  add_executable(solari_browser_tests tests/test_main.cpp)
  target_link_libraries(solari_browser_tests PRIVATE solari_browser doctest::doctest)

  enable_testing()
  add_test(NAME solari_browser_tests COMMAND solari_browser_tests)
endif()
