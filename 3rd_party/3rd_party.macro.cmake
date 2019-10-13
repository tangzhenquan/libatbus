# =========== 3rd_party =========== 
set (PROJECT_3RD_PARTY_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})

macro(project_build_cmake_options OUTVAR)
    set (${OUTVAR} "-G" "${CMAKE_GENERATOR}"
        "-DCMAKE_POLICY_DEFAULT_CMP0075=NEW" 
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    )

    foreach (VAR_NAME IN LISTS PROJECT_3RD_PARTY_CMAKE_INHERIT_VARS)
        if (DEFINED ${VAR_NAME})
            set(VAR_VALUE "${${VAR_NAME}}")
            if (VAR_VALUE)
                list (APPEND ${OUTVAR} "-D${VAR_NAME}=${VAR_VALUE}")
            endif ()
            unset(VAR_VALUE)
        endif ()
    endforeach ()

    if (CMAKE_GENERATOR_PLATFORM)
        list (APPEND ${OUTVAR} "-A" "${CMAKE_GENERATOR_PLATFORM}")
    endif ()

    if (CMAKE_BUILD_TYPE)
        if (MSVC)
            list (APPEND ${OUTVAR} "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
        elseif (CMAKE_BUILD_TYPE STREQUAL "Debug")
            list (APPEND ${OUTVAR} "-DCMAKE_BUILD_TYPE=RelWithDebInfo")
        else ()
            list (APPEND ${OUTVAR} "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
        endif ()
    endif ()
endmacro ()

include("${PROJECT_3RD_PARTY_ROOT_DIR}/libuv/libuv.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/protobuf/protobuf.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/atframe_utils/libatframe_utils.cmake")