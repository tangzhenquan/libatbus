/**
 * libatbus.h
 *
 *  Created on: 2014年8月11日
 *      Author: owent
 */

#pragma once

#ifndef LIBATBUS_PROTOCOL_H
#define LIBATBUS_PROTOCOL_H

#pragma once

#include "detail/libatbus_protocol.pb.h"

namespace atbus {
    typedef ::atbus::protocol::msg msg_t;
} // namespace atbus

#define ATBUS_MACRO_RESERVED_SIZE 1024

#endif /* LIBATBUS_PROTOCOL_H */
