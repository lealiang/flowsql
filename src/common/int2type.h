/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-09-08 02:28:54
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FAST_BASE_INT2TYPE_H
#define _FAST_BASE_INT2TYPE_H

#include <stdint.h>

template <int32_t val>
struct int2type {
    enum { value = val };
};

#endif  //_FAST_BASE_INT2TYPE_H
