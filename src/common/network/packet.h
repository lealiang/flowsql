/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-08-20 21:03:55
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FAST_COMMON_PACKET_H
#define _FAST_COMMON_PACKET_H

#include <new>

#include "../fast/power.hpp"

#include "list.h"
#include "macro.h"
#include "share_ptr.h"
#include "tcpip.h"

#include "object_list.h"
#include "tunnel.h"

namespace flowsql {

struct port_range {
    uint16_t start_port_;
    uint16_t end_port_;
};

/*
sizeof PacketContect = 5 * 8 = 40
*/
struct PacketContext {
    struct list_head node_;  // node of the list
    uint8_t error;           // error type
    uint16_t itfs;           // interface or virtual interface
    uint32_t type;           // packet_type in rte_mbuf by DPDK lib\librte_mbuf\rte_mbuf_ptype.h
    uint32_t hashcode;       // hash code calc by hardware supporting RSS
    uint64_t timestamp_ns;   // nano seconds
    uint64_t number;         // packet unique sequnce
    uint64_t stream_id;      // stream id, @todo add link id
    uint32_t original_len;   // 原始数据包长度(>=caplen)
    uint32_t caplen;         // packet length(抓取到的数据包的长度)
    uint32_t timesec;        // utc second
    uint16_t data_offset;    // hash key length

    tunnel_info_arr_t tunnel_arr;  // turnnel info array
} FAST_CACHELINE_ALIGN;

/*
Packet Data:
|-------------------------------------------------|
|--context--hashkey--packetdata-------------------|
*/
struct Packet : public PacketContext {
    enum { BaseSize = sizeof(PacketContext) };

    ///////////////////////////////////////////////////////
    // helper interface
    inline uint8_t* data() { return data_ + data_offset; }

    inline const uint8_t* data() const { return data_ + data_offset; }

    inline int32_t size() { return BaseSize + data_offset + caplen; }

    template <typename HashKey>
    inline HashKey* hash() {
        return (HashKey*)data_;
    }

    template <typename HashKey>
    inline const HashKey* hash() const {
        return (HashKey*)data_;
    }

    inline int32_t direction() const { return ((IHashKey*)data_)->reversed; }

    inline EthernetHeader* ethernet() {
        for (int index = this->tunnel_arr.tunnel_count_ - 1; index >= 0; index--) {
            auto& turnn_info = this->tunnel_arr.infos_[index];
            if (turnn_info.category & (uint8_t)ePacketCategory::MAC) {
                return (EthernetHeader*)(data_ + data_offset + turnn_info.l2offset);
            }
        }

        return (EthernetHeader*)(data_ + data_offset);
    }

    inline const EthernetHeader* ethernet() const {
        for (int index = this->tunnel_arr.tunnel_count_ - 1; index >= 0; index--) {
            auto& turnn_info = this->tunnel_arr.infos_[index];
            if (turnn_info.category & (uint8_t)ePacketCategory::MAC) {
                return (EthernetHeader*)(data_ + data_offset + turnn_info.l2offset);
            }
        }

        return (EthernetHeader*)(data_ + data_offset);
    }

    inline IPv4Header* ipv4() {
        auto turnn_info = this->tunnel_arr.top();
        auto category = turnn_info->category;
        if (category & (uint8_t)ePacketCategory::IPv4) {
            if (turnn_info->l3offset > 0) {
                return (IPv4Header*)(data_ + data_offset + turnn_info->l3offset);
            }
        }
        return nullptr;
    }

    inline const IPv4Header* ipv4() const {
        auto turnn_info = this->tunnel_arr.top();
        auto category = turnn_info->category;
        if (category & (uint8_t)ePacketCategory::IPv4) {
            if (turnn_info->l3offset > 0) {
                return (IPv4Header*)(data_ + data_offset + turnn_info->l3offset);
            }
        }
        return nullptr;
    }

    inline IPv6Header* ipv6() {
        auto turnn_info = this->tunnel_arr.top();
        auto category = turnn_info->category;
        if (category & (uint8_t)ePacketCategory::IPv6) {
            if (turnn_info->l3offset > 0) {
                return (IPv6Header*)(data_ + data_offset + turnn_info->l3offset);
            }
        }
        return nullptr;
    }

    inline const IPv6Header* ipv6() const {
        auto turnn_info = this->tunnel_arr.top();
        auto category = turnn_info->category;
        if (category & (uint8_t)ePacketCategory::IPv6) {
            if (turnn_info->l3offset > 0) {
                return (IPv6Header*)(data_ + data_offset + turnn_info->l3offset);
            }
        }
        return nullptr;
    }

