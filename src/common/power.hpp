/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-12-08 03:30:23
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FAST_BASE_POWER_HPP
#define _FAST_BASE_POWER_HPP

namespace flowsql {
template <typename value_type>
inline value_type get_exponent_number(value_type number) {
    value_type exponent = (number & (number - 1)) ? 1 : 0;
    value_type numeric = number;
    while ((numeric >>= 1) != 0) {
        ++exponent;
    }

    return exponent;
}

template <typename value_type>
inline value_type get_power_number(value_type number) {
    value_type exponent = (number & (number - 1)) ? 1 : 0;
    value_type numeric = number;
    while ((numeric >>= 1) != 0) {
        ++exponent;
    }

    return (value_type)(1 << exponent);
}
}  // namespace flowsql

#endif  // _FAST_BASE_POWER_HPP
