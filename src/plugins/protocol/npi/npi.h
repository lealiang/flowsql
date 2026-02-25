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
 * Date         : 2021-01-24 14:04:58
 * LastEditors  : Pericels
 * LastEditTime : 2022-06-24 17:42:33
 */
#ifndef _FAST_PLUGIN_NPI_H_
#define _FAST_PLUGIN_NPI_H_

#include <common/guid.h>
#include <common/typedef.h>
#include <common/loader.hpp>
#include "iprotocol.h"

namespace flowsql {

namespace protocol {
class Dictionary;
class NetworkLayer;
class Engine;
class Config;
}  // namespace protocol

class NetworkProtocolIdentify : public IPlugin, public IProtocol {
 public:
    NetworkProtocolIdentify();
    ~NetworkProtocolIdentify();

    // IPlugin
    virtual int Option(const char* arg);
    virtual int Load();    // do not call any interface in this func.
    virtual int Unload();  // do not call any interface in this func.

    // IProrocol
    virtual void Concurrency(int32_t number);
    /*
    Return value:
       0 : unknown protocol
     > 0 : protocol No.
    */
    virtual protocol::Protocol Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                        const protocol::Layers* layers);

    virtual int32_t Layer(int32_t pipeno, const uint8_t* packet, int32_t packet_size, protocol::Layers* layers);

    virtual protocol::IDictionary* Dictionary();

 protected:
    protocol::Config* config_ = nullptr;
    protocol::NetworkLayer* layer_ = nullptr;
    protocol::Engine* engine_ = nullptr;
};

}  // namespace flowsql

#endif  //_FAST_PLUGIN_NPI_H_
