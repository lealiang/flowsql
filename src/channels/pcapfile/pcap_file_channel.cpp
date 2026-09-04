// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "pcap_file_channel.h"

#include <framework/core/packet_codec.h>
#include <plugins/npi/packet_decoder.h>

#include <arrow/api.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <tuple>

#include <boost/multiprecision/cpp_int.hpp>

namespace flowsql::channels::pcapfile {
namespace {

using boost::multiprecision::cpp_int;

struct CaptureRecord {
    int64_t timestamp_ns = 0;
    uint32_t captured_len = 0;
    uint32_t wire_len = 0;
    uint32_t link_type = 0;
    uint32_t source_id = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> bytes;
};

uint16_t Read16(const uint8_t* p, bool little) {
    return little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                  : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Read32(const uint8_t* p, bool little) {
    if (little) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t Read64(const uint8_t* p, bool little) {
    uint64_t value = 0;
    if (little) {
        for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
    } else {
        for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    }
    return value;
}

int64_t ReadSigned64(const uint8_t* p, bool little) {
    return static_cast<int64_t>(Read64(p, little));
}

bool Has(const std::vector<uint8_t>& data, size_t pos, size_t count) {
    return pos <= data.size() && count <= data.size() - pos;
}

bool IsMagic(const uint8_t* p, std::initializer_list<uint8_t> bytes) {
    size_t i = 0;
    for (uint8_t value : bytes) {
        if (p[i++] != value) return false;
    }
    return true;
}

int SetError(std::string* error, const char* message, int code = EINVAL) {
    if (error) *error = message;
    return code;
}

int ValidatePcapngOptions(const std::vector<uint8_t>& data,
                          size_t begin,
                          size_t end,
                          bool little,
                          std::string* error) {
    if (begin > end || end > data.size()) return SetError(error, "pcapng option range invalid");
    if (begin == end) return 0;

    size_t position = begin;
    while (position < end) {
        if (end - position < 4) return SetError(error, "pcapng option header truncated");
        const uint16_t code = Read16(data.data() + position, little);
        const uint16_t length = Read16(data.data() + position + 2, little);
        position += 4;
        if (code == 0) {
            if (length != 0) return SetError(error, "pcapng option terminator invalid");
            if (position != end) return SetError(error, "pcapng option data follows terminator");
            return 0;
        }
        if (static_cast<size_t>(length) > end - position) {
            return SetError(error, "pcapng option data truncated");
        }
        const size_t padded = (static_cast<size_t>(length) + 3u) & ~static_cast<size_t>(3u);
        if (padded < length || padded > end - position) {
            return SetError(error, "pcapng option padding truncated");
        }
        position += padded;
    }
    return SetError(error, "pcapng option terminator missing");
}

int64_t TimestampNs(uint64_t ticks, uint8_t exponent, bool binary, int64_t offset_seconds, bool* ok) {
    cpp_int denominator = 1;
    if (binary) {
        denominator <<= exponent;
    } else {
        for (uint8_t i = 0; i < exponent; ++i) denominator *= 10;
    }
    if (!ok) return 0;
    *ok = false;
    const cpp_int numerator = cpp_int(ticks) * 1000000000;
    cpp_int quotient = numerator / denominator;
    const cpp_int remainder = numerator % denominator;
    const cpp_int twice = remainder * 2;
    if (twice > denominator || (twice == denominator && (quotient & 1) != 0)) ++quotient;
    quotient += cpp_int(offset_seconds) * 1000000000;
    const cpp_int min_value = std::numeric_limits<int64_t>::min();
    const cpp_int max_value = std::numeric_limits<int64_t>::max();
    if (quotient < min_value || quotient > max_value) return 0;
    *ok = true;
    return quotient.convert_to<int64_t>();
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsRegularFileNoSymlink(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p(path);
    return !std::filesystem::is_symlink(p, ec) && std::filesystem::is_regular_file(p, ec);
}

class PcapLayerAdapter final {
 public:
    explicit PcapLayerAdapter(IProtocol* protocol) : decoder_(protocol) {}

    packet::PacketLayerInfo Decode(const packet::PacketView& view) {
        return decoder_.Decode(view, packet::LayerDecodeOptions{
            packet::kExtractMac | packet::kExtractIp | packet::kExtractPort,
            packet::EndpointScope::kInnermost});
    }

 private:
    protocol::NpiPacketLayerDecoder decoder_;
};

}  // namespace

class PcapFileReader {
 public:
    int Open(const std::string& path, const std::string& requested_format, std::string* error) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return SetError(error, "open capture file failed", errno ? errno : EIO);
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        if (length < 0) return SetError(error, "stat capture file failed", EIO);
        input.seekg(0, std::ios::beg);
        data_.resize(static_cast<size_t>(length));
        if (!data_.empty() && !input.read(reinterpret_cast<char*>(data_.data()), length)) {
            return SetError(error, "read capture file failed", EIO);
        }
        if (data_.empty()) {
            kind_ = Kind::kPcap;
            pos_ = 0;
            return 0;
        }
        if (requested_format != "auto" && requested_format != "pcap" && requested_format != "pcapng") {
            return SetError(error, "invalid capture format");
        }
        const bool is_pcapng = data_.size() >= 4 && IsMagic(data_.data(), {0x0a, 0x0d, 0x0d, 0x0a});
        if (requested_format == "pcapng" && !is_pcapng) return SetError(error, "format is not pcapng");
        if (requested_format == "pcap" && is_pcapng) return SetError(error, "format is not classic pcap");
        if (requested_format == "pcapng" || (requested_format == "auto" && is_pcapng)) {
            kind_ = Kind::kPcapng;
            pos_ = 0;
            little_ = true;
            have_section_ = false;
            interfaces_.clear();
            source_id_next_ = 0;
            return ValidateFirstSection(error);
        }
        kind_ = Kind::kPcap;
        return OpenClassic(error);
    }

    int Next(CaptureRecord* output, std::string* error) {
        if (!output) return EINVAL;
        return kind_ == Kind::kPcap ? NextClassic(output, error) : NextPcapng(output, error);
    }

 private:
    enum class Kind { kPcap, kPcapng };
    struct InterfaceInfo {
        uint32_t link_type = 0;
        uint32_t snaplen = 0;
        uint8_t resolution = 6;
        bool binary_resolution = false;
        int64_t tsoffset = 0;
        uint32_t source_id = 0;
        uint64_t sequence = 0;
    };

    int OpenClassic(std::string* error) {
        if (data_.size() < 24) return SetError(error, "classic pcap global header is truncated");
        const uint8_t* p = data_.data();
        if (IsMagic(p, {0xd4, 0xc3, 0xb2, 0xa1})) {
            little_ = true;
            nanosecond_ = false;
        } else if (IsMagic(p, {0xa1, 0xb2, 0xc3, 0xd4})) {
            little_ = false;
            nanosecond_ = false;
        } else if (IsMagic(p, {0x4d, 0x3c, 0xb2, 0xa1})) {
            little_ = true;
            nanosecond_ = true;
        } else if (IsMagic(p, {0xa1, 0xb2, 0x3c, 0x4d})) {
            little_ = false;
            nanosecond_ = true;
        } else {
            return SetError(error, "unsupported capture magic");
        }
        if (Read16(p + 4, little_) != 2 || Read16(p + 6, little_) != 4) {
            return SetError(error, "unsupported pcap version");
        }
        network_ = Read32(p + 20, little_);
        snaplen_ = Read32(p + 16, little_);
        if (snaplen_ == 0) return SetError(error, "pcap snaplen is invalid");
        pos_ = 24;
        sequence_ = 0;
        return 0;
    }

    int ValidateFirstSection(std::string* error) {
        if (data_.size() < 28) return SetError(error, "pcapng section header is truncated");
        if (!IsMagic(data_.data(), {0x0a, 0x0d, 0x0d, 0x0a})) return SetError(error, "pcapng section header missing");
        const uint8_t* p = data_.data();
        if (IsMagic(p + 8, {0x4d, 0x3c, 0x2b, 0x1a})) little_ = true;
        else if (IsMagic(p + 8, {0x1a, 0x2b, 0x3c, 0x4d})) little_ = false;
        else return SetError(error, "pcapng byte-order magic invalid");
        const uint32_t total = Read32(p + 4, little_);
        if (total < 28 || (total & 3) != 0 || total > data_.size()) return SetError(error, "pcapng section length invalid");
        if (Read32(p + total - 4, little_) != total) return SetError(error, "pcapng section length trailer invalid");
        if (Read16(p + 12, little_) != 1 || Read16(p + 14, little_) != 0) return SetError(error, "pcapng version unsupported");
        if (ValidatePcapngOptions(data_, 24, total - 4, little_, error) != 0) return EINVAL;
        pos_ = total;
        have_section_ = true;
        interfaces_.clear();
        return 0;
    }

    int NextClassic(CaptureRecord* output, std::string* error) {
        if (pos_ == data_.size()) return 0;
        if (!Has(data_, pos_, 16)) return SetError(error, "pcap record header is truncated");
        const uint8_t* p = data_.data() + pos_;
        const uint32_t sec = Read32(p, little_);
        const uint32_t fraction = Read32(p + 4, little_);
        const uint32_t captured = Read32(p + 8, little_);
        const uint32_t wire = Read32(p + 12, little_);
        const uint32_t limit = nanosecond_ ? 1000000000U : 1000000U;
        if (fraction >= limit || (wire != 0 && wire < captured) || captured > snaplen_) {
            return SetError(error, "pcap record timestamp or length invalid");
        }
        if (!Has(data_, pos_ + 16, captured)) return SetError(error, "pcap packet bytes are truncated", EIO);
        const cpp_int timestamp = cpp_int(sec) * 1000000000 + cpp_int(fraction) * (nanosecond_ ? 1 : 1000);
        if (timestamp > std::numeric_limits<int64_t>::max()) return SetError(error, "pcap timestamp overflow");
        output->timestamp_ns = timestamp.convert_to<int64_t>();
        output->captured_len = captured;
        output->wire_len = wire;
        output->link_type = network_;
        output->source_id = 0;
        output->sequence = sequence_++;
        output->bytes.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_ + 16),
                             data_.begin() + static_cast<std::ptrdiff_t>(pos_ + 16 + captured));
        pos_ += 16 + captured;
        return 1;
    }

