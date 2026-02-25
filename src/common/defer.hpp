
/*
 * Copyright (C) 2020-06 - Klib by Dongkun
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Author       : Dongkun Created
 * Date         : 2020-06-29 14:41:37
 * LastEditors  : lealiang@qq.com
 * LastEditTime : 2020-09-02 20:54:12
 */

#ifndef _FAST_COMMON_DEFER_H_
#define _FAST_COMMON_DEFER_H_

#include <functional>

namespace flowsql {
class scope_guard_t {
 public:
    explicit scope_guard_t(std::function<void()> on_scope_code) : m_on_scope_code(on_scope_code), m_dismissed(false) {}

    ~scope_guard_t() {
        if (!m_dismissed) {
            m_on_scope_code();
        }
    }

    void dismiss() { m_dismissed = true; }

 private:
    std::function<void()> m_on_scope_code;
    bool m_dismissed;

 private:  // noncopyable
    scope_guard_t(scope_guard_t const&);
    scope_guard_t& operator=(scope_guard_t const&);
};

}  // namespace flowsql

#define SCOPEGUARD_LINENAME_CAT(name, line) name##line
#define SCOPEGUARD_LINENAME(name, line) SCOPEGUARD_LINENAME_CAT(name, line)

#define ON_SCOPE_EXIT(callback) flowsql::scope_guard_t SCOPEGUARD_LINENAME(EXIT, __LINE__)(callback)
#define defer(statement) flowsql::scope_guard_t SCOPEGUARD_LINENAME(EXIT, __LINE__)([&]() { statement; })

#endif  //_FAST_COMMON_DEFER_H_
