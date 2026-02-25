/*
 * Author       : LIHUO
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Date         : 2020-10-30 11:16:15
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#include "packet.h"

#include <common/logger_helper.h>

namespace flowsql {

PacketBuffer::PacketBuffer(uint32_t buff_len) : used_len_(0) {
    this->buff_len_ = buff_len;

    // for trace
    // LOG_W() << "PacketBuffer::PacketBuffer";
}

PacketBuffer::~PacketBuffer() {
    // for trace
    // LOG_W() << "PacketBuffer::~PacketBuffer";
    ;
}

}  // namespace  fast
