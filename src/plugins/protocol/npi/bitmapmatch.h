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
 * LastEditTime : 2022-06-24 17:35:20
 */

#ifndef _FAST_NPI_BITMAPMATCH_H_
#define _FAST_NPI_BITMAPMATCH_H_

#include <stdint.h>
#include <common/algo/bitmap.hpp>
#include "irecognizer.h"

namespace flowsql {
namespace protocol {


class BitmapRecognizer : public IRecognizer {
 public:
    inline void Set(int32_t key, IRecognizer* value) { entries_[key] = value; }
    inline IRecognizer* Get(int32_t key) { return entries_[key]; }
    inline void SetAll(IRecognizer* value) { entries_.Set(value); }
    inline void SetEmpty(IRecognizer* value) {
        entries_.SetIf(value, [](const IRecognizer* item) -> bool { return !item; });
    }
    virtual int32_t Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size, const protocol::Layers* layers,
                             RecognizeContext* rctx);

 protected:
    Bitmap<65536, IRecognizer*> entries_;
};

class BitmapDualRecognizer : public BitmapRecognizer {
 public:
    virtual int32_t Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size, const protocol::Layers* layers,
                             RecognizeContext* rctx);
};


}  // namespace protocol
}  // namespace flowsql

#endif  // _FAST_NPI_BITMAPMATCH_H_