    int BeginSection(std::string* error) {
        if (!Has(data_, pos_, 28)) return SetError(error, "pcapng section header is truncated");
        const uint8_t* p = data_.data() + pos_;
        if (IsMagic(p + 8, {0x4d, 0x3c, 0x2b, 0x1a})) little_ = true;
        else if (IsMagic(p + 8, {0x1a, 0x2b, 0x3c, 0x4d})) little_ = false;
        else return SetError(error, "pcapng byte-order magic invalid");
        const uint32_t total = Read32(p + 4, little_);
        if (total < 28 || (total & 3) != 0 || !Has(data_, pos_, total) || Read32(p + total - 4, little_) != total) {
            return SetError(error, "pcapng section length invalid");
        }
        if (Read16(p + 12, little_) != 1 || Read16(p + 14, little_) != 0) return SetError(error, "pcapng version unsupported");
        if (ValidatePcapngOptions(data_, pos_ + 24, pos_ + total - 4, little_, error) != 0) return EINVAL;
        pos_ += total;
        interfaces_.clear();
        have_section_ = true;
        return 0;
    }

    int NextPcapng(CaptureRecord* output, std::string* error) {
        while (pos_ < data_.size()) {
            if (!Has(data_, pos_, 12)) return SetError(error, "pcapng block header is truncated");
            const uint8_t* p = data_.data() + pos_;
            if (IsMagic(p, {0x0a, 0x0d, 0x0d, 0x0a})) {
                if (interfaces_.empty()) return SetError(error, "pcapng section has no interface");
                const int rc = BeginSection(error);
                if (rc != 0) return rc;
                continue;
            }
            if (!have_section_) return SetError(error, "pcapng block precedes section header");
            const uint32_t type = Read32(p, little_);
            const uint32_t total = Read32(p + 4, little_);
            if (total < 12 || (total & 3) != 0 || !Has(data_, pos_, total) || Read32(p + total - 4, little_) != total) {
                return SetError(error, "pcapng block length invalid");
            }
            const size_t block_pos = pos_;
            pos_ += total;
            if (type == 1) {
                if (total < 20) return SetError(error, "pcapng interface block is truncated");
                InterfaceInfo info;
                info.link_type = Read16(p + 8, little_);
                info.snaplen = Read32(p + 12, little_);
                info.resolution = 6;
                info.source_id = source_id_next_++;
                const size_t options_begin = block_pos + 16;
                const size_t options_end = block_pos + total - 4;
                size_t option_pos = options_begin;
                bool option_terminated = options_begin == options_end;
                while (option_pos + 4 <= options_end) {
                    const uint16_t code = Read16(data_.data() + option_pos, little_);
                    const uint16_t length = Read16(data_.data() + option_pos + 2, little_);
                    option_pos += 4;
                    if (code == 0) {
                        if (length != 0) return SetError(error, "pcapng interface option terminator invalid");
                        option_terminated = true;
                        break;
                    }
                    if (static_cast<size_t>(length) > options_end - option_pos) {
                        return SetError(error, "pcapng interface option truncated");
                    }
                    if (code == 9 && length == 1) {
                        const uint8_t value = data_[option_pos];
                        info.binary_resolution = (value & 0x80) != 0;
                        info.resolution = value & 0x7f;
                    } else if (code == 9) {
                        return SetError(error, "pcapng if_tsresol option length invalid");
                    } else if (code == 14 && length == 8) {
                        info.tsoffset = ReadSigned64(data_.data() + option_pos, little_);
                    } else if (code == 14) {
                        return SetError(error, "pcapng if_tsoffset option length invalid");
                    }
                    const size_t padded = (static_cast<size_t>(length) + 3u) & ~static_cast<size_t>(3u);
                    if (padded < length || padded > options_end - option_pos) {
                        return SetError(error, "pcapng interface option padding truncated");
                    }
                    option_pos += padded;
                }
                if (!option_terminated || option_pos != options_end) {
                    return SetError(error, "pcapng interface options are truncated");
                }
                interfaces_.push_back(info);
                continue;
            }
            if (type == 2 || type == 3) return SetError(error, "unsupported pcapng packet block");
            if (type != 6) return SetError(error, "unsupported pcapng block");
            if (total < 32) return SetError(error, "pcapng enhanced packet block is truncated");
            const uint32_t interface_id = Read32(p + 8, little_);
            if (interface_id >= interfaces_.size()) {
                return SetError(error, "pcapng packet references unknown interface");
            }
            auto& info = interfaces_[interface_id];
            const uint64_t timestamp = (static_cast<uint64_t>(Read32(p + 12, little_)) << 32) |
                                       Read32(p + 16, little_);
            const uint32_t captured = Read32(p + 20, little_);
            const uint32_t wire = Read32(p + 24, little_);
            if ((info.snaplen != 0 && captured > info.snaplen) || (wire != 0 && wire < captured)) {
                return SetError(error, "pcapng packet length invalid");
            }
            const size_t bytes_pos = block_pos + 28;
            const size_t padded_captured = (static_cast<size_t>(captured) + 3u) & ~static_cast<size_t>(3u);
            if (!Has(data_, bytes_pos, static_cast<size_t>(captured)) ||
                padded_captured > block_pos + total - 4 - bytes_pos) {
                return SetError(error, "pcapng packet bytes are truncated", EIO);
            }
            const size_t options_begin = bytes_pos + padded_captured;
            const size_t options_end = block_pos + total - 4;
            if (ValidatePcapngOptions(data_, options_begin, options_end, little_, error) != 0) {
                return EINVAL;
            }
            bool timestamp_ok = false;
            output->timestamp_ns =
                TimestampNs(timestamp, info.resolution, info.binary_resolution, info.tsoffset, &timestamp_ok);
            if (!timestamp_ok) return SetError(error, "pcapng timestamp overflow");
            output->captured_len = captured;
            output->wire_len = wire;
            output->link_type = info.link_type;
            output->source_id = info.source_id;
            output->sequence = info.sequence++;
            output->bytes.assign(data_.begin() + static_cast<std::ptrdiff_t>(bytes_pos),
                                 data_.begin() + static_cast<std::ptrdiff_t>(bytes_pos + captured));
            return 1;
        }
        if (have_section_ && interfaces_.empty()) return SetError(error, "pcapng section has no interface");
        return 0;
    }