    inline UDPHeader* udp() {
        auto turnn_info = this->tunnel_arr.top();
        if (turnn_info->category & (uint8_t)ePacketCategory::UDP) {
            return (UDPHeader*)(data_ + data_offset + turnn_info->l4offset);
        }
        return nullptr;
    }

    inline const UDPHeader* udp() const {
        auto turnn_info = this->tunnel_arr.top();
        if (turnn_info->category & (uint8_t)ePacketCategory::UDP) {
            return (UDPHeader*)(data_ + data_offset + turnn_info->l4offset);
        }
        return nullptr;
    }

    inline TCPHeader* tcp() {
        auto turnn_info = this->tunnel_arr.top();
        if (turnn_info->category & (uint8_t)ePacketCategory::TCP) {
            return (TCPHeader*)(data_ + data_offset + turnn_info->l4offset);
        }
        return nullptr;
    }

    inline const TCPHeader* tcp() const {
        auto turnn_info = this->tunnel_arr.top();
        if (turnn_info->category & (uint8_t)ePacketCategory::TCP) {
            return (TCPHeader*)(data_ + data_offset + turnn_info->l4offset);
        }
        return nullptr;
    }

    inline uint32_t payloadlength() const { return this->tunnel_arr.top()->payload_length; }

    inline const uint8_t* payload() const {
        auto top_info = this->tunnel_arr.top();

        uint8_t* ptr = (uint8_t*)&data_[0] + data_offset;
        return ptr + top_info->payload;
    }

    /**
     * 在buff_ptr上复制Packet结构
     */
    Packet* clone(uint8_t* buff_ptr, int buff_len) {
        uint32_t tlen = this->size();
        if (buff_len >= tlen) {
            memcpy(buff_ptr, this, tlen);
            return (Packet*)buff_ptr;
        }
        return nullptr;
    }

 protected:
    uint8_t data_[0];  // hashkey + packet
};

struct PacketPtr : public share_base, public Packet {
    uint8_t data_[2 * 1024];
};

/**
 * 相对较大的Buffer用于平铺Packet
 * 1. support reference count
 * 2. support iterator
 */
class PacketBuffer : public share_base {
 public:
    PacketBuffer(uint32_t buff_len);
    ~PacketBuffer();

    typedef typename object_list<Packet>::iterator iterator;

    /**
     * Book a Packet to store data
     */
    template <typename HashKey>
    Packet* Book(uint32_t payload_length) {
        Packet* pkt = 0;
        uint8_t* ptr_start = (uint8_t*)this->buff_ptr_ + this->used_len_;
        uint8_t* ptr_pkt = (uint8_t*)((uint64_t)((char*)ptr_start + 8 - 1) & (~(uint64_t)(8 - 1)));  // 8字节的内存对齐

        uint32_t offset = ptr_pkt - ptr_start;  // + offset
        uint32_t require_len = sizeof(Packet) + sizeof(HashKey) + payload_length;
        require_len += offset;

        if (this->free_len() >= require_len) {
            pkt = (Packet*)ptr_pkt;
            prefetch_range(pkt, sizeof(Packet) + sizeof(HashKey) + payload_length, 1, 3);

            new (pkt) Packet;
            pkt->data_offset = sizeof(HashKey);

            this->used_len_ += require_len;
            this->packet_count_ += 1;

            this->list_.push_back(pkt);
        }

        return pkt;
    }

    inline uint32_t used_length() const { return this->used_len_; }

    inline uint32_t free_len() const { return this->buff_len_ - this->used_len_; }

    inline iterator begin() { return this->list_.begin(); }

    inline iterator end() { return this->list_.end(); }

    inline uint16_t size() const { return this->list_.size(); }

 protected:
    uint32_t buff_len_;      // whole buffer length
    uint32_t used_len_;      // used buffer length
    uint16_t packet_count_;  // whole store packet count

    object_list<Packet> list_;

    uint8_t buff_ptr_[0];  // PacketBuffer buffer pointer
};                         // namespace flowsql

typedef share_ptr<PacketBuffer> PacketBufferPtr;

typedef object_list<Packet> PacketList;

class PacketGenerator {
 public:
    /*
        One packet_generator_t per thread
        if parallel == 32
        Max pps: 2^(24 + 8 - log32) = 2^27 = 128M
    */
    PacketGenerator(int32_t parallel, int32_t tno)
        : thread_index_(tno), exponent_(get_exponent_number(parallel)), sequence_(0) {}

