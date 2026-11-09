add_library(cachescope_trace_runtime SHARED
  lib/TraceRuntime.cpp
)

target_include_directories(cachescope_trace_runtime
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(cachescope_trace_runtime PRIVATE cxx_std_17)
set_target_properties(cachescope_trace_runtime PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(cachescope_trace_runtime PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

install(TARGETS cachescope_trace_runtime
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
