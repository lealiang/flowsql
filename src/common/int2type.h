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
 * Author       : lealiang@qq.com
 * Date         : 2020-09-08 02:28:54
 * LastEditors  : lealiang@qq.com
 * LastEditTime : 2020-09-08 02:30:41
 */
#ifndef _FAST_BASE_INT2TYPE_H
#define _FAST_BASE_INT2TYPE_H

#include <stdint.h>

template <int32_t val>
struct int2type {
    enum { value = val };
};

#endif  //_FAST_BASE_INT2TYPE_H
