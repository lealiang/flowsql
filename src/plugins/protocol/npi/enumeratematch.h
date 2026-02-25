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
 * LastEditTime : 2022-07-05 14:42:30
 */

#ifndef _FAST_NPI_ENUMERATEMATCH_H_
#define _FAST_NPI_ENUMERATEMATCH_H_

#include <stdint.h>
#include <common/algo/bitmap.hpp>
#include <limits>
#include <map>
#include <vector>
#include "irecognizer.h"

namespace flowsql {
namespace protocol {

class EnumerateRecognizer : public IRecognizer {
 public:
    EnumerateRecognizer(int32_t position) : position_(position), default_(0) {}
    virtual int32_t Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size, const protocol::Layers* layers,
                             RecognizeContext* rctx) = 0;

    virtual void Set(int32_t key, int32_t value) = 0;
    virtual int32_t Position() = 0;
    virtual int32_t Width() = 0;
    inline void Default(int32_t dft) { default_ = dft; }

 protected:
    int32_t position_ = 0;
    int32_t default_ = 0;
};

template <typename Integer = uint8_t>
class SmallEnumerateRecognizer : public EnumerateRecognizer {
 public:
    SmallEnumerateRecognizer(int32_t position) : EnumerateRecognizer(position) {}

    virtual void Set(int32_t key, int32_t value) { enums_[key] = value; }
    virtual int32_t Position() { return position_; }
    virtual int32_t Width() { return sizeof(Integer); }

    virtual int32_t Identify(int32_t /* pipeno */, const uint8_t* packet, int32_t packet_size,
                             const protocol::Layers* layers, RecognizeContext* rctx) {
        const uint8_t* payload = layers->Data(packet, packet_size);
        uint16_t length = layers->Payload(packet, packet_size);
        if (position_ + sizeof(Integer) <= length) {
            int32_t value = enums_[*((Integer*)(payload + position_))];
            if (value) {
                return value;
            }
            return default_;
        }

        return eLayer::NONE;
    }

 protected:
    Bitmap<256, int32_t> enums_;
};

template <typename Integer>
class GrandEnumerateRecognizer : public EnumerateRecognizer {
 public:
    GrandEnumerateRecognizer(int32_t position, int32_t bytewidth)
        : EnumerateRecognizer(position), bytewidth_(bytewidth) {}
    virtual void Set(int32_t key, int32_t value) { enums_[key] = value; }
    virtual int32_t Position() { return position_; }
    virtual int32_t Width() { return bytewidth_; }

    virtual int32_t Identify(int32_t /* pipeno */, const uint8_t* packet, int32_t packet_size,
                             const protocol::Layers* layers, RecognizeContext* rctx) {
        const uint8_t* payload = layers->Data(packet, packet_size);
        uint16_t length = layers->Payload(packet, packet_size);
        if (position_ + sizeof(Integer) <= length) {
            Integer number =
                *((Integer*)(payload + position_)) << (sizeof(Integer) - bytewidth_) >> (sizeof(Integer) - bytewidth_);
            int32_t value = enums_[number];
            if (value) {
                return value;
            }
            return default_;
        }

        return eLayer::NONE;
    }

 protected:
    std::map<Integer, int32_t> enums_;
    int32_t bytewidth_ = 0;
};

class EnumerateRecognizerPool {
 public:
    static EnumerateRecognizerPool* instance() {
        static EnumerateRecognizerPool _this;
        return &_this;
    }

    EnumerateRecognizer* Create(const std::string& enumstr, int32_t proid);
    int32_t Update(EnumerateRecognizer* enumer, const std::string& enumstr, int32_t proid);

 protected:
    EnumerateRecognizerPool() { pool_.reserve(1024); }
    ~EnumerateRecognizerPool();

 private:
    std::vector<EnumerateRecognizer*> pool_;
};

}  // namespace protocol
}  // namespace flowsql

#endif  // _FAST_NPI_ENUMERATEMATCH_H_
