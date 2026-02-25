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
 * Date         : 2021-01-28 03:34:30
 * LastEditors  : Pericles
 * LastEditTime : 2021-02-01 22:02:51
 */
#ifndef _FAST_BASE_BITMAP_HPP_
#define _FAST_BASE_BITMAP_HPP_

#include <stdint.h>

template <int32_t Range, typename ValueType>
struct Bitmap {
    ValueType& operator[](int32_t position) { return bitmap[position]; }
    const ValueType& operator[](int32_t position) const { return bitmap[position]; }
    void Set(ValueType value) {
        for (int32_t pos = 0; pos < Range; ++pos) {
            bitmap[pos] = value;
        }
    }
    template <typename fnCheck>
    void SetIf(ValueType value, fnCheck checker) {
        for (int32_t pos = 0; pos < Range; ++pos) {
            if (checker(bitmap[pos])) {
                bitmap[pos] = value;
            }
        }
    }
    ValueType bitmap[Range] = {0};
};

#endif  //_FAST_BASE_BITMAP_HPP_
