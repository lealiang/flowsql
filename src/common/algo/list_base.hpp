/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-03-10 22:10:44
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FLOWSQL_COMMON_ALGO_LIST_BASE_HPP_
#define _FLOWSQL_COMMON_ALGO_LIST_BASE_HPP_

#include <stdint.h>

namespace flowsql {
template <typename Entity>
class SinglyNode {
 public:
    SinglyNode() : next(nullptr) {}

 public:
    Entity *next;
};

template <typename Entity>
class DoublyNode {
 public:
    DoublyNode() : prior(nullptr), next(nullptr) {}

 public:
    Entity *prior;
    Entity *next;
};

template <typename Entity>
class CyclicNode {
 public:
    CyclicNode() : prior(nullptr), next(nullptr) {}

 public:
    Entity *prior;
    Entity *next;
};

namespace list {
template <typename ListNode>
inline int64_t distance(ListNode *begin, ListNode *end) {
    int64_t count = 0;
    if (begin) {
        for (ListNode *dis = begin; dis != end; dis = dis->next) {
            ++count;
        }
    }

    return count;
}

template <typename ListNode>
ListNode *insert_tail(ListNode *ln, ListNode *rn) {
    ListNode *cur = ln;
    while (cur->next) {
        cur = cur->next;
    }
    cur->next = rn;

    return ln;
}

template <typename ListNode>
ListNode *insert_head(ListNode *ln, ListNode *rn) {
    rn->next = ln;
    return rn;
}

}  // namespace list

}  // namespace flowsql

#define LIST_INSERT_TAIL(hnode, newnode)             \
    {                                                \
        if (hnode)                                   \
            flowsql::list::insert_tail(hnode, newnode); \
        else                                         \
            hnode = newnode;                         \
    }

#define LIST_INSERT_HEAD(hnode, newnode)             \
    {                                                \
        if (hnode)                                   \
            flowsql::list::insert_head(hnode, newnode); \
        else                                         \
            hnode = newnode;                         \
    }

#endif  //_FLOWSQL_COMMON_ALGO_LIST_BASE_HPP_
