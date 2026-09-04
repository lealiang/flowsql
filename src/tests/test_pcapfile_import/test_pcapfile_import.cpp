// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <channels/pcapfile/pcap_file_channel.h>
#include <framework/core/packet_codec.h>

#include <arrow/api.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pcapfile = flowsql::channels::pcapfile;

namespace {

class MockProtocol final : public flowsql::IProtocol {
 public:
    void Concurrency(int32_t) override {}
    flowsql::protocol::Protocol Identify(int32_t, const uint8_t*, int32_t, const flowsql::protocol::Layers*) override {
        ++identify_calls;
        return {};
    }
    int32_t Layer(int32_t, const uint8_t*, int32_t size, flowsql::protocol::Layers* layers) override {
        ++layer_calls;
        if (size == 0 || !layers) return -1;
        if (layer_result < 0) return layer_result;
        *layers = {};
        layers->layercount = 1;
        layers->layers[0].layer = flowsql::eLayer::ETHERNET;
        layers->layers[0].offset = 0;
        layers->payload = 0;
        return layer_result;
    }
    flowsql::protocol::IDictionary* Dictionary() override { return nullptr; }
    int layer_calls = 0;
    int identify_calls = 0;
    int layer_result = 1;
};

void Put16(std::vector<uint8_t>* out, uint16_t value, bool little) {
    if (little) { out->push_back(value & 0xff); out->push_back(value >> 8); }
    else { out->push_back(value >> 8); out->push_back(value & 0xff); }
}
void Put32(std::vector<uint8_t>* out, uint32_t value, bool little) {
    if (little) { for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>(value >> (8 * i))); }
    else { for (int i = 3; i >= 0; --i) out->push_back(static_cast<uint8_t>(value >> (8 * i))); }
}
void Put64(std::vector<uint8_t>* out, uint64_t value, bool little) {
    if (little) { for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>(value >> (8 * i))); }
    else { for (int i = 7; i >= 0; --i) out->push_back(static_cast<uint8_t>(value >> (8 * i))); }
}
void WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> MakeClassicPcap(bool little, bool nanosecond, uint32_t fraction,
                                     uint32_t link_type = 1, uint32_t captured = 4,
                                     uint32_t wire = 4) {
    std::vector<uint8_t> bytes;
    if (little && nanosecond) bytes.insert(bytes.end(), {0x4d, 0x3c, 0xb2, 0xa1});
    else if (!little && nanosecond) bytes.insert(bytes.end(), {0xa1, 0xb2, 0x3c, 0x4d});
    else if (little) bytes.insert(bytes.end(), {0xd4, 0xc3, 0xb2, 0xa1});
    else bytes.insert(bytes.end(), {0xa1, 0xb2, 0xc3, 0xd4});
    Put16(&bytes, 2, little); Put16(&bytes, 4, little);
    Put32(&bytes, 0, little); Put32(&bytes, 0, little); Put32(&bytes, 65535, little); Put32(&bytes, link_type, little);
    Put32(&bytes, 2, little); Put32(&bytes, fraction, little); Put32(&bytes, captured, little); Put32(&bytes, wire, little);
    bytes.insert(bytes.end(), {1, 2, 3, 4});
    return bytes;
}

std::vector<uint8_t> MakeNanosecondReplayPcap(const std::vector<uint64_t>& timestamps_ns) {
    std::vector<uint8_t> bytes = {0x4d, 0x3c, 0xb2, 0xa1};
    Put16(&bytes, 2, true);
    Put16(&bytes, 4, true);
    Put32(&bytes, 0, true);
    Put32(&bytes, 0, true);
    Put32(&bytes, 65535, true);
    Put32(&bytes, 1, true);
    for (size_t index = 0; index < timestamps_ns.size(); ++index) {
        Put32(&bytes, static_cast<uint32_t>(timestamps_ns[index] / 1000000000ULL), true);
        Put32(&bytes, static_cast<uint32_t>(timestamps_ns[index] % 1000000000ULL), true);
        Put32(&bytes, 1, true);
        Put32(&bytes, 1, true);
        bytes.push_back(static_cast<uint8_t>(index + 1));
    }
    return bytes;
}

void AppendPcapngBlock(std::vector<uint8_t>* out, uint32_t type,
                       const std::vector<uint8_t>& body, bool little) {
    const uint32_t total = static_cast<uint32_t>(body.size() + 12);
    Put32(out, type, little);
    Put32(out, total, little);
    out->insert(out->end(), body.begin(), body.end());
    Put32(out, total, little);
}

