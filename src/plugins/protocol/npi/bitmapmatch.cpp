/*
 * Copyright (C) 2020-06 - FAST
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Author       : Pericles
 * Date         : 2021-01-24 14:57:26
 * LastEditors  : Pericels
 * LastEditTime : 2022-06-30 10:48:39
 */

#include "bitmapmatch.h"

namespace flowsql {
namespace protocol {
int32_t BitmapRecognizer::Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                   const protocol::Layers* layers, RecognizeContext* rctx) {
    return entries_[rctx->proto]->Identify(pipeno, packet, packet_size, layers, rctx);
}

int32_t BitmapDualRecognizer::Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                       const protocol::Layers* layers, RecognizeContext* rctx) {
    // TCP/UDP/SCTP
    int32_t pro = eLayer::NONE;
    IRecognizer* dst_recognizer = entries_[rctx->w1];
    IRecognizer* src_recognizer = entries_[rctx->w2];

    if (dst_recognizer == src_recognizer) {
        pro = dst_recognizer->Identify(pipeno, packet, packet_size, layers, rctx);
    } else {
        pro = dst_recognizer->Identify(pipeno, packet, packet_size, layers, rctx);
        if (!pro) {
            pro = src_recognizer->Identify(pipeno, packet, packet_size, layers, rctx);
        }
    }

    return pro;
}
}  // namespace protocol
}  // namespace flowsql
