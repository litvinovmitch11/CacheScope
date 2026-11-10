cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
project(CacheScopeRunner VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(cachescope_runner src/trace_runner.cpp)
add_library(CacheScope::Runner ALIAS cachescope_runner)
target_compile_features(cachescope_runner PUBLIC cxx_std_17)
target_include_directories(cachescope_runner PUBLIC ../include)
target_link_libraries(cachescope_runner PRIVATE nlohmann_json::nlohmann_json)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(cachescope_runner PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  add_executable(cachescope_runner_tests tests/runner_tests.cpp)
  target_link_libraries(cachescope_runner_tests PRIVATE
    CacheScope::Runner nlohmann_json::nlohmann_json)
  target_compile_definitions(cachescope_runner_tests PRIVATE
    CACHESCOPE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    CACHESCOPE_SMOKE_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/smoke")
  foreach(suite IN ITEMS integration validation random-miss streaming)
    add_test(NAME cachescope.runner-${suite}
      COMMAND cachescope_runner_tests ${suite})
  endforeach()
endif()
