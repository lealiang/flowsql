/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_COMMON_INT2TYPE_H_
#define _FLOWSQL_COMMON_INT2TYPE_H_

#include <stdint.h>

template <int32_t val>
struct int2type {
    enum { value = val };
};

#endif  //_FLOWSQL_COMMON_INT2TYPE_H_
