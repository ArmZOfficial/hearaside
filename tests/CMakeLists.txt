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

# permanent share links, web side (web/share-vercel): the pages match plugins/hub/web, API tests
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME share_web_in_sync COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/web/share-vercel/scripts/sync-web.mjs --check)
    add_test(NAME share_api COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_SOURCE_DIR}/web/share-vercel/test/directory.test.js)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/design/gen_theme.py)
    add_test(NAME design_tokens_in_sync
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/design/gen_theme.py --check)
    add_test(NAME design_contrast   # WCAG 4.5:1 for every text colour (docs/ux-roadmap.md 6.2)
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/design/gen_theme.py --contrast)
endif()
