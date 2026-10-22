add_executable(ss_unit_tests
    unit/main.cpp
    unit/test_bus.cpp
    unit/test_dsp.cpp
    unit/test_engine.cpp
    unit/test_friends.cpp
    unit/test_jitter.cpp
    unit/test_autosync.cpp
    unit/test_paced_send.cpp
    unit/test_net.cpp
    unit/test_rt_safety.cpp)
target_link_libraries(ss_unit_tests PRIVATE ssengine ssdsp ssbus ssnet)
add_test(NAME unit COMMAND ss_unit_tests)

add_executable(ss_ipc_test integration/ipc_test.cpp)
target_link_libraries(ss_ipc_test PRIVATE ssbus)
add_test(NAME ipc COMMAND ss_ipc_test)

# permanent share links, web side (web/share-vercel): the pages match plugins/hub/web, API tests
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME share_web_in_sync COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/web/share-vercel/scripts/sync-web.mjs --check)
    add_test(NAME share_api COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_SOURCE_DIR}/web/share-vercel/test/directory.test.js)
    add_test(NAME net_sim COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/net-sim/net-sim.test.mjs)   # the bad-line proxy for the friend page tests
    add_test(NAME web_worklet COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/web-friend/worklet.test.mjs)   # the friend page's audio worklet, no browser
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/design/gen_theme.py)
    add_test(NAME design_tokens_in_sync
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/design/gen_theme.py --check)
    add_test(NAME design_contrast   # WCAG 4.5:1 for every text colour (docs/ux-roadmap.md 6.2)
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/design/gen_theme.py --contrast)
endif()
