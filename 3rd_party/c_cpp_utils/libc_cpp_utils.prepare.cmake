
# =========== 3rdparty c_cpp_utils ==================
set (3RD_PARTY_C_CPP_UTILS_BASE_DIR ${CMAKE_CURRENT_LIST_DIR})

if (ATFRAME_UTILS_ROOT)
    set (3RD_PARTY_C_CPP_UTILS_PKG_DIR ${ATFRAME_UTILS_ROOT})
else()
    set (3RD_PARTY_C_CPP_UTILS_PKG_DIR "${3RD_PARTY_C_CPP_UTILS_BASE_DIR}/repo")
    if(NOT EXISTS ${3RD_PARTY_C_CPP_UTILS_PKG_DIR})
        find_package(Git)
        execute_process(COMMAND ${GIT_EXECUTABLE} clone "https://github.com/atframework/atframe_utils.git" ${3RD_PARTY_C_CPP_UTILS_PKG_DIR}
            WORKING_DIRECTORY ${3RD_PARTY_C_CPP_UTILS_BASE_DIR}
        )
    endif()
endif()

set (3RD_PARTY_C_CPP_UTILS_INC_DIR "${3RD_PARTY_C_CPP_UTILS_PKG_DIR}/include")
set (3RD_PARTY_C_CPP_UTILS_SRC_DIR "${3RD_PARTY_C_CPP_UTILS_PKG_DIR}/src")
set (3RD_PARTY_C_CPP_UTILS_LINK_NAME atframe_utils)
