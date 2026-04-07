/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_IRECOGNIZER_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_IRECOGNIZER_H_

#include <common/network/netbase.h>
#include "iprotocol.h"
#include "layer.h"

namespace flowsql {
namespace protocol {

struct RecognizeContext {
    uint16_t layer = e2i(eLayer::NONE);
    uint16_t level = e2i(eLayer::NONE);
    union {
        uint16_t proto = 0;
        uint16_t dst_port;
        uint16_t w1;
    };
    union {
        uint16_t src_port = 0;
        uint16_t w2;
    };
};

/**
 * @brief 协议识别器接口，根据报文内容识别协议编号。
 */
interface IRecognizer {
    /**
     * @brief 识别输入报文的协议。
     * @param pipeno 处理管线编号。
     * @param packet 报文字节指针。
     * @param packet_size 报文字节长度。
     * @param layers 已解析层信息。
     * @param rctx 识别上下文（输入输出）。
     * @return 协议编号，UNKNOWN 表示未知。
     */
    virtual int32_t Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size, const protocol::Layers* layers,
                             RecognizeContext* rctx) = 0;
    virtual ~IRecognizer() {}
};

/**
 * @brief 固定返回值识别器，常用于链式识别中的占位或短路分支。
 */
class Recognized : public IRecognizer {
 public:
    explicit Recognized() : output_(UNKNOWN) {}
    explicit Recognized(int32_t val) : output_(val) {}
    inline void Set(int32_t value) { output_ = value; }
    virtual int32_t Identify(int32_t /* pipeno */, const uint8_t* packet, int32_t packet_size,
                             const protocol::Layers* layers, RecognizeContext* /* rctx */) {
        return output_;
    }

 private:
    int32_t output_ = UNKNOWN;
};

}  // namespace protocol
}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_IRECOGNIZER_H_