    // For DPDK
    int32_t operator()(Packet* packet,                                    /*In Out*/
                       const uint8_t* packet_ptr,                         /*In*/
                       uint32_t caplen,                                   /*In*/
                       uint64_t timestamp,                                /*In*/
                       uint16_t portid,                                   /*In*/
                       uint32_t type, uint8_t category, uint32_t hashcode /*In*/
    ) {
        packet->error = 0;
        packet->itfs = portid;
        packet->type = type;
        packet->hashcode = hashcode;
        packet->timestamp_ns = timestamp;
        packet->original_len = caplen;
        packet->caplen = caplen;
        packet->timesec = timestamp / 1000000000;
        packet->number = packet->timesec;
        packet->number = (packet->number << 32) + (thread_index_ << (32 - exponent_)) + (sequence_++);
        packet->tunnel_arr.parsed_ = false;

        return _init_packet_context(packet_ptr, packet, hashcode);
    }

 private:
    inline int32_t _get_ipv4(Ipv4HashKey* ipv4_hash, EthernetHeader* eth_head, IPv4Header*& ipv4Hdr,
                             const uint8_t* packet_ptr, Packet* packet, uint32_t head_offset) {
        if (0x0081 == eth_head->protocol) {
            VLAN_802_1Q* vlan_head = (VLAN_802_1Q*)((uint8_t*)eth_head + head_offset);
            head_offset += sizeof(VLAN_802_1Q);

            if (0x0008 == vlan_head->type) {
                ipv4Hdr = (IPv4Header*)(packet_ptr + head_offset);
            } else if (0x0081 == vlan_head->type) {
                // 暂时只处理两层vlan的情况
                vlan_head = (VLAN_802_1Q*)((uint8_t*)eth_head + head_offset);
                head_offset += sizeof(VLAN_802_1Q);
                if (0x0008 == vlan_head->type) {
                    ipv4Hdr = (IPv4Header*)(packet_ptr + head_offset);
                }
            }
        } else if (0x0008 == eth_head->protocol) {
            ipv4Hdr = (IPv4Header*)(packet_ptr + head_offset);
        }

        packet->data_offset = sizeof(Ipv4HashKey);
        ipv4_hash->src_port = 0;
        ipv4_hash->dst_port = 0;
        if (likely(ipv4Hdr == 0)) {
            return 0;
        }

        ipv4_hash->src_ip.dword = ipv4Hdr->src_addr.dword;
        ipv4_hash->dst_ip.dword = ipv4Hdr->dst_addr.dword;

        return ipv4Hdr->ihl << 2;
    }

    inline int32_t _get_ipv6(Ipv6HashKey* ipv6_hash, EthernetHeader* eth_head, IPv6Header*& ipv6Hdr,
                             const uint8_t* packet_ptr, Packet* packet, uint32_t& head_offset) {
        if (0x0081 == eth_head->protocol) {
            VLAN_802_1Q* vlan_head = (VLAN_802_1Q*)((uint8_t*)eth_head + head_offset);
            head_offset += sizeof(VLAN_802_1Q);

            if (0xDD86 == vlan_head->type) {
                ipv6Hdr = (IPv6Header*)(packet_ptr + head_offset);
            } else if (0x0081 == vlan_head->type) {
                // 暂时只处理两层vlan的情况
                vlan_head = (VLAN_802_1Q*)((uint8_t*)eth_head + head_offset);
                head_offset += sizeof(VLAN_802_1Q);
                if (0xDD86 == vlan_head->type) {
                    ipv6Hdr = (IPv6Header*)(packet_ptr + head_offset);
                }
            }
        } else if (0xDD86 == eth_head->protocol) {
            ipv6Hdr = (IPv6Header*)(packet_ptr + head_offset);
        }
        packet->data_offset = sizeof(Ipv6HashKey);
        ipv6_hash->src_port = 0;
        ipv6_hash->dst_port = 0;

        if (unlikely(ipv6Hdr == 0)) {
            return 0;
        }

        ipv6_hash->src_ip.qwords[0] = ipv6Hdr->src_addr.qwords[0];
        ipv6_hash->src_ip.qwords[1] = ipv6Hdr->src_addr.qwords[1];
        ipv6_hash->dst_ip.qwords[0] = ipv6Hdr->dst_addr.qwords[0];
        ipv6_hash->dst_ip.qwords[1] = ipv6Hdr->dst_addr.qwords[1];
        return sizeof(IPv6Header);
    }

    template <typename HashKey>
    inline int32_t _get_tcp(HashKey* hashkey, const uint8_t* packet_ptr, Packet* packet, uint32_t head_offset,
                            int32_t ippayloadlen) {
        TCPHeader* tcpHdr = (TCPHeader*)(packet_ptr + head_offset);
        hashkey->src_port = tcpHdr->src_port;
        hashkey->dst_port = tcpHdr->dst_port;
        return sizeof(TCPHeader);
    }

    template <typename HashKey>
    inline int32_t _get_udp(HashKey* hashkey, const uint8_t* packet_ptr, Packet* packet, uint32_t head_offset,
                            int32_t ippayloadlen) {
        UDPHeader* udpHdr = (UDPHeader*)(packet_ptr + head_offset);
        hashkey->src_port = udpHdr->src_port;
        hashkey->dst_port = udpHdr->dst_port;
        return sizeof(UDPHeader);
    }

