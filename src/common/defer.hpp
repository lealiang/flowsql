
/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-06-29 14:41:37
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FLOWSQL_COMMON_DEFER_HPP_
#define _FLOWSQL_COMMON_DEFER_HPP_

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

#endif  //_FLOWSQL_COMMON_DEFER_HPP_
