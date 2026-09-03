// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_PACKET_DECODER_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_PACKET_DECODER_H_

#include <framework/interfaces/ipacket.h>

#include "iprotocol.h"

namespace flowsql::protocol {

class NpiPacketLayerDecoder final : public packet::IPacketLayerDecoder {
 public:
    explicit NpiPacketLayerDecoder(IProtocol* protocol, int32_t pipeno = 0);

    packet::PacketLayerInfo Decode(const packet::PacketView& packet,
                                   const packet::LayerDecodeOptions& options) override;

 private:
    IProtocol* protocol_ = nullptr;
    int32_t pipeno_ = 0;
};

class NpiPacketProtocolIdentifier final : public packet::IPacketProtocolIdentifier {
 public:
    explicit NpiPacketProtocolIdentifier(IProtocol* protocol, int32_t pipeno = 0);

    packet::PacketProtocolInfo Identify(const packet::PacketView& packet,
                                        const packet::PacketLayerInfo& layer) override;

 private:
    IProtocol* protocol_ = nullptr;
    int32_t pipeno_ = 0;
};

}  // namespace flowsql::protocol

#endif  // _FLOWSQL_PLUGINS_PROTOCOL_NPI_PACKET_DECODER_H_