    std::vector<uint8_t> data_;
    size_t pos_ = 0;
    Kind kind_ = Kind::kPcap;
    bool little_ = true;
    bool nanosecond_ = false;
    bool have_section_ = false;
    uint32_t network_ = 0;
    uint32_t snaplen_ = 0;
    uint64_t sequence_ = 0;
    uint32_t source_id_next_ = 0;
    std::vector<InterfaceInfo> interfaces_;
};

int ParsePcapFileSourceConfig(const std::string& option,
                              PcapFileSourceConfig* out,
                              std::string* normalized,
                              std::string* error) {
    if (!out) return EINVAL;
    rapidjson::Document document;
    document.Parse(option.empty() ? "{}" : option.c_str());
    if (document.HasParseError() || !document.IsObject()) return SetError(error, "pcapfile option must be a JSON object");
    for (auto it = document.MemberBegin(); it != document.MemberEnd(); ++it) {
        const std::string key = it->name.GetString();
        if (key != "path" && key != "format" && key != "batch_packets" && key != "replay_mode" && key != "replay_speed_milli") {
            return SetError(error, "unknown pcapfile option");
        }
    }
    if (!document.HasMember("path") || !document["path"].IsString() || document["path"].GetStringLength() == 0) {
        return SetError(error, "pcapfile option path is required");
    }
    out->path = document["path"].GetString();
    out->format = "auto";
    out->batch_packets = 256;
    out->replay_mode = PcapReplayMode::kFast;
    out->replay_speed_milli = 1000;
    if (document.HasMember("format")) {
        if (!document["format"].IsString()) return SetError(error, "format must be string");
        out->format = Lower(document["format"].GetString());
        if (out->format != "auto" && out->format != "pcap" && out->format != "pcapng") return SetError(error, "invalid format");
    }
    if (document.HasMember("batch_packets")) {
        if (!document["batch_packets"].IsUint() || document["batch_packets"].GetUint() == 0) return SetError(error, "batch_packets must be positive integer");
        out->batch_packets = document["batch_packets"].GetUint();
    }
    if (document.HasMember("replay_mode")) {
        if (!document["replay_mode"].IsString()) return SetError(error, "replay_mode must be string");
        const std::string mode = Lower(document["replay_mode"].GetString());
        if (mode == "fast") out->replay_mode = PcapReplayMode::kFast;
        else if (mode == "timestamp") out->replay_mode = PcapReplayMode::kTimestamp;
        else return SetError(error, "invalid replay_mode");
    }
    if (document.HasMember("replay_speed_milli")) {
        if (!document["replay_speed_milli"].IsUint() || document["replay_speed_milli"].GetUint() == 0) return SetError(error, "replay_speed_milli must be positive integer");
        out->replay_speed_milli = document["replay_speed_milli"].GetUint();
    }
    if (out->replay_mode != PcapReplayMode::kTimestamp && document.HasMember("replay_speed_milli")) {
        // The value is accepted and normalized even though it has no effect in fast mode.
    }
    if (!IsRegularFileNoSymlink(out->path)) return SetError(error, "pcapfile path must be a regular non-symlink file");

    if (normalized) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject();
        writer.Key("path"); writer.String(out->path.c_str());
        writer.Key("format"); writer.String(out->format.c_str());
        writer.Key("batch_packets"); writer.Uint(out->batch_packets);
        writer.Key("replay_mode"); writer.String(out->replay_mode == PcapReplayMode::kTimestamp ? "timestamp" : "fast");
        writer.Key("replay_speed_milli"); writer.Uint(out->replay_speed_milli);
        writer.EndObject();
        *normalized = buffer.GetString();
    }
    return 0;
}

