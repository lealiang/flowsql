/*
 * Copyright (C) 2020-06 - flowSQL
 * 
 * 
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 * the Free Software Foundation; either version 3 of the License, or 
 * 
 * 
 * Author       : LIHUO
 * Date         : 2021-11-10 12:47:28
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FLOWSQL_COMMON_NETWORK_NETADDRESS_H_
#define _FLOWSQL_COMMON_NETWORK_NETADDRESS_H_

#ifndef __APPLE__
#include <byteswap.h>
#endif
#include <arpa/inet.h>  // inet_addr
#include <stdint.h>
#include <string.h>
#include <string>
#include "../typedef.h"

/* ethernet address */
struct EtherAdderss {
    union {
        uint8_t bytes[6];
        uint16_t words[3];
    };

    EtherAdderss() : words() {}

    friend inline bool operator<(const EtherAdderss& l, const EtherAdderss& r) {
        return 0 > memcmp(l.bytes, r.bytes, sizeof(EtherAdderss));
    }

    friend inline bool operator==(const EtherAdderss& l, const EtherAdderss& r) {
        return 0 == memcmp(l.bytes, r.bytes, sizeof(EtherAdderss));
    }

    operator bool() const {
        for (size_t index = 0; index < sizeof(words) / (sizeof(words[0])); index++) {
            if (words[index] > 0) {
                return true;
            }
        }
        return false;
    }

    bool load(const char* mac_str) {
        const int span = 1;
        for (int index = 0; index < 6; index++) {
            int v = 0;
            sscanf(mac_str + index * span, "%02x", &v);
            this->bytes[index] = (uint8_t)v;
        }
        return true;
    }

    std::string to_string() const {
        char mac_str[18];
        int len = snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", this->bytes[0], this->bytes[1],
                           this->bytes[2], this->bytes[3], this->bytes[4], this->bytes[5]);
        if (len > 0) {
            mac_str[len] = '\0';
        }

        return mac_str;
    }

} FAST_MEM_ALIGN(1);

/* IPv4 address */
struct IPv4Address {
    union {
        uint32_t dword;
        uint32_t addr;
        struct {
            uint8_t byte1;
            uint8_t byte2;
            uint8_t byte3;
            uint8_t byte4;
        };
        uint8_t bytes[4];
    };

    IPv4Address() : addr() {}
    IPv4Address(uint32_t ip) : addr(ip){};
    IPv4Address(const IPv4Address& other) { this->addr = other.addr; }
    IPv4Address& operator=(const IPv4Address& other) {
        this->addr = other.addr;
        return *this;
    }
    void from(const char* addr_str) {
        if (addr_str) {
            this->addr = inet_addr(addr_str);
        }
    }

    operator std::string() { return std::move(this->to_str()); }

    inline std::string to_str() {
        char ip_str[20];
        inet_ntop(AF_INET, (void*)&(this->addr), ip_str, sizeof(ip_str));
        return std::string(ip_str);
    }

    operator uint32_t&() { return this->addr; }
    operator bool() const { return this->addr > 0; }
    friend inline bool operator<(const IPv4Address& l, const IPv4Address& r) { return l.dword < r.dword; }
    friend inline bool operator<=(const IPv4Address& l, const IPv4Address& r) { return l.dword <= r.dword; }
    friend inline bool operator>(const IPv4Address& l, const IPv4Address& r) { return l.dword > r.dword; }
    friend inline bool operator>=(const IPv4Address& l, const IPv4Address& r) { return l.dword >= r.dword; }

    friend inline bool operator==(const IPv4Address& l, const IPv4Address& r) { return l.dword == r.dword; }
    friend inline bool operator!=(const IPv4Address& l, const IPv4Address& r) { return l.dword != r.dword; }

} FAST_MEM_ALIGN(1);
typedef IPv4Address IPAddress;

/* IPv6 address */
struct IPv6Address {
    union {
        uint8_t bytes[16];
        uint16_t words[8];
        uint32_t dwords[4];
        uint64_t qwords[2];
        uint128_t addr;
    };
    IPv6Address() : addr() {}

    operator std::string() { return std::move(this->to_str()); }
    operator uint128_t() { return this->addr; }

    inline std::string to_str() {
        char ip_str[50];
        inet_ntop(AF_INET6, (void*)&(this->addr), ip_str, sizeof(ip_str));
        return std::string(ip_str);
    }

    friend inline bool operator<(const IPv6Address& l, const IPv6Address& r) {
        return l.addr < r.addr;
    }
    friend inline bool operator>(const IPv6Address& l, const IPv6Address& r) {
        return l.addr > r.addr;
    }

    friend inline bool operator==(const IPv6Address& l, const IPv6Address& r) {
        return l.addr == r.addr;
    }
} FAST_MEM_ALIGN_PACKED;

#endif // _FLOWSQL_COMMON_NETWORK_NETADDRESS_H_