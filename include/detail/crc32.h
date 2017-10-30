#pragma once

#ifndef LIBATBUS_DETAIL_CRC32_H
#define LIBATBUS_DETAIL_CRC32_H

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace atbus {
    namespace detail {
        uint32_t crc32(uint32_t crc, const unsigned char *s, size_t l);
    }
}

#endif