PcapFileChannel::PcapFileChannel(std::string name, PcapFileSourceConfig config, IProtocol* protocol,
                                 PcapReplayWaiter replay_waiter)
    : name_(std::move(name)),
      config_(std::move(config)),
      protocol_(protocol),
      replay_waiter_(std::move(replay_waiter)) {}

PcapFileChannel::~PcapFileChannel() { Close(); }

int PcapFileChannel::Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) return 0;
    if (!outstanding_.empty()) return EBUSY;
    if (!protocol_) return ENODEV;
    auto reader = std::make_unique<PcapFileReader>();
    std::string error;
    const int rc = reader->Open(config_.path, config_.format, &error);
    if (rc != 0) {
        error_code_ = rc;
        error_message_ = std::move(error);
        error_sent_ = false;
        finished_ = true;
        return error_code_;
    }
    reader_ = std::move(reader);
    opened_ = true;
    cancelled_ = false;
    finished_ = false;
    eof_sent_ = false;
    error_sent_ = false;
    error_code_ = 0;
    error_message_.clear();
    previous_timestamp_ns_ = 0;
    have_previous_timestamp_ = false;
    replay_remainder_ = 0;
    return 0;
}

int PcapFileChannel::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!outstanding_.empty() || poll_in_progress_) return EBUSY;
    opened_ = false;
    cancelled_ = true;
    finished_ = true;
    reader_.reset();
    state_cv_.notify_all();
    return 0;
}

