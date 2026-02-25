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
 * Date         : 2020-12-08 03:30:23
 * LastEditors  : Author Name
 * LastEditTime : 2020-12-09 03:13:50
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
