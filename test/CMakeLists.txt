
EchoWithColor(COLOR GREEN "-- Configure Unit Test ${CMAKE_CURRENT_LIST_DIR}")

include("${PROJECT_TEST_BAS_DIR}/test.build_bin.cmake")

include_directories(${PROJECT_LIBATBUS_ROOT_SRC_DIR})

file(GLOB_RECURSE PROJECT_TEST_SRC_LIST
    ${PROJECT_TEST_SRC_DIR}/app/*.cpp
    ${PROJECT_TEST_SRC_DIR}/frame/*.h
    ${PROJECT_TEST_SRC_DIR}/frame/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/*.hpp
    ${CMAKE_CURRENT_LIST_DIR}/*.h
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/*.cc
    ${CMAKE_CURRENT_LIST_DIR}/*.cxx
)
source_group_by_dir(PROJECT_TEST_SRC_LIST)

# ============ test - coroutine test frame ============
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/test")

find_package(Threads)
if (CMAKE_USE_PTHREADS_INIT)
    list(APPEND COMPILER_OPTION_EXTERN_CXX_LIBS pthread)
endif ()

add_executable(atbus_unit_test ${PROJECT_TEST_SRC_LIST})
target_link_libraries(atbus_unit_test
    ${PROJECT_LIB_LINK}
    ${PROJECT_TEST_LIB_LINK}
    ${3RD_PARTY_LIBUV_LINK_NAME}
    ${3RD_PARTY_ATFRAME_UTILS_LINK_NAME}
    ${COMPILER_OPTION_EXTERN_CXX_LIBS}
)

add_test(test atbus_unit_test)