bool PcapFileChannel::IsOpened() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_;
}

BlockPollEvent PcapFileChannel::PollBlock(int timeout_ms) {
    std::vector<packet::PacketRecord> records;
    std::chrono::nanoseconds replay_delay(0);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!opened_) {
            if (error_code_ != 0 && !error_sent_) {
                error_sent_ = true;
                return BlockPollEvent{BlockPollEvent::kError, nullptr, error_code_};
            }
            if (cancelled_ || finished_) {
                return BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED};
            }
            return BlockPollEvent{BlockPollEvent::kError, nullptr, EINVAL};
        }
        if (cancelled_) return BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED};
        const auto can_poll = [this]() {
            return cancelled_ || !opened_ || (!poll_in_progress_ && outstanding_.empty());
        };
        if (poll_in_progress_ || !outstanding_.empty()) {
            if (timeout_ms <= 0) return BlockPollEvent{BlockPollEvent::kTimeout, nullptr, 0};
            if (!state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), can_poll)) {
                return BlockPollEvent{BlockPollEvent::kTimeout, nullptr, 0};
            }
            if (!opened_) return BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED};
            if (cancelled_) return BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED};
        }
        if (finished_) {
            if (error_code_ != 0 && !error_sent_) {
                error_sent_ = true;
                return BlockPollEvent{BlockPollEvent::kError, nullptr, error_code_};
            }
            if (!eof_sent_ && error_code_ == 0) {
                eof_sent_ = true;
                return BlockPollEvent{BlockPollEvent::kEof, nullptr, 0};
            }
            return BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED};
        }
        const uint32_t packet_limit =
            config_.replay_mode == PcapReplayMode::kTimestamp ? 1 : config_.batch_packets;
        records.reserve(packet_limit);
        PcapLayerAdapter layer_adapter(protocol_);
        std::string error;
        for (uint32_t index = 0; index < packet_limit; ++index) {
            CaptureRecord capture;
            const int rc = reader_->Next(&capture, &error);
            if (rc != 0 && rc != 1) {
                error_code_ = rc;
                error_message_ = std::move(error);
                finished_ = true;
                if (records.empty()) {
                    error_sent_ = true;
                    return BlockPollEvent{BlockPollEvent::kError, nullptr, error_code_};
                }
                break;
            }
            if (rc == 0) {
                finished_ = true;
                if (records.empty()) {
                    eof_sent_ = true;
                    return BlockPollEvent{BlockPollEvent::kEof, nullptr, 0};
                }
                break;
            }
            packet::PacketRecord record;
            record.meta.timestamp_ns = capture.timestamp_ns;
            record.meta.captured_len = capture.captured_len;
            record.meta.wire_len = capture.wire_len;
            record.meta.link_type = capture.link_type;
            record.meta.source_id = capture.source_id;
            record.meta.sequence = capture.sequence;
            auto owner = std::make_shared<std::vector<uint8_t>>(std::move(capture.bytes));
            record.raw_data.owner = owner;
            record.raw_data.data = owner->empty() ? nullptr : owner->data();
            record.raw_data.size = static_cast<uint32_t>(owner->size());
            if (record.meta.link_type == 1 && record.meta.captured_len != 0) {
                packet::PacketView view;
                view.meta = record.meta;
                view.bytes = Span<const uint8_t>(record.raw_data.data, record.raw_data.size);
                record.layer = layer_adapter.Decode(view);
            } else {
                record.layer.status = record.meta.captured_len == 0 ? packet::LayerStatus::kTruncated
                                                                      : packet::LayerStatus::kUnsupportedLinkType;
            }
            records.push_back(std::move(record));
            if (config_.replay_mode == PcapReplayMode::kTimestamp && have_previous_timestamp_) {
                const __int128 delta = static_cast<__int128>(capture.timestamp_ns) - previous_timestamp_ns_;
                if (delta > 0) {
                    const __int128 scaled = delta * 1000 + replay_remainder_;
                    const __int128 delay_ns = scaled / config_.replay_speed_milli;
                    replay_remainder_ = static_cast<uint32_t>(scaled % config_.replay_speed_milli);
                    const int64_t bounded_delay = delay_ns > std::numeric_limits<int64_t>::max()
                                                      ? std::numeric_limits<int64_t>::max()
                                                      : static_cast<int64_t>(delay_ns);
                    replay_delay = std::chrono::nanoseconds(bounded_delay);
                }
            }
            previous_timestamp_ns_ = capture.timestamp_ns;
            have_previous_timestamp_ = true;
        }
        poll_in_progress_ = true;
    }
    const auto finish_poll = [this](BlockPollEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        poll_in_progress_ = false;
        state_cv_.notify_all();
        return event;
    };
    if (replay_delay.count() > 0) {
        if (replay_waiter_) {
            replay_waiter_(replay_delay);
        } else {
            std::unique_lock<std::mutex> lock(mutex_);
            state_cv_.wait_for(lock, replay_delay, [this]() { return cancelled_ || !opened_; });
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_ || !opened_) {
                poll_in_progress_ = false;
                state_cv_.notify_all();
                return BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED};
            }
        }
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    const auto rc = packet::EncodePacketBatch(records, &batch, &error);
    if (rc != packet::PacketBatchError::kNone || !batch) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_code_ = EINVAL;
            error_message_ = error.empty() ? "encode packet block failed" : std::move(error);
            finished_ = true;
            error_sent_ = true;
        }
        return finish_poll(BlockPollEvent{BlockPollEvent::kError, nullptr, EINVAL});
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_ || !opened_) {
            poll_in_progress_ = false;
            state_cv_.notify_all();
            return BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED};
        }
        outstanding_.insert(batch.get());
        poll_in_progress_ = false;
        state_cv_.notify_all();
    }
    return BlockPollEvent{BlockPollEvent::kData, std::move(batch), 0};
}

