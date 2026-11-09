cmake_minimum_required(VERSION 3.20)
project(CacheScopeManualRuntime LANGUAGES CXX)

add_library(cachescope_manual_runtime SHARED
  ../../compiler/runtime/lib/TraceRuntime.cpp
)
target_include_directories(cachescope_manual_runtime PRIVATE
  ../../compiler/runtime/include
)
target_compile_features(cachescope_manual_runtime PRIVATE cxx_std_17)
set_target_properties(cachescope_manual_runtime PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(cachescope_manual_runtime PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()
