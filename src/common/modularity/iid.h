/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-06-29 11:09:05
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_COMMON_MODULARITY_IID_H_
#define _FLOWSQL_COMMON_MODULARITY_IID_H_

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

#endif  //_FLOWSQL_COMMON_MODULARITY_IID_H_