int PcapFileChannel::ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) {
    if (!block) return EINVAL;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = outstanding_.find(block.get());
    if (it == outstanding_.end()) return EINVAL;
    outstanding_.erase(it);
    state_cv_.notify_all();
    return 0;
}

void PcapFileChannel::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    finished_ = true;
    state_cv_.notify_all();
}

bool PcapFileChannel::IsFinished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
}

size_t PcapFileChannel::OutstandingBatchCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_.size();
}

bool PcapFileChannel::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return poll_in_progress_ || !outstanding_.empty();
}

int PcapFilePlugin::Option(const char* arg) {
    plugin_option_ = arg ? arg : "";
    return 0;
}

int PcapFilePlugin::Load(IQuerier* querier) {
    std::lock_guard<std::mutex> lock(mutex_);
    querier_ = querier;
    protocol_ = nullptr;
    if (querier_) {
        querier_->Traverse(IID_PROTOCOL, [&](void* value) {
            if (!protocol_) protocol_ = static_cast<IProtocol*>(value);
            return 0;
        });
    }
    return protocol_ ? 0 : ENODEV;
}

int PcapFilePlugin::Unload() {
    std::lock_guard<std::mutex> lock(mutex_);
    const int stop_rc = StopChannelsLocked();
    if (stop_rc != 0) return stop_rc;
    channels_.clear();
    options_.clear();
    protocol_ = nullptr;
    querier_ = nullptr;
    return 0;
}

