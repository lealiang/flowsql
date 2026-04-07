/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_IPROTOCOL_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_IPROTOCOL_H_

#include <common/guid.h>
#include <common/network/netbase.h>
#include <common/typedef.h>
#include <stdint.h>
#include <functional>
#include <memory>

namespace flowsql {
// {8F701981-9915-42BE-8126-186FE17449F3}
const Guid IID_PROTOCOL = {0x8f701981, 0x9915, 0x42be, {0x81, 0x26, 0x18, 0x6f, 0xe1, 0x74, 0x49, 0xf3}};

namespace protocol {

const int32_t MAX_LAYERS = 15;

enum eNumber { POSSIBLE = -1, UNKNOWN = 0 };

struct Entry {
    int32_t number;
    int32_t parents;
    const char* name;
    const char* desc_en;
    const char* desc_ch;
};

struct Layers {
    uint16_t layercount;  // Layer count
    uint16_t payload;
    struct {
        uint16_t offset;
        eLayer layer;  // eLayer
    } layers[MAX_LAYERS];

    union Levels {
        uint64_t qword = 0;
        struct {
            uint8_t count;
            uint8_t degree[7];
        };
    };

    inline Levels operator() (uint8_t layer) {
        Levels levels;
        for (int32_t dgr = 0; dgr < layercount; ++dgr) {
            if (layer == layers[dgr].layer) {
                levels.degree[levels.count++] = dgr;
            }
        }
        return levels;
    }

    inline Levels operator() (uint8_t layer1, uint8_t layer2) {
        Levels levels;
        for (int32_t dgr = 0; dgr < layercount; ++dgr) {
            if (layer1 == layers[dgr].layer || layer1 == layers[dgr].layer) {
                levels.degree[levels.count++] = dgr;
            }
        }
        return levels;
    }

    template <typename Header>
    inline const Header* Get(const uint8_t* packet, int32_t packet_size, uint8_t degree) const {
        if (degree < layercount) {
            return reinterpret_cast<const Header*>(packet + layers[degree].offset);
        }

        return nullptr;
    }

    template <typename Header>
    inline const Header* Forward(const uint8_t* packet, int32_t packet_size) const {
        for (int32_t dgr = 0; dgr < layercount; ++dgr) {
            if (Header::level == layers[dgr].layer) {
                return reinterpret_cast<const Header*>(packet + layers[dgr].offset);
            }
        }
        return nullptr;
    }

    template <typename Header>
    inline const Header* Backward(const uint8_t* packet, int32_t packet_size) const {
        for (int32_t dgr = layercount - 1; dgr >= 0; --dgr) {
            if (Header::level == layers[dgr].layer) {
                return reinterpret_cast<const Header*>(packet + layers[dgr].offset);
            }
        }
        return nullptr;
    }

    template <typename Header>
    inline const Header* Top(const uint8_t* packet, int32_t packet_size) const {
        return reinterpret_cast<const Header*>(packet + layers[layercount - 1].offset);
    }

    inline eLayer Top() const { return layers[layercount - 1].layer; }

    // Payload
    inline uint16_t Payload(const uint8_t* packet, int32_t packet_size) const { return packet_size - payload; }
    inline const uint8_t* Data(const uint8_t* packet, int32_t packet_size) const { return packet + payload; }
};

struct Protocol {
    Protocol() : id(0), subid(0) {}
    Protocol(uint16_t proto) : id(proto), subid(0) {}
    Protocol(uint16_t proto, uint16_t subpro) : id(proto), subid(subpro) {}
    inline operator uint32_t() { return (id << 16) | subid; }

    uint16_t id;     // protocol number, like ICMP
    uint16_t subid;  // sub protocol number, like ICMP-ECHO
};

/**
 * @brief 协议词典接口，提供协议条目查询与遍历能力。
 */
interface IDictionary {
    /**
     * @brief 获取条目数量。
     * @return 协议条目数。
     */
    virtual int32_t Count() const = 0;
    /**
     * @brief 按协议编号查询条目。
     * @param number 协议编号。
     * @return 协议条目指针，未找到返回 nullptr。
     */
    virtual const Entry* Query(int32_t number) const = 0;
    /**
     * @brief 遍历所有协议条目。
     * @param traverser 遍历回调，参数为条目指针，返回非 0 可中断遍历。
     * @return 遍历状态码。
     */
    virtual int32_t Traverse(std::function<int32_t(const Entry*)> traverser) const = 0;
};

}  // namespace protocol

/**
 * @brief 协议识别主接口，负责并发配置、协议识别与层解析。
 */
interface IProtocol {
    virtual ~IProtocol() {}

    /**
     * @brief 设置协议识别并发度。
     * @param number 并发 worker 数量。
     */
    virtual void Concurrency(int32_t number) = 0;
    /**
     * @brief 识别报文协议编号。
     * @param pipeno 管线编号。
     * @param packet 报文字节指针。
     * @param packet_size 报文字节长度。
     * @param layers 已解析层信息。
     * @return 协议标识（id/subid 组合）；id=0 表示未知协议。
     */
    virtual protocol::Protocol Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                        const protocol::Layers* layers) = 0;

    /**
     * @brief 执行分层解析并填充层信息。
     * @param pipeno 管线编号。
     * @param packet 报文字节指针。
     * @param packet_size 报文字节长度。
     * @param layers 输入输出层结构。
     * @return 成功解析的层数，<0 表示失败。
     */
    virtual int32_t Layer(int32_t pipeno, const uint8_t* packet, int32_t packet_size, protocol::Layers* layers) = 0;

    /**
     * @brief 获取协议词典接口。
     * @return 词典接口指针（非拥有语义）。
     */
    virtual protocol::IDictionary* Dictionary() = 0;
};

}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_IPROTOCOL_H_