void AppendPcapngSectionHeaderWithOptions(std::vector<uint8_t>* out, bool little,
                                          const std::vector<uint8_t>& options) {
    std::vector<uint8_t> body;
    Put32(&body, 0x1a2b3c4d, little);
    Put16(&body, 1, little);
    Put16(&body, 0, little);
    Put64(&body, 0xffffffffffffffffULL, little);
    body.insert(body.end(), options.begin(), options.end());
    AppendPcapngBlock(out, 0x0a0d0d0a, body, little);
}

void AppendPcapngSectionHeader(std::vector<uint8_t>* out, bool little) {
    AppendPcapngSectionHeaderWithOptions(out, little, {});
}

void AppendPcapngInterface(std::vector<uint8_t>* out, bool little, uint16_t link_type,
                           bool binary_resolution = false, uint8_t resolution = 6,
                           int64_t tsoffset = 0, uint32_t snaplen = 65535) {
    std::vector<uint8_t> body;
    Put16(&body, link_type, little);
    Put16(&body, 0, little);
    Put32(&body, snaplen, little);
    if (binary_resolution || resolution != 6) {
        Put16(&body, 9, little); Put16(&body, 1, little);
        body.push_back(static_cast<uint8_t>(resolution | (binary_resolution ? 0x80 : 0)));
        body.insert(body.end(), 3, 0);
    }
    if (tsoffset != 0) {
        Put16(&body, 14, little); Put16(&body, 8, little);
        Put64(&body, static_cast<uint64_t>(tsoffset), little);
    }
    if (body.size() > 8) {
        Put16(&body, 0, little); Put16(&body, 0, little);
    }
    AppendPcapngBlock(out, 1, body, little);
}

void AppendPcapngEnhancedPacket(std::vector<uint8_t>* out, bool little, uint32_t interface_id,
                                uint64_t timestamp, const std::vector<uint8_t>& packet,
                                uint32_t wire_len) {
    std::vector<uint8_t> body;
    Put32(&body, interface_id, little);
    Put32(&body, static_cast<uint32_t>(timestamp >> 32), little);
    Put32(&body, static_cast<uint32_t>(timestamp), little);
    Put32(&body, static_cast<uint32_t>(packet.size()), little);
    Put32(&body, wire_len, little);
    body.insert(body.end(), packet.begin(), packet.end());
    while ((body.size() & 3u) != 0) body.push_back(0);
    AppendPcapngBlock(out, 6, body, little);
}

void AppendPcapngEnhancedPacket(std::vector<uint8_t>* out, bool little, uint32_t interface_id,
                                uint64_t timestamp, const std::vector<uint8_t>& packet) {
    AppendPcapngEnhancedPacket(out, little, interface_id, timestamp, packet,
                               static_cast<uint32_t>(packet.size()));
}

std::string Temp(const char* suffix) { return std::string("/tmp/flowsql_pcapfile_") + suffix; }

void AssertPcapngSourceError(const char* suffix, const std::vector<uint8_t>& bytes) {
    const std::string path = Temp(suffix);
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel channel("pcapng_error", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kError);
    assert(!event.batch && event.err != 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
}

void TestClassicPcap() {
    std::vector<uint8_t> bytes;
    Put32(&bytes, 0xa1b2c3d4, true); Put16(&bytes, 2, true); Put16(&bytes, 4, true);
    Put32(&bytes, 0, true); Put32(&bytes, 0, true); Put32(&bytes, 65535, true); Put32(&bytes, 1, true);
    Put32(&bytes, 2, true); Put32(&bytes, 123456, true); Put32(&bytes, 4, true); Put32(&bytes, 6, true);
    bytes.insert(bytes.end(), {1, 2, 3, 4});
    const std::string path = Temp("classic.pcap"); WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config; config.path = path; config.format = "pcap"; config.batch_packets = 1;
    pcapfile::PcapFileChannel channel("classic", config, &protocol);
    assert(channel.Open() == 0);
    auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(event.batch->num_rows() == 1);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto captured = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(1));
    auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
    assert(timestamp->Value(0) == 2123456000LL);
    assert(captured->Value(0) == 4);
    assert(raw->GetView(0) == "\x01\x02\x03\x04");
    assert(protocol.layer_calls == 1 && protocol.identify_calls == 0);
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.ReleaseBlock(event.batch) != 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
    assert(channel.PollBlock(0).kind != flowsql::BlockPollEvent::kData);
}

