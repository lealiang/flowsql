/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-28 03:34:30
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_COMMON_ALGO_BITMAP_HPP_
#define _FLOWSQL_COMMON_ALGO_BITMAP_HPP_

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

#endif  //_FLOWSQL_COMMON_ALGO_BITMAP_HPP_
