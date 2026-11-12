cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
project(CacheScopeRunner VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)
include(GNUInstallDirs)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
)
FetchContent_MakeAvailable(nlohmann_json)

if(NOT TARGET cachescope_hierarchy)
  add_subdirectory(../simulator ${CMAKE_CURRENT_BINARY_DIR}/simulator)
endif()

add_library(cachescope_runner lib/trace_runner.cpp)
add_library(CacheScope::Runner ALIAS cachescope_runner)
target_compile_features(cachescope_runner PUBLIC cxx_std_17)
target_include_directories(cachescope_runner PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cachescope_runner
  PUBLIC CacheScope::Hierarchy nlohmann_json::nlohmann_json)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(cachescope_runner PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cachescope_runner_cli tools/cachescope-runner.cpp)
set_target_properties(cachescope_runner_cli PROPERTIES
  OUTPUT_NAME cachescope-runner)
target_link_libraries(cachescope_runner_cli PRIVATE CacheScope::Runner)
target_compile_features(cachescope_runner_cli PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(cachescope_runner_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS cachescope_runner_cli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

option(CACHESCOPE_BUILD_HTTP_SERVER "Build the local interactive runner HTTP backend" ON)
if(CACHESCOPE_BUILD_HTTP_SERVER)
  set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(httplib
    URL https://codeload.github.com/yhirose/cpp-httplib/tar.gz/refs/tags/v0.66.0
    URL_HASH SHA256=537a77cdb96d3b4ad64d0b7103971de52eac75cd4ee03a2fdb84417b0d742459
  )
  FetchContent_MakeAvailable(httplib)
  find_package(Threads REQUIRED)
  add_executable(cachescope_runner_server app/server.cpp app/experiments.cpp lib/runner_api.cpp)
  target_include_directories(cachescope_runner_server PRIVATE lib)
  target_link_libraries(cachescope_runner_server PRIVATE
    CacheScope::Runner httplib::httplib Threads::Threads)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(cachescope_runner_server PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(BUILD_TESTING)
  if(CACHESCOPE_BUILD_HTTP_SERVER)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME cachescope.runner-http
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_tests.py
        --server $<TARGET_FILE:cachescope_runner_server>
        --repo ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_tests_properties(cachescope.runner-http PROPERTIES TIMEOUT 120)
    add_executable(cachescope_session_tests tests/session_tests.cpp lib/runner_api.cpp)
    target_include_directories(cachescope_session_tests PRIVATE lib)
    target_link_libraries(cachescope_session_tests PRIVATE CacheScope::Runner Threads::Threads)
    add_test(NAME cachescope.runner-session-failure
      COMMAND cachescope_session_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../schemas/fixtures/trace-v1-complete.json)
  endif()
  add_executable(cachescope_runner_tests tests/runner_tests.cpp)
  target_link_libraries(cachescope_runner_tests PRIVATE
    CacheScope::Runner nlohmann_json::nlohmann_json)
  target_compile_definitions(cachescope_runner_tests PRIVATE
    CACHESCOPE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    CACHESCOPE_TRACE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../schemas/fixtures"
    CACHESCOPE_SMOKE_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/smoke")
  foreach(suite IN ITEMS integration real-cache fifo-cache random-cache
                         validation random-miss streaming)
    add_test(NAME cachescope.runner-${suite}
      COMMAND cachescope_runner_tests ${suite})
  endforeach()
  foreach(policy IN ITEMS lru fifo random)
    add_test(NAME cachescope.runner-cli-${policy}
      COMMAND cachescope_runner_cli
        --trace ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/example.trace.json
        --output-dir ${CMAKE_CURRENT_BINARY_DIR}/cli-smoke-${policy}
        --cache-name L1
        --size-bytes 32768
        --line-size-bytes 64
        --associativity 8
        --replacement-policy ${policy}
        --random-seed 42)
  endforeach()
  add_test(NAME cachescope.runner-cli-hierarchy
    COMMAND cachescope_runner_cli
      --trace ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/example.trace.json
      --output-dir ${CMAKE_CURRENT_BINARY_DIR}/cli-smoke-hierarchy
      --cache-level L1:32768:64:8:lru
      --cache-level L2:262144:64:8:fifo
      --cache-level L3:8388608:64:16:random:42)
  add_test(NAME cachescope.runner-cli-rejects-malformed-level
    COMMAND cachescope_runner_cli
      --trace ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/example.trace.json
      --output-dir ${CMAKE_CURRENT_BINARY_DIR}/cli-invalid-level
      --cache-level L1:32768:64:8)
  set_tests_properties(cachescope.runner-cli-rejects-malformed-level
    PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cachescope.runner-cli-rejects-mixed-line-sizes
    COMMAND cachescope_runner_cli
      --trace ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/example.trace.json
      --output-dir ${CMAKE_CURRENT_BINARY_DIR}/cli-invalid-lines
      --cache-level L1:32768:64:8:lru
      --cache-level L2:262144:128:8:lru)
  set_tests_properties(cachescope.runner-cli-rejects-mixed-line-sizes
    PROPERTIES WILL_FAIL TRUE)
endif()
