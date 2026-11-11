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
          --repo ${PROJECT_SOURCE_DIR}
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

add_test(
  NAME cachescope.schema-suite
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/schema_suite.py
          --repo ${PROJECT_SOURCE_DIR}
)

set_tests_properties(
  cachescope.pass-suite
  cachescope.runtime-suite
  cachescope.end-to-end
  cachescope.documentation-suite
  cachescope.schema-suite
  PROPERTIES TIMEOUT 120
)

if(TARGET cachescope_runner_cli)
  add_test(
    NAME cachescope.cache-pipeline-e2e
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cache_pipeline_e2e.py
            --mlir-bin ${LLVM_TOOLS_BINARY_DIR}
            --plugin $<TARGET_FILE:CacheScopePassPlugin>
            --runtime $<TARGET_FILE:cachescope_trace_runtime>
            --runner $<TARGET_FILE:cachescope_runner_cli>
  )
  set_tests_properties(cachescope.cache-pipeline-e2e PROPERTIES TIMEOUT 300)
endif()
