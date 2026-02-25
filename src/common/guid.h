/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-09-12 22:58:01
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_COMMON_GUID_H_
#define _FLOWSQL_COMMON_GUID_H_
#include <stdint.h>
#include <string.h>
#include <iomanip>
#include <sstream>
namespace flowsql {
struct Guid {
    uint32_t d1_;
    uint16_t d2_;
    uint16_t d3_;
    uint8_t d4_[8];
};

inline bool operator<(const Guid& l, const Guid& r) { return ::memcmp(&l, &r, sizeof(Guid)) < 0; }

// {0xd2f6c761, 0x5d72, 0x4f89, {0xb2, 0x2e, 0x92, 0x61, 0x72, 0x3f, 0xc1, 0x7d}};
template <typename charwide>
std::basic_ostream<charwide>& operator<<(std::basic_ostream<charwide>& ss, const Guid& guid) {
    ss << '{';
    ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << guid.d1_ << ',' << ' ';
    ss << "0x" << std::hex << std::setw(4) << std::setfill('0') << guid.d2_ << ',' << ' ';
    ss << "0x" << std::hex << std::setw(4) << std::setfill('0') << guid.d3_ << ',' << ' ';
    ss << '{';
    ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)guid.d4_[0] << ',' << ' ';
    ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)guid.d4_[1] << ',' << ' ';
    ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)guid.d4_[2] << ',' << ' ';
    ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)guid.d4_[3] << ',' << ' ';
    ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)guid.d4_[4] << ',' << ' ';
    ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)guid.d4_[5] << ',' << ' ';
    ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)guid.d4_[6] << ',' << ' ';
    ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)guid.d4_[7];
    ss << '}';
    ss << '}';
    return ss;
}

// {0xd2f6c761, 0x5d72, 0x4f89, {0xb2, 0x2e, 0x92, 0x61, 0x72, 0x3f, 0xc1, 0x7d}};
template <typename charwide>
std::basic_istream<charwide>& operator>>(std::basic_istream<charwide>& ss, Guid& guid) {
    char sep;
    ss >> sep;  // {
                // -std=c++14
    auto fnget = [&ss](auto& val) -> std::basic_istream<charwide>& {
        char prefix1, prefix2;
        ss >> prefix1;
        switch (prefix1) {
            case '0':
                ss >> prefix2;
                switch (prefix2) {
                    case 'x':
                    case 'X':
                        ss >> std::hex >> val;
                        break;
                    default:
                        ss.unget();
                        ss >> std::oct >> val;
                        break;
                }
                break;
            case 'x':
            case 'X':
                ss >> std::hex >> val;
                break;
            default:
                ss.unget();
                ss >> std::dec >> val;
                break;
        }

        return ss;
    };

    fnget(guid.d1_) >> sep;         // ,
    fnget(guid.d2_) >> sep;         // ,
    fnget(guid.d3_) >> sep >> sep;  // , {
    for (int b = 0; b < 8; ++b) {
        uint16_t bv = 0;
        fnget(bv) >> sep;  // ,
        guid.d4_[b] = bv & 0xff;
    }
    ss >> sep;  // }

    return ss;
}

}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_GUID_H_
