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
 * Date         : 2021-01-12 16:23:19
 * LastEditors  : ,: Pericles
 * LastEditTime : ,: 2021-01-29 21:29:19
 */

#ifndef _FAST_BASE_ALGO_RADIXTREE_HPP_
#define _FAST_BASE_ALGO_RADIXTREE_HPP_

#include <assert.h>
#include <stdint.h>
#include "objects_pool.hpp"

namespace flowsql {

template <uint8_t bitwidth, typename ValueType>
class RadixTree {
 public:
    class RadixNode {
     public:
        inline RadixNode*& Self(int32_t val) {
            assert(val < width);
            return next_[val];
        }

        inline void Value(ValueType value) { value_ = value; }

        inline ValueType Value() { return value_; }

     private:
        enum { width = 1 << bitwidth };
        ValueType value_ = 0;
        RadixNode* next_[width] = {nullptr};
    };

    template <typename Integer>
    RadixNode* Insert(Integer val) {
        if (!root_) {
            root_ = create_new_node();
        }

        return this->Insert(root_, val);
    }

    template <typename Integer>
    RadixNode* Search(Integer val) {
        if (!root_) {
            return nullptr;
        }

        return this->Search(root_, val);
    }

    template <typename Integer>
    RadixNode* Insert(RadixNode* parent, Integer integer) {
        _Integerbits<Integer> ib(integer);

        auto node = parent;
        if (!node) {
            if (!root_) {
                root_ = create_new_node();
            }
            node = root_;
        }
        
        for (auto val : ib.bits) {
            auto& child = node->Self(val.bit);
            if (!child) {
                child = create_new_node();
            }
            node = child;
        }

        return node;
    }

    template <typename Integer>
    RadixNode* Search(RadixNode* parent, Integer integer) {
        _Integerbits<Integer> ib(integer);

        auto node = parent;
        for (auto val : ib.bits) {
            auto& child = node->Self(val.bit);
            if (!child) {
                return nullptr;
            }
            node = child;
        }

        return node;
    }

 protected:
    inline RadixNode* create_new_node() { return radix_node_pool_.Alloc(); }

 private:
    template <typename Integer>
    struct _Integerbits {
        enum { _range = sizeof(Integer) * 8 / bitwidth };
        explicit _Integerbits(Integer i) : integer(i) {
            union int2bit {
                Integer bitval : bitwidth;
                Integer intval;
            } i2b;
            for (auto os = 0; os < _range; ++os) {
                i2b.intval = i >> (os * bitwidth);
                bits[os].bit = i2b.bitval;
            }
        }

        struct _bits {
            Integer bit : bitwidth;
        } bits[_range];

        Integer integer;
    };

    RadixNode* root_ = nullptr;
    ObjectsPool<RadixNode, 1024> radix_node_pool_;
};

}  // namespace flowsql

#endif  //_FAST_BASE_ALGO_RADIXTREE_HPP_
