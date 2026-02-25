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
 * Date         : 2021-03-10 22:10:44
 * LastEditors  : Pericles
 * LastEditTime : 2021-03-10 22:27:07
 */

#ifndef _FAST_COMMON_ALGO_LIST_BASE_HPP_
#define _FAST_COMMON_ALGO_LIST_BASE_HPP_

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

#endif  //_FAST_COMMON_ALGO_LIST_BASE_HPP_
