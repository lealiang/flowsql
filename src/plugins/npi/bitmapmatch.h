/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_BITMAPMATCH_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_BITMAPMATCH_H_

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

#endif  // _FLOWSQL_PLUGINS_PROTOCOL_NPI_BITMAPMATCH_H_
