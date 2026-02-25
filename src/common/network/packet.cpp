/*
 * Author       : kun.dong
 * Copyright (C) 2020-06 - FAST
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Date         : 2020-10-30 11:16:15
 * LastEditors  : fei.wang
 * LastEditTime : 2020-12-03 16:49:40
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