int PcapFilePlugin::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    return StopChannelsLocked();
}

int PcapFilePlugin::StopChannelsLocked() {
    for (const auto& item : channels_) {
        if (item.second && item.second->IsBusy()) return EBUSY;
    }
    for (auto& item : channels_) {
        if (!item.second) continue;
        const int rc = item.second->Close();
        if (rc != 0) return rc;
    }
    return 0;
}

IBlockStreamChannel* PcapFilePlugin::Get(const char* type, const char* name) {
    if (!type || !name || Lower(type) != "pcapfile") return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(MakeKey(type, name));
    return it == channels_.end() ? nullptr : it->second.get();
}

void PcapFilePlugin::List(std::function<void(const char*, const char*, IBlockStreamChannel*)> callback) {
    if (!callback) return;
    std::vector<std::shared_ptr<PcapFileChannel>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : channels_) snapshot.push_back(item.second);
    }
    for (const auto& channel : snapshot) callback("pcapfile", channel->Name(), channel.get());
}

int PcapFilePlugin::BuildChannel(const std::string& name, const std::string& option,
                                 std::shared_ptr<PcapFileChannel>* out, std::string* error) {
    if (!out) return EINVAL;
    out->reset();
    PcapFileSourceConfig config;
    std::string normalized;
    const int rc = ParsePcapFileSourceConfig(option, &config, &normalized, error);
    if (rc != 0) return rc;
    auto channel = std::make_shared<PcapFileChannel>(name, config, protocol_);
    channel->SetNormalizedOption(normalized);
    const int open_rc = channel->Open();
    if (open_rc != 0) {
        if (error && error->empty()) *error = "open pcapfile channel failed";
        return open_rc;
    }
    *out = std::move(channel);
    return 0;
}

