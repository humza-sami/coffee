add_executable(coffee_tests
  unit/test_util.cpp
  unit/test_model.cpp
  unit/test_store.cpp
  unit/test_cli.cpp
  unit/test_artifacts.cpp
  unit/test_executor.cpp
  unit/test_workspace.cpp)
target_link_libraries(coffee_tests PRIVATE coffee_core coffee_catch2)
target_compile_definitions(coffee_tests PRIVATE COFFEE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}" COFFEE_BIN="$<TARGET_FILE:coffee>")
add_dependencies(coffee_tests coffee)
target_compile_options(coffee_tests PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME unit COMMAND coffee_tests)

add_test(NAME cli_smoke
  COMMAND ${CMAKE_SOURCE_DIR}/tests/cli_smoke.sh $<TARGET_FILE:coffee> ${CMAKE_SOURCE_DIR}/examples/coffee.yaml)

set(COFFEE_V1_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/tests/integration/fixtures/v1" CACHE PATH "Checksum-verified genuine schema-v1 fixture directory")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  foreach(_suite test_protocol test_cluster test_faults test_demo test_hardening test_quickstart test_upgrade)
    add_test(NAME integration_${_suite}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/${_suite}.py)
    set_tests_properties(integration_${_suite} PROPERTIES
      ENVIRONMENT "COFFEE_BIN=$<TARGET_FILE:coffee>;PYTHONPATH=${CMAKE_SOURCE_DIR}/tests/integration;COFFEE_V1_FIXTURE_DIR=${COFFEE_V1_FIXTURE_DIR}" TIMEOUT 600)
  endforeach()
  foreach(_suite test_install test_install_safety)
    add_test(NAME integration_${_suite}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/${_suite}.py)
    set_tests_properties(integration_${_suite} PROPERTIES
      ENVIRONMENT "COFFEE_BUILD_DIR=${CMAKE_BINARY_DIR};COFFEE_CMAKE=${CMAKE_COMMAND}" TIMEOUT 600)
  endforeach()
endif()
