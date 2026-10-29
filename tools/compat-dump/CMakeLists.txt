cmake_minimum_required(VERSION 3.25)
project(HearasideLayout LANGUAGES CXX)

add_executable(hs-layout-dump layout.cpp)
target_compile_features(hs-layout-dump PRIVATE cxx_std_20)
target_include_directories(hs-layout-dump PRIVATE ../../libs/ssbus/include)
if(MSVC)
    target_compile_options(hs-layout-dump PRIVATE /utf-8 /permissive-)
endif()

enable_testing()
add_test(NAME shm_v14_layout COMMAND hs-layout-dump --check
    ${CMAKE_CURRENT_SOURCE_DIR}/../../compat/fixtures/shm-v14-layout.json)