int PcapFilePlugin::AddChannel(const std::string& type, const std::string& name, const std::string& option) {
    if (Lower(type) != "pcapfile") return ENOTSUP;
    if (name.empty()) return EINVAL;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = MakeKey(type, name);
    if (channels_.find(key) != channels_.end()) return EEXIST;
    std::shared_ptr<PcapFileChannel> channel;
    std::string error;
    const int rc = BuildChannel(name, option, &channel, &error);
    if (rc != 0) return rc;
    options_[key] = channel->Option();
    channels_[key] = std::move(channel);
    return 0;
}

int PcapFilePlugin::ModifyChannel(const std::string& type, const std::string& name, const std::string& option) {
    if (Lower(type) != "pcapfile") return ENOTSUP;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = MakeKey(type, name);
    auto it = channels_.find(key);
    if (it == channels_.end()) return ENOENT;
    if (it->second->IsBusy()) return EBUSY;
    std::shared_ptr<PcapFileChannel> next;
    std::string error;
    const int rc = BuildChannel(name, option, &next, &error);
    if (rc != 0) return rc;
    const int close_rc = it->second->Close();
    if (close_rc != 0) return close_rc;
    it->second = std::move(next);
    options_[key] = it->second->Option();
    return 0;
}

int PcapFilePlugin::RemoveChannel(const std::string& type, const std::string& name) {
    if (Lower(type) != "pcapfile") return ENOTSUP;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = MakeKey(type, name);
    auto it = channels_.find(key);
    if (it == channels_.end()) return ENOENT;
    if (it->second->IsBusy()) return EBUSY;
    const int close_rc = it->second->Close();
    if (close_rc != 0) return close_rc;
    channels_.erase(it);
    options_.erase(key);
    return 0;
}

void PcapFilePlugin::QueryChannels(std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> callback) {
    if (!callback) return;
    std::vector<std::tuple<std::string, std::string, std::string, std::string>> values;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : channels_) {
            const auto dot = item.first.find('.');
            const std::string name = dot == std::string::npos ? item.first : item.first.substr(dot + 1);
            const std::string status = item.second->IsFinished() ? "stopped" : "running";
            values.emplace_back("pcapfile", name, options_[item.first], status);
        }
    }
    for (const auto& value : values) callback(std::get<0>(value), std::get<1>(value), std::get<2>(value), std::get<3>(value));
}

std::string PcapFilePlugin::MakeKey(const std::string& type, const std::string& name) {
    return Lower(type) + "." + name;
}

}  // namespace flowsql::channels::pcapfile