    int32_t _init_packet_context(const uint8_t* packet_ptr, Packet* packet, uint32_t hashcode) {
        uint32_t head_offset = sizeof(EthernetHeader);
        EthernetHeader* eth_head = (EthernetHeader*)packet_ptr;
        IPv4Header* ipv4Hdr = nullptr;
        IPv6Header* ipv6Hdr = nullptr;
        MacHashKey* mac_hash = new (packet->hash<MacHashKey>()) MacHashKey();
        Ipv4HashKey* ipv4_hash = new (packet->hash<Ipv4HashKey>()) Ipv4HashKey(hashcode);
        Ipv6HashKey* ipv6_hash = new (packet->hash<Ipv6HashKey>()) Ipv6HashKey(hashcode);
        int32_t ippayload = 0;
        switch (1) {  // packet->category
            case e2i(ePipeCategory::L2): {
                memcpy(mac_hash->src_addr.bytes, eth_head->src_addr.bytes, 6);
                memcpy(mac_hash->dst_addr.bytes, eth_head->dst_addr.bytes, 6);
                mac_hash->Order();

                // Copy data
                packet->data_offset = sizeof(MacHashKey);
                memcpy(packet->data(), packet_ptr, packet->caplen);
                break;
            }
            case e2i(ePipeCategory::L3_IPV4): {
                // auto l3headlen = _get_ipv4(ipv4_hash, eth_head, ipv4Hdr, packet_ptr, packet, head_offset);
                ipv4_hash->Order();
                memcpy(packet->data(), packet_ptr, packet->caplen);
                break;
            }
            case e2i(ePipeCategory::L4_IPV4_TCP):
                head_offset += _get_ipv4(ipv4_hash, eth_head, ipv4Hdr, packet_ptr, packet, head_offset);
                if (likely(ipv4Hdr != nullptr) && ipv4Hdr->total_length) {
                    ippayload = n2h16(ipv4Hdr->total_length) - (ipv4Hdr->ihl << 2);
                } else {
                    ippayload = packet->caplen - head_offset;
                }
                _get_tcp<Ipv4HashKey>(ipv4_hash, packet_ptr, packet, head_offset, ippayload);
                ipv4_hash->Order();
                memcpy(packet->data(), packet_ptr, packet->caplen);
                break;
            case e2i(ePipeCategory::L4_IPV4_UDP):
                head_offset += _get_ipv4(ipv4_hash, eth_head, ipv4Hdr, packet_ptr, packet, head_offset);
                if (likely(ipv4Hdr != nullptr) && ipv4Hdr->total_length) {
                    ippayload = n2h16(ipv4Hdr->total_length) - (ipv4Hdr->ihl << 2);
                } else {
                    ippayload = packet->caplen - head_offset;
                }
                _get_udp<Ipv4HashKey>(ipv4_hash, packet_ptr, packet, head_offset, ippayload);
                ipv4_hash->Order();
                memcpy(packet->data(), packet_ptr, packet->caplen);
                break;
            case e2i(ePipeCategory::L3_IPV6):
                _get_ipv6(ipv6_hash, eth_head, ipv6Hdr, packet_ptr, packet, head_offset);
                ipv6_hash->Order();
                memcpy(packet->data(), packet_ptr, packet->caplen);
                break;
            case e2i(ePipeCategory::L4_IPV6_TCP):
                head_offset += _get_ipv6(ipv6_hash, eth_head, ipv6Hdr, packet_ptr, packet, head_offset);
                if (likely(ipv6Hdr != nullptr)) {
                    _get_tcp<Ipv6HashKey>(ipv6_hash, packet_ptr, packet, head_offset, n2h16(ipv6Hdr->payload));
                }
                ipv6_hash->Order();
                memcpy(packet->data(), packet_ptr, packet->caplen);
                break;
            case e2i(ePipeCategory::L4_IPV6_UDP):
                head_offset += _get_ipv6(ipv6_hash, eth_head, ipv6Hdr, packet_ptr, packet, head_offset);
                if (likely(ipv6Hdr != nullptr)) {
                    _get_udp<Ipv6HashKey>(ipv6_hash, packet_ptr, packet, head_offset, n2h16(ipv6Hdr->payload));
                }
                ipv6_hash->Order();
                memcpy(packet->data(), packet_ptr, packet->caplen);
                break;
            default:
                return -1;
        }

        return 0;
    }

 private:
    uint32_t thread_index_;
    uint32_t exponent_;
    uint32_t sequence_;
};

}  // namespace flowsql

#endif  // _FAST_COMMON_PACKET_H
