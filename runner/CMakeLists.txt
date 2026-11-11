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
  PUBLIC CacheScope::Hierarchy
  PRIVATE nlohmann_json::nlohmann_json)
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

if(BUILD_TESTING)
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
