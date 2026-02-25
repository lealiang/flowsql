/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-06-29 16:52:48
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FAST_COMMON_ALGO_LIST_HPP_
#define _FAST_COMMON_ALGO_LIST_HPP_

#include <functional>
#include "../threadsafe/lock.h"
#include "list_base.hpp"

namespace flowsql {

template <typename Node, typename LockType = flowsql::lockless>
class SinglyList {
 protected:
    SinglyList(const SinglyList &);
    SinglyList &operator=(const SinglyList &);

 public:
    SinglyList(std::function<void(Node *)> deleter = nullptr)
        : deleter_(deleter), head_(nullptr), tail_(nullptr), size_(0) {}

    SinglyList(SinglyList &&right)
        : deleter_(right.deleter_), head_(right.head_), tail_(right.tail_), size_(right.size_) {
        right.deleter_ = nullptr;
        right.head_ = nullptr;
        right.tail_ = nullptr;
        right.size_ = 0;
    }

    ~SinglyList() {
        Clear(deleter_);
        deleter_ = nullptr;
    }

    inline void Clear(std::function<void(Node *)> deleter = nullptr) {
        lock_.lock();
        if (deleter) {
            Node *node = head_;
            while (node) {
                Node *next_node = (Node *)node->next;
                deleter(node);
                node = next_node;
            }
        }
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
        lock_.unlock();
    }

    inline Node *Front() { return head_; }

    inline Node *Back() { return tail_; }

    inline Node *Swap() {
        lock_.lock();
        Node *swap_list = head_;
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
        lock_.unlock();
        return swap_list;
    }

    inline void PushFront(Node *node) {
        if (node) {
            lock_.lock();
            if (nullptr == head_) {
                head_ = node;
                tail_ = node;
                size_ = 1;
                lock_.unlock();
                return;
            }

            node->next = head_;
            head_ = node;
            size_ += 1;
            lock_.unlock();
        }
    }

    inline void PushBack(Node *node) {
        if (node) {
            lock_.lock();
            node->next = nullptr;
            if (nullptr == head_) {
                head_ = node;
                tail_ = node;
                size_ = 1;
                lock_.unlock();
                return;
            }

            tail_->next = node;
            tail_ = node;
            size_ += 1;
            lock_.unlock();
        }
    }

    template <typename OtherNode, typename OtherLockType>
    inline void Join(SinglyList<OtherNode, OtherLockType> &&right) {
        lock_.lock();

        Node *r_head = (Node *)right.head();
        Node *r_tail = (Node *)right.tail();
        if (r_head && r_tail) {
            if (head_ && tail_) {
                tail_->next = r_head;
                tail_ = r_tail;
            } else {
                head_ = r_head;
                tail_ = r_tail;
            }
            size_ += right.Size();
        }
        right.Clear();

        lock_.unlock();
    }

    inline void Join(Node *node_head) {
        lock_.lock();

        Node *node = node_head;
        while (node) {
            Node *next_node = (Node *)node->next;
            if (head_ && tail_) {
                tail_->next = node;
                tail_ = (Node *)tail_->next;
                size_ += 1;
            } else {
                head_ = node;
                tail_ = node;
                size_ = 1;
            }

            node = next_node;
        }

        lock_.unlock();
    }

    inline Node *Pop() {
        Node *front_node = nullptr;

        lock_.lock();
        if (head_) {
            front_node = (Node *)head_;
            head_ = (Node *)head_->next;
            size_ -= 1;
        }
        lock_.unlock();

        return front_node;
    }

    inline Node *Head() { return head_; }

    inline Node *Tail() { return tail_; }

    inline bool Empty() { return !head_ || !tail_; }

    inline int64_t Size() { return size_; }

 protected:
    std::function<void(Node *)> deleter_;
    Node *head_;
    Node *tail_;
    int64_t size_;
    LockType lock_;
};

}  // namespace flowsql

#endif  //_FAST_COMMON_ALGO_LIST_HPP_