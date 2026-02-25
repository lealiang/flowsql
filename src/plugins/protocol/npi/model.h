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
 * Date         : 2021-01-29 02:20:25
 * LastEditors  : Pericles
 * LastEditTime : 2021-03-09 02:47:53
 */

#ifndef _FAST_PLUGIN_NPI_MODEL_H_
#define _FAST_PLUGIN_NPI_MODEL_H_

#include <stdint.h>
#include <map>
#include <string>

namespace flowsql {
namespace protocol {

// Declaration
struct Item;
struct Feature;
struct Definition;
class Engine;

class Model {
 public:
    class KeyTyper {
     public:
        KeyTyper();
        int32_t operator()(const char* key);

     protected:
        std::map<std::string, int32_t> keytype_;
    };

    class ValueTyper {
     public:
        ValueTyper();
        int32_t operator()(const char* key);

     protected:
        std::map<std::string, int32_t> valtype_;
    };

    explicit Model(Engine* engine);
    int32_t operator()(Definition* defs);
    static int32_t GetKeyType(const char* key);
    static int32_t GetValueType(const char* key);

 protected:
    int32_t Convert(int32_t number, Feature* feature);

 protected:
    Engine* engine_ = nullptr;
};

}  // namespace protocol
}  // namespace flowsql

#endif  //_FAST_PLUGIN_NPI_MODEL_H_
