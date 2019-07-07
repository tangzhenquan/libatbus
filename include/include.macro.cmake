# =========== include - macro ===========
set (PROJECT_LIBATBUS_ROOT_INC_DIR ${CMAKE_CURRENT_LIST_DIR})

include_directories(${PROJECT_LIBATBUS_ROOT_INC_DIR})

# define CONF from cmake to c macro
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/detail/libatbus_config.h.in"
    "${CMAKE_CURRENT_LIST_DIR}/detail/libatbus_config.h"
    @ONLY
)

add_custom_command(
    OUTPUT "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail/libatbus_protocol_generated.h"
    COMMAND flatbuffers::flatc --gen-mutable --gen-name-strings --gen-object-api --no-includes --natural-utf8 --allow-non-utf8 
        -o "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail" --cpp "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail/libatbus_protocol.fbs"
    DEPENDS "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail/libatbus_protocol.fbs"
)

add_custom_target(atbus_generate_protocol SOURCES "${PROJECT_LIBATBUS_ROOT_INC_DIR}/detail/libatbus_protocol_generated.h")
if (MSVC)
    set_property(TARGET atbus_generate_protocol PROPERTY FOLDER "atframework")
endif ()
