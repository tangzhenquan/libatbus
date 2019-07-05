# =========== include - macro ===========
set (PROJECT_LIBATBUS_ROOT_INC_DIR ${CMAKE_CURRENT_LIST_DIR})

include_directories(${PROJECT_LIBATBUS_ROOT_INC_DIR})

# define CONF from cmake to c macro
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/detail/libatbus_config.h.in"
    "${CMAKE_CURRENT_LIST_DIR}/detail/libatbus_config.h"
    @ONLY
)

execute_process(
    COMMAND flatbuffers::flatc --gen-mutable --gen-name-strings --no-includes --no-prefix --natural-utf8 --allow-non-utf8 
    -i "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail/libatbus_protocol.fbs" -o "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail"
)

add_custom_command(
    OUTPUT "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail/libatbus_protocol_generated.h"
    COMMAND flatbuffers::flatc --gen-mutable --gen-name-strings --no-includes --no-prefix --natural-utf8 --allow-non-utf8 
        -i "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail/libatbus_protocol.fbs" -o "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail"
    DEPENDS "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail/libatbus_protocol.fbs"
)