void TestClassicMagicAndEndian() {
    const struct {
        bool little;
        bool nanosecond;
        uint32_t fraction;
        int64_t expected;
    } cases[] = {
        {true, false, 123, 2000123000LL},
        {false, false, 123, 2000123000LL},
        {true, true, 123, 2000000123LL},
        {false, true, 123, 2000000123LL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const std::string path = Temp((std::string("magic_") + std::to_string(i) + ".pcap").c_str());
        WriteFile(path, MakeClassicPcap(cases[i].little, cases[i].nanosecond, cases[i].fraction));
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "auto";
        config.batch_packets = 1;
        pcapfile::PcapFileChannel channel("magic", config, &protocol);
        assert(channel.Open() == 0);
        auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData);
        auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
        assert(timestamp->Value(0) == cases[i].expected);
        assert(channel.ReleaseBlock(event.batch) == 0);
    }
}

void TestPendingErrorAfterData() {
    auto bytes = MakeClassicPcap(true, false, 0);
    Put32(&bytes, 3, true);
    Put32(&bytes, 0, true);
    Put32(&bytes, 4, true);
    Put32(&bytes, 4, true);
    bytes.push_back(9);
    const std::string path = Temp("pending_error.pcap");
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.batch_packets = 8;
    pcapfile::PcapFileChannel channel("pending", config, &protocol);
    assert(channel.Open() == 0);
    auto data = channel.PollBlock(0);
    assert(data.kind == flowsql::BlockPollEvent::kData);
    assert(data.batch->num_rows() == 1);
    assert(channel.ReleaseBlock(data.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kError);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
}

void TestUnsupportedAndMalformedLayer() {
    const std::string unsupported_path = Temp("unsupported_link.pcap");
    WriteFile(unsupported_path, MakeClassicPcap(true, false, 0, 147));
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = unsupported_path;
    pcapfile::PcapFileChannel unsupported("unsupported", config, &protocol);
    assert(unsupported.Open() == 0);
    auto unsupported_event = unsupported.PollBlock(0);
    assert(unsupported_event.kind == flowsql::BlockPollEvent::kData);
    auto unsupported_status = std::static_pointer_cast<arrow::UInt8Array>(unsupported_event.batch->column(7));
    assert(unsupported_status->Value(0) == static_cast<uint8_t>(flowsql::packet::LayerStatus::kUnsupportedLinkType));
    assert(protocol.layer_calls == 0);
    assert(unsupported.ReleaseBlock(unsupported_event.batch) == 0);

    const std::string malformed_path = Temp("malformed_layer.pcap");
    WriteFile(malformed_path, MakeClassicPcap(true, false, 0));
    protocol.layer_result = -1;
    config.path = malformed_path;
    pcapfile::PcapFileChannel malformed("malformed", config, &protocol);
    assert(malformed.Open() == 0);
    auto malformed_event = malformed.PollBlock(0);
    assert(malformed_event.kind == flowsql::BlockPollEvent::kData);
    auto malformed_status = std::static_pointer_cast<arrow::UInt8Array>(malformed_event.batch->column(7));
    assert(malformed_status->Value(0) == static_cast<uint8_t>(flowsql::packet::LayerStatus::kMalformed));
    assert(malformed.ReleaseBlock(malformed_event.batch) == 0);
}

void TestEmptyFileEof() {
    const std::string path = Temp("empty.pcap");
    WriteFile(path, {});
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    pcapfile::PcapFileChannel channel("empty", config, &protocol);
    assert(channel.Open() == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestPcapngSectionAndResolution() {
    std::vector<uint8_t> bytes;
    auto block = [&](uint32_t type, const std::vector<uint8_t>& body) {
        const uint32_t total = static_cast<uint32_t>(body.size() + 12);
        Put32(&bytes, type, true); Put32(&bytes, total, true);
        bytes.insert(bytes.end(), body.begin(), body.end()); Put32(&bytes, total, true);
    };
    std::vector<uint8_t> shb; Put32(&shb, 0x1a2b3c4d, true); Put16(&shb, 1, true); Put16(&shb, 0, true); Put64(&shb, 0xffffffffffffffffULL, true); block(0x0a0d0d0a, shb);
    std::vector<uint8_t> idb; Put16(&idb, 1, true); Put16(&idb, 0, true); Put32(&idb, 65535, true);
    Put16(&idb, 9, true); Put16(&idb, 1, true); idb.push_back(9); idb.push_back(0); idb.push_back(0); idb.push_back(0); Put16(&idb, 14, true); Put16(&idb, 8, true); Put64(&idb, 1, true); Put16(&idb, 0, true); Put16(&idb, 0, true); block(1, idb);
    std::vector<uint8_t> epb; Put32(&epb, 0, true); Put32(&epb, 0, true); Put32(&epb, 1, true); Put32(&epb, 3, true); Put32(&epb, 5, true); epb.insert(epb.end(), {9, 8, 7}); epb.push_back(0); block(6, epb);
    const std::string path = Temp("section.pcapng"); WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config; config.path = path; config.format = "pcapng";
    pcapfile::PcapFileChannel channel("ng", config, &protocol);
    assert(channel.Open() == 0);
    auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    assert(timestamp->Value(0) == 1000000001LL);
    assert(source->Value(0) == 0 && sequence->Value(0) == 0);
    assert(channel.ReleaseBlock(event.batch) == 0);
}

void TestPcapngSectionsAndRounding() {
    std::vector<uint8_t> bytes;
    AppendPcapngSectionHeader(&bytes, true);
    AppendPcapngInterface(&bytes, true, 1, false, 10);
    AppendPcapngEnhancedPacket(&bytes, true, 0, 5, {1});
    AppendPcapngEnhancedPacket(&bytes, true, 0, 15, {2});
    AppendPcapngSectionHeader(&bytes, false);
    AppendPcapngInterface(&bytes, false, 1, true, 3, -1);
    AppendPcapngEnhancedPacket(&bytes, false, 0, 8, {3});
    const std::string path = Temp("sections_rounding.pcapng");
    WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "auto";
    config.batch_packets = 8;
    pcapfile::PcapFileChannel channel("sections", config, &protocol);
    assert(channel.Open() == 0);
    auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData);
    assert(event.batch->num_rows() == 3);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    assert(timestamp->Value(0) == 0);
    assert(timestamp->Value(1) == 2);
    assert(timestamp->Value(2) == 0);
    assert(source->Value(0) == 0 && source->Value(1) == 0 && source->Value(2) == 1);
    assert(sequence->Value(0) == 0 && sequence->Value(1) == 1 && sequence->Value(2) == 0);
    assert(protocol.layer_calls == 3);
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestPcapngEnhancedPacketMapping() {
    std::vector<uint8_t> bytes;
    AppendPcapngSectionHeader(&bytes, true);
    AppendPcapngInterface(&bytes, true, 1, false, 6, 0, 4);
    AppendPcapngEnhancedPacket(&bytes, true, 0, 1234567, {0xde, 0xad, 0xbe}, 9);
    const std::string path = Temp("epb_mapping.pcapng");
    WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel channel("epb_mapping", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(event.batch->num_rows() == 1);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto captured = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(1));
    auto wire = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(2));
    auto link_type = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(3));
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
    assert(timestamp->Value(0) == 1234567000LL);
    assert(captured->Value(0) == 3 && wire->Value(0) == 9);
    assert(link_type->Value(0) == 1 && source->Value(0) == 0 && sequence->Value(0) == 0);
    assert(raw->GetView(0) == std::string_view("\xde\xad\xbe", 3));
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestPcapngEnhancedPacketErrors() {
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        AppendPcapngEnhancedPacket(&bytes, true, 1, 0, {1});
        AssertPcapngSourceError("epb_unknown_interface.pcapng", bytes);
    }
    for (uint32_t type : {2u, 3u, 0x12345678u}) {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        AppendPcapngBlock(&bytes, type, std::vector<uint8_t>(20, 0), true);
        const std::string suffix = "epb_unsupported_" + std::to_string(type) + ".pcapng";
        AssertPcapngSourceError(suffix.c_str(), bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1, false, 6, 0, 2);
        AppendPcapngEnhancedPacket(&bytes, true, 0, 0, {1, 2, 3});
        AssertPcapngSourceError("epb_exceeds_snaplen.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        std::vector<uint8_t> body;
        Put32(&body, 0, true);
        Put32(&body, 0, true);
        Put32(&body, 0, true);
        Put32(&body, 5, true);
        Put32(&body, 5, true);
        body.insert(body.end(), {1, 2, 3, 4});
        AppendPcapngBlock(&bytes, 6, body, true);
        AssertPcapngSourceError("epb_truncated_bytes.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        std::vector<uint8_t> body;
        Put32(&body, 0, true);
        Put32(&body, 0, true);
        Put32(&body, 0, true);
        Put32(&body, 4, true);
        Put32(&body, 4, true);
        body.insert(body.end(), {1, 2, 3, 4});
        Put16(&body, 1, true);
        Put16(&body, 4, true);
        AppendPcapngBlock(&bytes, 6, body, true);
        AssertPcapngSourceError("epb_truncated_option.pcapng", bytes);
    }
}

void TestPcapngSectionValidation() {
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AssertPcapngSourceError("section_without_idb.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        const std::string path = Temp("empty_section.pcapng");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        pcapfile::PcapFileChannel channel("empty_section", config, &protocol);
        assert(channel.Open() == 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngSectionHeader(&bytes, false);
        AppendPcapngInterface(&bytes, false, 1);
        AssertPcapngSourceError("section_before_idb.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        AppendPcapngEnhancedPacket(&bytes, true, 0, 0, {1});
        AppendPcapngSectionHeader(&bytes, false);
        const std::string path = Temp("last_section_without_idb.pcapng");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        pcapfile::PcapFileChannel channel("last_section_without_idb", config, &protocol);
        assert(channel.Open() == 0);
        const auto data = channel.PollBlock(0);
        assert(data.kind == flowsql::BlockPollEvent::kData && data.batch);
        assert(data.batch->num_rows() == 1);
        assert(channel.ReleaseBlock(data.batch) == 0);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
    {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> options;
        Put16(&options, 1, true);
        Put16(&options, 4, true);
        AppendPcapngSectionHeaderWithOptions(&bytes, true, options);
        const std::string path = Temp("first_shb_truncated_option.pcapng");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        pcapfile::PcapFileChannel channel("first_shb_truncated_option", config, &protocol);
        assert(channel.Open() != 0);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        std::vector<uint8_t> options;
        Put16(&options, 0, false);
        Put16(&options, 1, false);
        AppendPcapngSectionHeaderWithOptions(&bytes, false, options);
        AssertPcapngSourceError("later_shb_invalid_terminator.pcapng", bytes);
    }
}

void TestManyPcapngSectionsIterative() {
    constexpr uint32_t kSectionCount = 32768;
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(kSectionCount) * 48);
    for (uint32_t index = 0; index < kSectionCount; ++index) {
        const bool little = (index & 1u) == 0;
        AppendPcapngSectionHeader(&bytes, little);
        AppendPcapngInterface(&bytes, little, 1);
        if (index + 1 == kSectionCount) {
            AppendPcapngEnhancedPacket(&bytes, little, 0, 0, {1});
        }
    }
    const std::string path = Temp("many_sections.pcapng");
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel channel("many_sections", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(event.batch->num_rows() == 1);
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    assert(source->Value(0) == kSectionCount - 1);
    assert(sequence->Value(0) == 0);
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestReplayModes() {
    const std::vector<uint64_t> timestamps_ns = {
        1000000000ULL,
        1006001001ULL,
        1006001002ULL,
        1003001002ULL,
        1012002003ULL,
    };
    const std::string path = Temp("replay.pcap");
    WriteFile(path, MakeNanosecondReplayPcap(timestamps_ns));

    {
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        config.batch_packets = 8;
        config.replay_mode = pcapfile::PcapReplayMode::kFast;
        std::vector<int64_t> waits_ns;
        pcapfile::PcapFileChannel channel(
            "fast", config, &protocol,
            [&](std::chrono::nanoseconds delay) { waits_ns.push_back(delay.count()); });
        assert(channel.Open() == 0);
        const auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        assert(event.batch->num_rows() == static_cast<int64_t>(timestamps_ns.size()));
        assert(waits_ns.empty());
        assert(channel.ReleaseBlock(event.batch) == 0);
    }

    {
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        config.batch_packets = 8;
        config.replay_mode = pcapfile::PcapReplayMode::kTimestamp;
        config.replay_speed_milli = 1000;
        std::vector<int64_t> waits_ns;
        pcapfile::PcapFileChannel channel(
            "timestamp_original_speed", config, &protocol,
            [&](std::chrono::nanoseconds delay) { waits_ns.push_back(delay.count()); });
        assert(channel.Open() == 0);
        auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && waits_ns.empty());
        assert(channel.ReleaseBlock(event.batch) == 0);
        event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData);
        assert(waits_ns == std::vector<int64_t>{6001001});
        assert(channel.ReleaseBlock(event.batch) == 0);
    }

    {
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        config.batch_packets = 8;
        config.replay_mode = pcapfile::PcapReplayMode::kTimestamp;
        config.replay_speed_milli = 3000;
        std::vector<int64_t> waits_ns;
        pcapfile::PcapFileChannel channel(
            "timestamp", config, &protocol,
            [&](std::chrono::nanoseconds delay) { waits_ns.push_back(delay.count()); });
        assert(channel.Open() == 0);
        for (size_t index = 0; index < timestamps_ns.size(); ++index) {
            const auto event = channel.PollBlock(0);
            assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
            assert(event.batch->num_rows() == 1);
            auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
            auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
            auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
            assert(timestamp->Value(0) == static_cast<int64_t>(timestamps_ns[index]));
            assert(sequence->Value(0) == index);
            assert(raw->GetView(0).size() == 1);
            assert(static_cast<uint8_t>(raw->GetView(0)[0]) == index + 1);
            assert(channel.ReleaseBlock(event.batch) == 0);
            if (index == 0) assert(waits_ns.empty());
            if (index == 1) assert(waits_ns == std::vector<int64_t>{2000333});
            if (index == 2) assert(waits_ns == std::vector<int64_t>({2000333, 1}));
            if (index == 3) assert(waits_ns == std::vector<int64_t>({2000333, 1}));
        }
        assert(waits_ns == std::vector<int64_t>({2000333, 1, 3000333}));
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
    }
}

void TestBackpressureAndCancelWakeup() {
    const std::string path = Temp("backpressure.pcap");
    WriteFile(path, MakeNanosecondReplayPcap({1000000000ULL, 1000000001ULL}));

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcap";
    config.batch_packets = 1;
    pcapfile::PcapFileChannel channel("backpressure", config, &protocol);
    assert(channel.Open() == 0);
    const auto first = channel.PollBlock(0);
    assert(first.kind == flowsql::BlockPollEvent::kData && first.batch);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kTimeout);

    auto next_poll = std::async(std::launch::async, [&]() { return channel.PollBlock(5000); });
    assert(next_poll.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(channel.ReleaseBlock(first.batch) == 0);
    assert(next_poll.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto second = next_poll.get();
    assert(second.kind == flowsql::BlockPollEvent::kData && second.batch);
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(second.batch->column(5));
    assert(sequence->Value(0) == 1);
    assert(channel.ReleaseBlock(second.batch) == 0);

    assert(channel.Close() == 0);
    assert(channel.Open() == 0);
    const auto outstanding = channel.PollBlock(0);
    assert(outstanding.kind == flowsql::BlockPollEvent::kData && outstanding.batch);
    auto cancelled_poll = std::async(std::launch::async, [&]() { return channel.PollBlock(5000); });
    assert(cancelled_poll.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    channel.Cancel();
    assert(cancelled_poll.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(cancelled_poll.get().kind == flowsql::BlockPollEvent::kCancelled);
    assert(channel.ReleaseBlock(outstanding.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
}

void TestCancelInterruptsReplayWait() {
    const std::string path = Temp("cancel_replay_wait.pcap");
    WriteFile(path, MakeNanosecondReplayPcap({1000000000ULL, 11000000000ULL}));

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcap";
    config.replay_mode = pcapfile::PcapReplayMode::kTimestamp;
    pcapfile::PcapFileChannel channel("cancel_replay", config, &protocol);
    assert(channel.Open() == 0);
    const auto first = channel.PollBlock(0);
    assert(first.kind == flowsql::BlockPollEvent::kData && first.batch);
    assert(channel.ReleaseBlock(first.batch) == 0);

    auto replay_poll = std::async(std::launch::async, [&]() { return channel.PollBlock(0); });
    assert(replay_poll.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(channel.Close() == EBUSY);
    channel.Cancel();
    assert(replay_poll.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(replay_poll.get().kind == flowsql::BlockPollEvent::kCancelled);
    assert(channel.Close() == 0);
}

void TestBatchOwnerAndPluginLifecycle() {
    const std::string path = Temp("owner_lifecycle.pcap");
    WriteFile(path, MakeClassicPcap(true, false, 0));

    std::shared_ptr<arrow::RecordBatch> retained;
    {
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        auto channel = std::make_unique<pcapfile::PcapFileChannel>("owner", config, &protocol);
        assert(channel->Open() == 0);
        const auto event = channel->PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        retained = event.batch;
        assert(channel->Close() == EBUSY);
    }
    auto retained_raw = std::static_pointer_cast<arrow::BinaryArray>(retained->column(6));
    assert(retained_raw->GetView(0) == "\x01\x02\x03\x04");

    MockProtocol protocol;
    pcapfile::PcapFilePlugin plugin;
    class Querier final : public flowsql::IQuerier {
     public:
        explicit Querier(flowsql::IProtocol* protocol) : protocol_(protocol) {}
        int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
            if (iid < flowsql::IID_PROTOCOL || flowsql::IID_PROTOCOL < iid) return 0;
            return callback ? callback(protocol_) : 0;
        }
        void* First(const flowsql::Guid& iid) override {
            if (!(iid < flowsql::IID_PROTOCOL) && !(flowsql::IID_PROTOCOL < iid)) return protocol_;
            return nullptr;
        }

     private:
        flowsql::IProtocol* protocol_;
    } querier(&protocol);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.AddChannel("pcapfile", "owner", "{\"path\":\"" + path + "\"}") == 0);
    auto* channel = plugin.Get("pcapfile", "owner");
    assert(channel != nullptr);
    const auto event = channel->PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(plugin.Stop() == EBUSY);
    assert(plugin.Unload() == EBUSY);
    assert(plugin.Get("pcapfile", "owner") == channel);
    assert(channel->ReleaseBlock(event.batch) == 0);
    assert(plugin.Stop() == 0);
    assert(plugin.Unload() == 0);
    auto released_raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
    assert(released_raw->GetView(0) == "\x01\x02\x03\x04");
}

void TestMalformedPcapngAndFormatMismatch() {
    std::vector<uint8_t> bytes;
    AppendPcapngSectionHeader(&bytes, true);
    AppendPcapngInterface(&bytes, true, 1);
    AppendPcapngEnhancedPacket(&bytes, true, 0, 0, {1, 2, 3});
    bytes[bytes.size() - 1] = 0;
    bytes.back() = 0xff;
    const std::string path = Temp("bad_pcapng.pcapng");
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel channel("bad", config, &protocol);
    assert(channel.Open() == 0);
    auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kError);
    assert(channel.PollBlock(0).kind != flowsql::BlockPollEvent::kEof);

    const std::string pcap_path = Temp("format_mismatch.pcap");
    WriteFile(pcap_path, MakeClassicPcap(true, false, 0));
    config.path = pcap_path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel mismatch("mismatch", config, &protocol);
    assert(mismatch.Open() != 0);
    assert(mismatch.PollBlock(0).kind == flowsql::BlockPollEvent::kError);
    assert(mismatch.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
}

void TestIncrementalReadAfterOpen() {
    {
        const std::string path = Temp("classic_truncated_after_open.pcap");
        auto bytes = MakeClassicPcap(true, false, 0);
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        pcapfile::PcapFileChannel channel("classic_truncated_after_open", config, &protocol);
        assert(channel.Open() == 0);
        bytes.resize(24);
        WriteFile(path, bytes);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
    {
        const std::string path = Temp("pcapng_truncated_after_open.pcapng");
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        const size_t section_header_size = bytes.size();
        AppendPcapngInterface(&bytes, true, 1);
        AppendPcapngEnhancedPacket(&bytes, true, 0, 0, {1, 2, 3, 4});
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        pcapfile::PcapFileChannel channel("pcapng_truncated_after_open", config, &protocol);
        assert(channel.Open() == 0);
        bytes.resize(section_header_size);
        WriteFile(path, bytes);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        Put32(&bytes, 6, true);
        Put32(&bytes, 0xfffffffcU, true);
        Put32(&bytes, 0, true);
        AssertPcapngSourceError("oversized_declared_block.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes = {0xd4, 0xc3, 0xb2, 0xa1};
        Put16(&bytes, 2, true);
        Put16(&bytes, 4, true);
        Put32(&bytes, 0, true);
        Put32(&bytes, 0, true);
        Put32(&bytes, 0xffffffffU, true);
        Put32(&bytes, 1, true);
        Put32(&bytes, 0, true);
        Put32(&bytes, 0, true);
        Put32(&bytes, 0xffffffffU, true);
        Put32(&bytes, 0xffffffffU, true);
        const std::string path = Temp("oversized_declared_packet.pcap");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        pcapfile::PcapFileChannel channel("oversized_declared_packet", config, &protocol);
        assert(channel.Open() == 0);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
}

void TestCancelAndManagerBusy() {
    const std::string path = Temp("manager_busy.pcap");
    auto bytes = MakeClassicPcap(true, false, 0);
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFilePlugin plugin;
    class Querier final : public flowsql::IQuerier {
     public:
        explicit Querier(flowsql::IProtocol* protocol) : protocol_(protocol) {}
        int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
            if (iid < flowsql::IID_PROTOCOL || flowsql::IID_PROTOCOL < iid) return 0;
            return callback ? callback(protocol_) : 0;
        }
        void* First(const flowsql::Guid& iid) override {
            if (!(iid < flowsql::IID_PROTOCOL) && !(flowsql::IID_PROTOCOL < iid)) return protocol_;
            return nullptr;
        }
     private:
        flowsql::IProtocol* protocol_;
    } querier(&protocol);
    assert(plugin.Load(&querier) == 0);
    const std::string option = "{\"path\":\"" + path + "\",\"batch_packets\":1}";
    assert(plugin.AddChannel("pcapfile", "busy", option) == 0);
    auto* channel = dynamic_cast<pcapfile::PcapFileChannel*>(plugin.Get("pcapfile", "busy"));
    assert(channel != nullptr);
    auto event = channel->PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData);
    assert(plugin.ModifyChannel("pcapfile", "busy", option) == EBUSY);
    assert(plugin.RemoveChannel("pcapfile", "busy") == EBUSY);
    assert(channel->ReleaseBlock(event.batch) == 0);
    channel->Cancel();
    assert(channel->PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    assert(plugin.RemoveChannel("pcapfile", "busy") == 0);
    plugin.Unload();
}

void TestOptions() {
    pcapfile::PcapFileSourceConfig config;
    std::string normalized;
    std::string error;
    assert(pcapfile::ParsePcapFileSourceConfig("{\"path\":\"/tmp/none\",\"batch_packets\":0}", &config, &normalized, &error) != 0);
    assert(pcapfile::ParsePcapFileSourceConfig("{\"path\":\"/tmp/none\",\"unknown\":1}", &config, &normalized, &error) != 0);

    const std::string path = Temp("options_valid.pcap");
    WriteFile(path, MakeClassicPcap(true, false, 0));
    const std::string option = "{\"path\":\"" + path +
                               "\",\"format\":\"PCAP\",\"batch_packets\":4,"
                               "\"replay_mode\":\"TIMESTAMP\",\"replay_speed_milli\":2000}";
    assert(pcapfile::ParsePcapFileSourceConfig(option, &config, &normalized, &error) == 0);
    assert(config.path == path);
    assert(config.format == "pcap");
    assert(config.batch_packets == 4);
    assert(config.replay_mode == pcapfile::PcapReplayMode::kTimestamp);
    assert(config.replay_speed_milli == 2000);
    assert(normalized == "{\"path\":\"" + path +
                             "\",\"format\":\"pcap\",\"batch_packets\":4,"
                             "\"replay_mode\":\"timestamp\",\"replay_speed_milli\":2000}");
}

void TestPluginManager() {
    MockProtocol protocol;
    pcapfile::PcapFilePlugin plugin;
    class Querier final : public flowsql::IQuerier {
     public:
        explicit Querier(flowsql::IProtocol* protocol) : protocol_(protocol) {}
        int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
            if (iid < flowsql::IID_PROTOCOL || flowsql::IID_PROTOCOL < iid) return 0;
            return callback ? callback(protocol_) : 0;
        }
        void* First(const flowsql::Guid& iid) override {
            if (!(iid < flowsql::IID_PROTOCOL) && !(flowsql::IID_PROTOCOL < iid)) return protocol_;
            return nullptr;
        }
     private:
        flowsql::IProtocol* protocol_;
    } querier(&protocol);
    assert(plugin.Load(&querier) == 0);
    const std::string path = Temp("manager.pcap");
    WriteFile(path, {});
    assert(plugin.AddChannel("pcapfile", "one", "{\"path\":\"" + path + "\"}") == 0);
    auto* channel = plugin.Get("PCAPFILE", "one");
    assert(channel != nullptr);
    assert(std::string(channel->Category()) == "pcapfile");
    assert(std::string(channel->Name()) == "one");
    assert(std::string(channel->Type()) == flowsql::ChannelType::kBlockStream);
    assert(std::string(channel->Schema()) == "packet");
    assert(channel->Flush() == 0);
    int listed = 0;
    plugin.List([&](const char* type, const char* name, flowsql::IBlockStreamChannel* listed_channel) {
        assert(std::string(type) == "pcapfile");
        assert(std::string(name) == "one");
        assert(listed_channel == channel);
        ++listed;
    });
    assert(listed == 1);
    assert(plugin.AddChannel("other", "x", "{}") == ENOTSUP);
    assert(plugin.ModifyChannel("pcapfile", "missing", "{}") == ENOENT);
    assert(plugin.ModifyChannel("pcapfile", "one", "{\"path\":\"" + path + "\"}") == 0);
    assert(plugin.RemoveChannel("pcapfile", "one") == 0);
    assert(plugin.RemoveChannel("pcapfile", "one") == ENOENT);
    plugin.Unload();
}

}  // namespace

int main() {
    TestClassicPcap();
    TestClassicMagicAndEndian();
    TestPendingErrorAfterData();
    TestUnsupportedAndMalformedLayer();
    TestEmptyFileEof();
    TestPcapngSectionAndResolution();
    TestPcapngSectionsAndRounding();
    TestPcapngEnhancedPacketMapping();
    TestPcapngEnhancedPacketErrors();
    TestPcapngSectionValidation();
    TestManyPcapngSectionsIterative();
    TestReplayModes();
    TestBackpressureAndCancelWakeup();
    TestCancelInterruptsReplayWait();
    TestBatchOwnerAndPluginLifecycle();
    TestMalformedPcapngAndFormatMismatch();
    TestIncrementalReadAfterOpen();
    TestOptions();
    TestPluginManager();
    TestCancelAndManagerBusy();
    std::puts("[PASS] pcapfile import");
    return 0;
}
