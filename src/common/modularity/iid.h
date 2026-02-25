/*
 * Copyright (C) 2020-06 - FATEAM
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Author       : lealiang@qq.com
 * Date         : 2020-06-29 11:09:05
 * LastEditors  : lealiang@qq.com
 * LastEditTime : 2020-08-21 01:17:29
 */
#ifndef _FAST_COMMON_MODULARITY_IID_H_
#define _FAST_COMMON_MODULARITY_IID_H_

// You should create your own iid definition file and include this file.
#include <stdint.h>

namespace flowsql {
// interface id
enum class eIID {
    IID_MBASIC = 0, /*Basic module*/
    IID_COMMANDER,
    IID_CONFIGURE_SERVICE,
    IID_CONFIGURE,
    IID_CAPTURE,
    IID_FLOWCENTER,
    IID_STORAGE,
    IID_DELIVER,
    IID_SPLITTER,        /*As a service module*/
    IID_BASIC_STATISTIC, /*A consumer of FLOWCENTER*/
    IID_PLUGINS,         /*A consumer of FLOWCENTER*/

};
}  // namespace flowsql

#endif  //_FAST_COMMON_MODULARITY_IID_H_
