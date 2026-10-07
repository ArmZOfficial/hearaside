add_executable(ss_unit_tests
    unit/main.cpp
    unit/test_bus.cpp
    unit/test_dsp.cpp
    unit/test_engine.cpp)
target_link_libraries(ss_unit_tests PRIVATE ssengine ssdsp ssbus)
add_test(NAME unit COMMAND ss_unit_tests)

add_executable(ss_ipc_test integration/ipc_test.cpp)
target_link_libraries(ss_ipc_test PRIVATE ssbus)
add_test(NAME ipc COMMAND ss_ipc_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/design/gen_theme.py)
    add_test(NAME design_tokens_in_sync
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/design/gen_theme.py --check)
endif()
