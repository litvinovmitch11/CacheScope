find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CACHESCOPE_TEST_COMMON_ARGS
  --repo ${PROJECT_SOURCE_DIR}
  --mlir-bin ${LLVM_TOOLS_BINARY_DIR}
  --plugin $<TARGET_FILE:CacheScopePassPlugin>
  --runtime $<TARGET_FILE:cachescope_trace_runtime>
)

add_test(
  NAME cachescope.pass-suite
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/pass_suite.py
          ${CACHESCOPE_TEST_COMMON_ARGS}
)

add_test(
  NAME cachescope.runtime-suite
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/runtime_suite.py
          --runtime $<TARGET_FILE:cachescope_trace_runtime>
)

add_test(
  NAME cachescope.end-to-end
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/end_to_end.py
          ${CACHESCOPE_TEST_COMMON_ARGS}
)

add_test(
  NAME cachescope.documentation-suite
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/documentation_suite.py
          --repo ${PROJECT_SOURCE_DIR}
)

set_tests_properties(
  cachescope.pass-suite
  cachescope.runtime-suite
  cachescope.end-to-end
  cachescope.documentation-suite
  PROPERTIES TIMEOUT 120
)
