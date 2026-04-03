#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <chrono>
#include <common/error_code.h>
#include <common/log.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_set>

#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/fan_in_stream_channel.h"
#include "framework/core/fan_out_stream_channel.h"
#include "framework/core/pipeline.h"
#include "framework/core/ring_stream_channel.h"
#include "framework/core/sql_parser.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ibridge.h"
#include "framework/interfaces/ioperator.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "framework/interfaces/ioperator_registry.h"
#include "framework/interfaces/istream_channel.h"
#include "framework/interfaces/istream_factory.h"

namespace flowsql {
namespace scheduler {

static std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool IEquals(const std::string& a, const std::string& b) {
    return ToLowerAscii(a) == ToLowerAscii(b);
}

static bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

static bool IsDataFrameRef(const std::string& name) {
    return StartsWithIgnoreCase(name, "dataframe.") && name.size() > strlen("dataframe.");
}

static std::string DataFrameNamePart(const std::string& name) {
    if (!IsDataFrameRef(name)) return "";
    return name.substr(strlen("dataframe."));
}

static bool IsStreamRef(const std::string& name) {
    return StartsWithIgnoreCase(name, "stream.") && name.size() > strlen("stream.");
}

static std::string StreamNamePart(const std::string& name) {
    if (!IsStreamRef(name)) return "";
    return name.substr(strlen("stream."));
}

static bool ParseDatabaseDestination(const std::string& dest,
                                     std::string* db_type,
                                     std::string* db_name,
                                     std::string* table_name) {
    if (!db_type || !db_name || !table_name) return false;
    db_type->clear();
    db_name->clear();
    table_name->clear();

    const auto first = dest.find('.');
    if (first == std::string::npos || first == 0 || first >= dest.size() - 1) {
        return false;
    }
    *db_type = ToLowerAscii(dest.substr(0, first));

    const auto second = dest.find('.', first + 1);
    if (second == std::string::npos) {
        *db_name = dest.substr(first + 1);
        return !db_name->empty();
    }

    if (second == first + 1 || second >= dest.size() - 1) {
        return false;
    }
    *db_name = dest.substr(first + 1, second - first - 1);
    *table_name = dest.substr(second + 1);
    return !db_name->empty() && !table_name->empty();
}

static bool IsQualifiedDestination(const std::string& dest) {
    // 合法目标：
    // 1) dataframe.<name>
    // 2) type.name 或 type.name.table
    if (dest.empty()) return false;
    if (IsDataFrameRef(dest)) return true;
    const auto first = dest.find('.');
    if (first == std::string::npos || first == 0 || first == dest.size() - 1) return false;
    const auto second = dest.find('.', first + 1);
    if (second == first + 1) return false;
    if (second != std::string::npos && second == dest.size() - 1) return false;
    return true;
}

// --- JSON 辅助 ---
static std::string MakeErrorJson(const std::string& error) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string MakeExecutionErrorJson(const std::string& error,
                                          const std::string& error_code,
                                          const std::string& error_stage) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("error_stage");
    w.String(error_stage.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string ExtractStageFromExecutionError(const std::string& error) {
    // Pipeline::Run 失败消息：operator <category>.<name> execution failed
    static const std::regex kPattern(R"(^operator\s+([^.]+)\.([^\s]+)\s+execution failed$)",
                                     std::regex_constants::icase);
    std::smatch m;
    if (!std::regex_match(error, m, kPattern)) return "";
    if (m.size() < 3) return "";
    return m[2].str();
}

static const char* DataTypeName(DataType t) {
    switch (t) {
        case DataType::INT32: return "INT32";
        case DataType::INT64: return "INT64";
        case DataType::UINT32: return "UINT32";
        case DataType::UINT64: return "UINT64";
        case DataType::FLOAT: return "FLOAT";
        case DataType::DOUBLE: return "DOUBLE";
        case DataType::STRING: return "STRING";
        case DataType::BYTES: return "BYTES";
        case DataType::TIMESTAMP: return "TIMESTAMP";
        case DataType::BOOLEAN: return "BOOLEAN";
        default: return "UNKNOWN";
    }
}

static int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string MakeWithParamsJson(const std::unordered_map<std::string, std::string>& params) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    for (const auto& kv : params) {
        w.Key(kv.first.c_str());
        w.String(kv.second.c_str());
    }
    w.EndObject();
    return buf.GetString();
}

static size_t NextPowerOfTwo(size_t value) {
    if (value <= 1) return 1;
    size_t v = value - 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if (sizeof(size_t) >= 8) {
        v |= v >> 32;
    }
    return v + 1;
}

static const char* StreamTaskStatusName(StreamTaskStatus status) {
    switch (status) {
        case StreamTaskStatus::kCreated: return "created";
        case StreamTaskStatus::kRunning: return "running";
        case StreamTaskStatus::kStopping: return "stopping";
        case StreamTaskStatus::kStopped: return "stopped";
        case StreamTaskStatus::kCancelled: return "cancelled";
        case StreamTaskStatus::kFailed: return "failed";
        default: return "unknown";
    }
}

static void WriteTaskSnapshotJson(rapidjson::Writer<rapidjson::StringBuffer>* w,
                                  const TaskSnapshot& s) {
    if (!w) return;
    w->StartObject();
    w->Key("task_id");
    w->String(s.task_id.c_str());
    w->Key("status");
    w->String(StreamTaskStatusName(s.status));
    w->Key("stop_requested");
    w->Bool(s.stop_requested);
    w->Key("joined");
    w->Bool(s.joined);
    w->Key("shard_count");
    w->Uint(s.shard_count);
    w->Key("active_shards");
    w->Uint(s.active_shards);

    w->Key("processed_batches");
    w->Uint64(s.processed_batches);
    w->Key("processed_rows");
    w->Uint64(s.processed_rows);
    w->Key("processed_bytes");
    w->Uint64(s.processed_bytes);
    w->Key("output_rows");
    w->Uint64(s.output_rows);
    w->Key("output_batches");
    w->Uint64(s.output_batches);
    w->Key("dropped_batches");
    w->Uint64(s.dropped_batches);
    w->Key("poll_timeouts");
    w->Uint64(s.poll_timeouts);
    w->Key("poll_errors");
    w->Uint64(s.poll_errors);
    w->Key("queue_depth");
    w->Uint64(s.queue_depth);
    w->Key("queue_depth_peak");
    w->Uint64(s.queue_depth_peak);
    w->Key("uptime_ms");
    w->Int64(s.uptime_ms);
    w->Key("started_ms");
    w->Int64(s.started_ms);
    w->Key("last_active_ms");
    w->Int64(s.last_active_ms);
    w->Key("finished_ms");
    w->Int64(s.finished_ms);
    w->Key("error_code");
    w->Int(s.error_code);
    w->Key("error_message");
    w->String(s.error_message.c_str());

    rapidjson::Document stats_doc;
    if (!s.op_stats_json.empty()) {
        stats_doc.Parse(s.op_stats_json.c_str());
    }
    w->Key("op_stats");
    if (stats_doc.HasParseError()) {
        w->String(s.op_stats_json.c_str());
    } else {
        rapidjson::StringBuffer stats_buf;
        rapidjson::Writer<rapidjson::StringBuffer> stats_writer(stats_buf);
        stats_doc.Accept(stats_writer);
        w->RawValue(stats_buf.GetString(), stats_buf.GetSize(),
                    stats_doc.IsArray() ? rapidjson::kArrayType : rapidjson::kObjectType);
    }
    w->EndObject();
}

class SharedSpmcState final : public std::enable_shared_from_this<SharedSpmcState> {
 public:
    SharedSpmcState(std::string category,
                    std::string name,
                    std::shared_ptr<IStreamChannel> source,
                    size_t ring_size)
        : category_(std::move(category)),
          name_(std::move(name)),
          source_(std::move(source)) {
        RingStreamChannelOptions opts;
        opts.ring_mode = RingMode::SPMC;
        opts.overflow = OverflowPolicy::kBlock;
        opts.finite = source_ ? source_->IsFinite() : false;
        opts.ring_size = NextPowerOfTwo(std::max<size_t>(64, ring_size));
        queue_ = std::make_shared<RingStreamChannel>("spmc", name_, opts);
        (void)queue_->Open();
    }

    ~SharedSpmcState() {
        Stop();
    }

    int Start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return 0;
        }
        if (!source_) return EINVAL;
        producer_finished_.store(false, std::memory_order_release);
        stop_requested_.store(false, std::memory_order_release);
        dispatch_thread_ = std::thread([self = shared_from_this()]() { self->DispatchLoop(); });
        return 0;
    }

    void Stop() {
        stop_requested_.store(true, std::memory_order_release);
        std::call_once(cancel_once_, [this]() {
            if (source_) source_->Cancel();
        });
        {
            std::lock_guard<std::mutex> lock(dispatch_join_mu_);
            if (dispatch_thread_.joinable() &&
                dispatch_thread_.get_id() != std::this_thread::get_id()) {
                dispatch_thread_.join();
            }
        }
        producer_finished_.store(true, std::memory_order_release);
        if (queue_) {
            queue_->Close();
        }
    }

    PollEvent PollNext(int timeout_ms) {
        if (timeout_ms < 0) timeout_ms = 0;
        const PollEvent ev = queue_->PollNext(timeout_ms);
        if (ev.kind == PollEventKind::kData) {
            return ev;
        }
        const int err = error_code_.load(std::memory_order_acquire);
        if (err != 0) {
            return PollEvent::Error(err, error_message_);
        }
        if (producer_finished_.load(std::memory_order_acquire) && queue_->IsEmpty()) {
            JoinDispatchThreadIfFinished();
            return PollEvent::Eof();
        }
        if (ev.kind == PollEventKind::kError &&
            ev.err != -EBADF && ev.err != -ECANCELED) {
            return ev;
        }
        return PollEvent::Timeout();
    }

    void Cancel() {
        std::call_once(cancel_once_, [this]() {
            if (source_) source_->Cancel();
        });
        stop_requested_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(dispatch_join_mu_);
        if (dispatch_thread_.joinable() &&
            dispatch_thread_.get_id() != std::this_thread::get_id()) {
            dispatch_thread_.join();
        }
    }

    int CloseView() {
        return 0;
    }

    bool IsFinished() const {
        const bool done = producer_finished_.load(std::memory_order_acquire) && queue_ && queue_->IsEmpty();
        if (done) {
            const_cast<SharedSpmcState*>(this)->JoinDispatchThreadIfFinished();
        }
        return done;
    }

    bool IsFull() const {
        return queue_ && queue_->IsFull();
    }

    bool IsEmpty() const {
        return !queue_ || queue_->IsEmpty();
    }

    size_t Capacity() const {
        return queue_ ? queue_->Capacity() : 0;
    }

    size_t Size() const {
        return queue_ ? queue_->Size() : 0;
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() {
        return source_ ? source_->GetOutputSchema() : nullptr;
    }

    const char* Category() const { return category_.c_str(); }
    const char* Name() const { return name_.c_str(); }
    const char* Schema() const { return schema_cache_.c_str(); }
    bool IsFinite() const { return source_ ? source_->IsFinite() : false; }

 private:
    void JoinDispatchThreadIfFinished() {
        if (!producer_finished_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(dispatch_join_mu_);
        if (dispatch_thread_.joinable()) {
            dispatch_thread_.join();
        }
    }

    void SetErrorOnce(int code, const std::string& msg) {
        int expected = 0;
        if (error_code_.compare_exchange_strong(expected, code, std::memory_order_acq_rel)) {
            error_message_ = msg;
        }
    }

    void DispatchLoop() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            PollEvent ev = source_->PollNext(100);
            if (ev.kind == PollEventKind::kTimeout) {
                if (source_->IsFinished() && source_->IsEmpty()) {
                    producer_finished_.store(true, std::memory_order_release);
                    return;
                }
                continue;
            }
            if (ev.kind == PollEventKind::kData) {
                while (!stop_requested_.load(std::memory_order_acquire)) {
                    int rc = queue_->Put(ev.batch.data, ev.batch.ts_ms);
                    if (rc == 0) break;
                    if (rc == EAGAIN) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    SetErrorOnce(-EIO, "shared spmc dispatch put failed");
                    producer_finished_.store(true, std::memory_order_release);
                    return;
                }
                continue;
            }
            if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) {
                producer_finished_.store(true, std::memory_order_release);
                return;
            }
            if (ev.kind == PollEventKind::kError) {
                SetErrorOnce(ev.err == 0 ? -EIO : ev.err,
                             ev.err_msg.empty() ? "shared spmc source poll failed" : ev.err_msg);
                producer_finished_.store(true, std::memory_order_release);
                return;
            }
        }
        producer_finished_.store(true, std::memory_order_release);
    }

    std::string category_;
    std::string name_;
    std::string schema_cache_ = "[]";
    std::shared_ptr<IStreamChannel> source_;
    std::shared_ptr<RingStreamChannel> queue_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> producer_finished_{false};
    std::once_flag cancel_once_;
    std::atomic<int> error_code_{0};
    std::string error_message_;
    std::mutex dispatch_join_mu_;
    std::thread dispatch_thread_;
};

class SharedSpmcInputView final : public IStreamChannel {
 public:
    SharedSpmcInputView(std::shared_ptr<SharedSpmcState> state, uint32_t view_id)
        : state_(std::move(state)),
          view_name_(state_ ? std::string(state_->Name()) + ".v" + std::to_string(view_id)
                            : ("spmc.v" + std::to_string(view_id))) {}

    const char* Category() override {
        return state_ ? state_->Category() : "spmc";
    }

    const char* Name() override {
        return view_name_.c_str();
    }

    const char* Type() override {
        return ChannelType::kStream;
    }

    const char* Schema() override {
        return state_ ? state_->Schema() : "[]";
    }

    int Open() override { return 0; }
    int Close() override { return state_ ? state_->CloseView() : 0; }
    bool IsOpened() const override { return true; }
    int Flush() override { return 0; }

    int Put(std::shared_ptr<arrow::RecordBatch>, int64_t) override {
        return ENOTSUP;
    }

    PollEvent PollNext(int timeout_ms = 100) override {
        if (!state_) return PollEvent::Error(-EINVAL, "invalid shared spmc state");
        return state_->PollNext(timeout_ms);
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() override {
        return state_ ? state_->GetOutputSchema() : nullptr;
    }

    int SetFilter(const char*, std::vector<std::string>* unsupported_out) override {
        if (unsupported_out) unsupported_out->clear();
        return 0;
    }

    bool IsFull() const override { return state_ && state_->IsFull(); }
    bool IsEmpty() const override { return !state_ || state_->IsEmpty(); }
    size_t Capacity() const override { return state_ ? state_->Capacity() : 0; }
    size_t Size() const override { return state_ ? state_->Size() : 0; }
    bool IsFinite() const override { return state_ && state_->IsFinite(); }
    void CloseStream() override {}
    void Cancel() override {
        if (state_) state_->Cancel();
    }
    bool IsFinished() const override {
        return state_ && state_->IsFinished();
    }

 private:
    std::shared_ptr<SharedSpmcState> state_;
    std::string view_name_;
};

class FanOutPartitionView final : public IStreamChannel {
 public:
    FanOutPartitionView(std::shared_ptr<FanOutStreamChannel> parent,
                        std::shared_ptr<IStreamChannel> partition)
        : parent_(std::move(parent)),
          partition_(std::move(partition)) {}

    const char* Category() override {
        return partition_ ? partition_->Category() : "fanout";
    }

    const char* Name() override {
        return partition_ ? partition_->Name() : "fanout.partition";
    }

    const char* Type() override {
        return ChannelType::kStream;
    }

    const char* Schema() override {
        return partition_ ? partition_->Schema() : "[]";
    }

    int Open() override { return 0; }
    int Close() override { return partition_ ? partition_->Close() : 0; }
    bool IsOpened() const override { return partition_ && partition_->IsOpened(); }
    int Flush() override { return partition_ ? partition_->Flush() : 0; }

    int Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) override {
        return partition_ ? partition_->Put(std::move(batch), ts_ms) : EINVAL;
    }

    PollEvent PollNext(int timeout_ms = 100) override {
        if (!partition_) return PollEvent::Error(-EINVAL, "fanout partition unavailable");
        return partition_->PollNext(timeout_ms);
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() override {
        return partition_ ? partition_->GetOutputSchema() : nullptr;
    }

    int SetFilter(const char* condition_json,
                  std::vector<std::string>* unsupported_out) override {
        return partition_ ? partition_->SetFilter(condition_json, unsupported_out) : EINVAL;
    }

    bool IsFull() const override { return partition_ && partition_->IsFull(); }
    bool IsEmpty() const override { return !partition_ || partition_->IsEmpty(); }
    size_t Capacity() const override { return partition_ ? partition_->Capacity() : 0; }
    size_t Size() const override { return partition_ ? partition_->Size() : 0; }
    bool IsFinite() const override { return partition_ && partition_->IsFinite(); }
    void CloseStream() override {
        if (partition_) partition_->CloseStream();
    }
    void Cancel() override {
        if (partition_) partition_->Cancel();
        if (parent_) parent_->Cancel();
    }
    bool IsFinished() const override { return partition_ && partition_->IsFinished(); }

 private:
    std::shared_ptr<FanOutStreamChannel> parent_;
    std::shared_ptr<IStreamChannel> partition_;
};

// --- IPlugin ---
int SchedulerPlugin::Option(const char* arg) {
    if (!arg) return 0;

    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();

        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);

        if (key == "host") host_ = val;
        else if (key == "port") port_ = std::stoi(val);
        else if (key == "stream_workers") stream_worker_count_ = static_cast<size_t>(std::stoull(val));

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int SchedulerPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    LOG_INFO("SchedulerPlugin::Load: host=%s, port=%d", host_.c_str(), port_);
    return 0;
}

int SchedulerPlugin::Unload() {
    return 0;
}

// --- 通道管理 ---
void SchedulerPlugin::RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch) {
    channels_[key] = std::move(ch);
}

// --- IPlugin::Start ---
int SchedulerPlugin::Start() {
    auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
    if (catalog) {
        std::vector<OperatorMeta> ops;
        std::unordered_set<std::string> seen;
        auto append_unique = [&](OperatorMeta meta) {
            const std::string key = ToLowerAscii(meta.category) + "." + ToLowerAscii(meta.name);
            if (seen.insert(key).second) {
                ops.push_back(std::move(meta));
            }
        };
        querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
            auto* op = static_cast<IOperator*>(p);
            if (!op || op->Category().empty() || op->Name().empty()) return 0;
            OperatorMeta meta;
            meta.category = op->Category();
            meta.name = op->Name();
            meta.type = IEquals(meta.category, "builtin") ? "builtin" : "cpp";
            meta.source = "scheduler";
            meta.description = op->Description();
            meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
            append_unique(std::move(meta));
            return 0;
        });
        auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));
        if (op_registry) {
            op_registry->List([&](const char* name) {
                if (!name || name[0] == '\0') return;
                if (std::string(name).find('.') != std::string::npos) return;
                OperatorMeta meta;
                meta.category = "builtin";
                meta.name = name;
                meta.type = "builtin";
                meta.source = "scheduler";
                meta.position = "data";
                IOperator* op = op_registry->Create(name);
                if (op) {
                    meta.description = op->Description();
                    meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
                    delete op;
                }
                append_unique(std::move(meta));
            });
        }
        UpsertResult upsert = catalog->UpsertBatch(ops);
        if (upsert.failed_count > 0) {
            LOG_ERROR("SchedulerPlugin::Start: catalog upsert failed, success=%d failed=%d err=%s",
                      upsert.success_count, upsert.failed_count, upsert.error_message.c_str());
        } else {
            LOG_INFO("SchedulerPlugin::Start: synced %d C++ operators to Catalog", upsert.success_count);
        }
    } else {
        LOG_ERROR("SchedulerPlugin::Start: IOperatorCatalog not found");
    }

    size_t workers = stream_worker_count_;
    if (workers == 0) {
        workers = static_cast<size_t>(std::thread::hardware_concurrency());
    }
    if (workers == 0) workers = 1;
    stream_runtime_.Start(workers);

    LOG_INFO("SchedulerPlugin::Start: ready, stream_workers=%zu", workers);
    return 0;
}

int SchedulerPlugin::Stop() {
    std::vector<std::shared_ptr<StreamTask>> tasks;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        tasks.reserve(stream_tasks_.size());
        for (const auto& kv : stream_tasks_) {
            if (kv.second) tasks.push_back(kv.second);
        }
    }

    for (const auto& task : tasks) {
        const StreamTaskStatus st = task->Status();
        if (st == StreamTaskStatus::kRunning ||
            st == StreamTaskStatus::kStopping ||
            st == StreamTaskStatus::kCreated) {
            task->RequestStop();
        }
    }
    for (const auto& task : tasks) {
        task->Join();
    }
    stream_runtime_.Stop();

    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        stream_tasks_.clear();
    }
    channels_.clear();
    LOG_INFO("SchedulerPlugin::Stop: done");
    return 0;
}

// --- IRouterHandle ---
void SchedulerPlugin::EnumRoutes(std::function<void(const RouteItem&)> cb) {
    // 任务执行
    cb({"POST", "/tasks/instant/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleExecute(u, req, rsp);
        }});
    cb({"POST", "/tasks/stream/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamExecute(u, req, rsp);
        }});
    cb({"POST", "/tasks/stream/stop",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStop(u, req, rsp);
        }});
    cb({"POST", "/tasks/stream/status",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStatus(u, req, rsp);
        }});
    cb({"GET", "/tasks/stream/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamList(u, req, rsp);
        }});
    // 流式通道查询（管理面最小字段）
    cb({"POST", "/channels/stream/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannels(u, req, rsp);
        }});
    // 内存通道查询
    cb({"POST", "/channels/dataframe/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    // 内存通道数据预览
    cb({"POST", "/channels/dataframe/preview",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandlePreviewDataframe(u, req, rsp);
        }});
    // Python 算子刷新
    cb({"POST", "/operators/python/refresh",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRefreshOperators(u, req, rsp);
        }});
}

// --- 通道查找辅助 ---
IChannel* SchedulerPlugin::FindChannel(const std::string& name) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    if (IsDataFrameRef(name) && ch_registry) {
        auto ch = ch_registry->Get(DataFrameNamePart(name).c_str());
        auto* df = dynamic_cast<IDataFrameChannel*>(ch.get());
        if (df) return df;
    }

    // 先在内部通道表中查找
    auto it = channels_.find(name);
    if (it != channels_.end()) return it->second.get();

    // 通过 IQuerier 遍历静态注册的通道
    IChannel* found = nullptr;
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* c = static_cast<IChannel*>(p);
            auto dot = name.find('.');
            bool category_and_name_match = false;
            if (dot != std::string::npos) {
                const std::string req_category = name.substr(0, dot);
                const std::string req_name = name.substr(dot + 1);
                category_and_name_match = IEquals(c->Category(), req_category) && std::string(c->Name()) == req_name;
            }
            if (category_and_name_match || std::string(c->Name()) == name) {
                found = c;
                return -1;  // 找到了，停止遍历
            }
            return 0;
        });
    }

    // 模糊匹配内部通道表
    if (!found) {
        // 【第四层】尝试通过 IDatabaseFactory 获取数据库通道
        // 支持三段式（type.name.table）和两段式（type.name）
        if (querier_) {
            auto* factory = static_cast<IDatabaseFactory*>(
                querier_->First(IID_DATABASE_FACTORY));
            if (factory) {
                // 尝试解析 type.name 格式
                auto pos = name.find('.');
                if (pos != std::string::npos) {
                    std::string type = ToLowerAscii(name.substr(0, pos));
                    std::string rest = name.substr(pos + 1);
                    // 对于三段式 type.name.table，取前两段作为 type.name
                    auto pos2 = rest.find('.');
                    std::string db_name = (pos2 != std::string::npos) ? rest.substr(0, pos2) : rest;

                    auto* db_ch = factory->Get(type.c_str(), db_name.c_str());
                    if (db_ch) found = db_ch;
                }
            }
        }
    }

    // 尝试通过 IStreamFactory 获取流式通道（type.name）
    if (!found && querier_) {
        auto* stream_factory = static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY));
        if (stream_factory) {
            auto pos = name.find('.');
            if (pos != std::string::npos) {
                const std::string type = ToLowerAscii(name.substr(0, pos));
                const std::string rest = name.substr(pos + 1);
                const auto pos2 = rest.find('.');
                const std::string stream_name = (pos2 != std::string::npos) ? rest.substr(0, pos2) : rest;
                auto* stream_ch = stream_factory->Get(type.c_str(), stream_name.c_str());
                if (stream_ch) found = stream_ch;
            }
        }
    }

    return found;
}

// --- 算子查找 ---
// 先查 C++ 静态算子（IQuerier），再查 Python 算子（IBridge）
std::shared_ptr<IOperator> SchedulerPlugin::FindOperator(const std::string& category, const std::string& name) {
    if (!querier_) return nullptr;
    auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));

    // 1. 先查 C++ 静态算子
    IOperator* found = nullptr;
    querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* op = static_cast<IOperator*>(p);
        if (IEquals(op->Category(), category) && op->Name() == name) {
            found = op;
            return -1;
        }
        return 0;
    });
    // C++ 算子由 PluginLoader 管理生命周期，用空 deleter 包装
    if (found) return std::shared_ptr<IOperator>(found, [](IOperator*) {});

    // 2. 再查 Python 算子（通过 IBridge）
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        auto py_op = bridge->FindOperator(category, name);
        if (py_op) return py_op;
    }

    // 3. 查注册表（兼容 "category.name" 与 legacy builtin name）
    if (op_registry) {
        const std::string key = category + "." + name;
        IOperator* op = op_registry->Create(key.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
    if (IEquals(category, "builtin") && op_registry) {
        IOperator* op = op_registry->Create(name.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
    return nullptr;
}

std::shared_ptr<IOperator> SchedulerPlugin::CreateOperator(const std::string& category,
                                                           const std::string& name) {
    if (!querier_) return nullptr;
    auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));
    if (op_registry) {
        const std::string key = category + "." + name;
        if (IOperator* op = op_registry->Create(key.c_str())) {
            return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
        }
        if (IEquals(category, "builtin")) {
            if (IOperator* op = op_registry->Create(name.c_str())) {
                return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
            }
        }
    }

    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        return bridge->FindOperator(category, name);
    }
    return nullptr;
}

// --- Build Database Query ---

// 从目标名称中提取表名（支持三段式 type.name.table）
static std::string ExtractTableName(const std::string& dest_name) {
    auto pos1 = dest_name.find('.');
    if (pos1 != std::string::npos) {
        auto pos2 = dest_name.find('.', pos1 + 1);
        if (pos2 != std::string::npos) {
            return dest_name.substr(pos2 + 1);  // 三段式
        }
        return dest_name.substr(pos1 + 1);  // 两段式
    }
    return dest_name;
}

// BuildQuery: 构建数据库查询语句
// 将 sql_part 中的第一个 FROM 子句替换为实际表名（支持多段式表名规范化）
// 子查询中的 FROM 保持原样，不做替换
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    std::string sql = stmt.sql_part;
    std::string table = ExtractTableName(source_name);

    // 匹配第一个 FROM 子句（支持多段式表名如 catalog.db.table）
    // 使用不区分大小写，兼容 "from" / "From" 等写法。
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w-]+\.)*[\w-]+))", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(sql, m, FROM_PATTERN)) {
        // 只替换第一个匹配（主查询的 FROM），子查询不受影响
        sql = sql.substr(0, m.position()) +
              m[1].str() + table +
              sql.substr(m.position() + m.length());
    }

    return sql;
}

// --- 辅助：对 DataFrame 通道应用 WHERE 过滤 ---
static std::shared_ptr<DataFrameChannel> ApplyDataFrameFilter(
    IDataFrameChannel* src, const std::string& where_clause, uint64_t seq) {
    DataFrame data;
    if (src->Read(&data) != 0 || data.RowCount() == 0) return nullptr;

    if (data.Filter(where_clause.c_str()) != 0) return nullptr;

    auto filtered = std::make_shared<DataFrameChannel>("_filter", std::to_string(seq));
    filtered->Open();
    filtered->Write(&data);
    return filtered;
}

// --- 无算子：纯数据搬运 ---
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink,
                                      const std::string& source_type,
                                      const std::string& sink_type,
                                      const SqlStatement& stmt, int64_t* rows_affected,
                                      std::string* error) {
    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        // DataFrame + WHERE → 先过滤再复制
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            return ChannelAdapter::CopyDataFrame(filtered.get(), dst);
        }
        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;
        std::string table = ExtractTableName(stmt.dest);

        // DataFrame + WHERE → 先过滤再写入
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            int64_t rows = ChannelAdapter::WriteFromDataFrame(filtered.get(), dst, table.c_str(), error);
            if (rows_affected) *rows_affected = rows;
            return (rows < 0) ? -1 : 0;
        }
        int64_t rows = ChannelAdapter::WriteFromDataFrame(src, dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        std::string query = BuildQuery(stmt.source, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst, error);
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        auto tmp = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
        tmp->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(src, query.c_str(), tmp.get(), error);
        if (rc != 0) return rc;

        std::string table = ExtractTableName(stmt.dest);
        int64_t rows = ChannelAdapter::WriteFromDataFrame(tmp.get(), dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (error) *error = "unsupported transfer: " + source_type + " → " + sink_type;
    return -1;
}

// --- 有算子：自动适配通道类型 ---
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink,
                                          IOperator* op,
                                          const std::string& sink_type,
                                          const SqlStatement& stmt, int64_t* rows_affected,
                                          std::string* error) {
    std::vector<IOperator*> ops;
    std::vector<IChannel*> inputs;
    inputs.push_back(source);
    ops.push_back(op);
    return ExecuteWithOperatorChain(Span<IChannel*>(inputs), sink, ops, sink_type, stmt, rows_affected, error);
}

int SchedulerPlugin::ExecuteWithOperatorChain(Span<IChannel*> inputs, IChannel* sink,
                                              const std::vector<IOperator*>& ops,
                                              const std::string& sink_type,
                                              const SqlStatement& stmt, int64_t* rows_affected,
                                              std::string* error) {
    if (ops.empty() || inputs.empty()) return -1;

    Span<IChannel*> stage_inputs = inputs;
    std::shared_ptr<DataFrameChannel> tmp_in;
    std::vector<IChannel*> stage_input_holder;
    std::vector<std::shared_ptr<DataFrameChannel>> stage_buffers;

    // 单源算子路径仍保留 Database/DataFrame 自动适配；多源在 HandleExecute 限制为 dataframe.*
    if (stage_inputs.size == 1) {
        IChannel* single = stage_inputs[0];
        const std::string source_type(single->Type());

        if (source_type == ChannelType::kDatabase) {
            auto* db_src = dynamic_cast<IDatabaseChannel*>(single);
            if (!db_src) return -1;

            tmp_in = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
            tmp_in->Open();

            std::string query = BuildQuery(stmt.source, stmt);
            int rc = ChannelAdapter::ReadToDataFrame(db_src, query.c_str(), tmp_in.get(), error);
            if (rc != 0) return rc;

            stage_input_holder.clear();
            stage_input_holder.push_back(tmp_in.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        } else if (source_type == ChannelType::kDataFrame && !stmt.where_clause.empty()) {
            auto* df_src = dynamic_cast<IDataFrameChannel*>(single);
            if (!df_src) return -1;

            tmp_in = ApplyDataFrameFilter(df_src, stmt.where_clause, ++tmp_channel_seq_);
            if (!tmp_in) return -1;

            stage_input_holder.clear();
            stage_input_holder.push_back(tmp_in.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        }
    } else {
        for (size_t i = 0; i < stage_inputs.size; ++i) {
            auto* df = dynamic_cast<IDataFrameChannel*>(stage_inputs[i]);
            if (!df) {
                if (error) *error = "multi-source operator input must be dataframe channel";
                return -1;
            }
        }
    }

    for (size_t i = 0; i < ops.size(); ++i) {
        std::shared_ptr<DataFrameChannel> stage_out;
        IChannel* actual_sink = nullptr;
        if (i + 1 == ops.size()) {
            if (sink_type == ChannelType::kDatabase) {
                stage_out = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
                stage_out->Open();
                actual_sink = stage_out.get();
            } else {
                actual_sink = sink;
            }
        } else {
            stage_out = std::make_shared<DataFrameChannel>("_pipe", std::to_string(++tmp_channel_seq_));
            stage_out->Open();
            actual_sink = stage_out.get();
        }

        if (ops[i]->Work(stage_inputs, actual_sink) != 0) {
            if (error && error->empty()) {
                std::string op_error = ops[i]->LastError();
                if (!op_error.empty()) {
                    *error = op_error;
                } else {
                    *error = "operator " + ops[i]->Category() + "." + ops[i]->Name() + " execution failed";
                }
            }
            return -1;
        }

        if (stage_out) {
            stage_buffers.push_back(stage_out);
            stage_input_holder.clear();
            stage_input_holder.push_back(stage_out.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        }
    }

    if (sink_type == ChannelType::kDatabase) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;
        if (stage_buffers.empty()) return -1;

        std::string table = ExtractTableName(stmt.dest);
        int64_t written_rows = ChannelAdapter::WriteFromDataFrame(stage_buffers.back().get(), db_sink, table.c_str(), error);
        if (written_rows < 0) return -1;

        if (rows_affected) *rows_affected = written_rows;
    }

    return 0;
}

std::string SchedulerPlugin::NextStreamTaskId() {
    const uint64_t seq = stream_task_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream oss;
    oss << "stream_task_" << CurrentTimeMs() << "_" << seq;
    return oss.str();
}

int32_t SchedulerPlugin::ResolveStreamSink(
    const SqlStatement& stmt,
    SinkBinding* binding,
    std::string* err_out) {
    if (!binding) {
        if (err_out) *err_out = "invalid sink binding target";
        return error::BAD_REQUEST;
    }
    binding->sink_channel.reset();
    binding->sink_type.clear();
    binding->db_type.clear();
    binding->db_name.clear();
    binding->table_name.clear();

    if (stmt.dest.empty()) {
        if (err_out) *err_out = "stream task requires INTO destination";
        return error::BAD_REQUEST;
    }

    if (IsStreamRef(stmt.dest)) {
        auto* stream_factory = querier_
            ? static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY))
            : nullptr;
        if (!stream_factory) {
            if (err_out) *err_out = "stream factory unavailable";
            return error::UNAVAILABLE;
        }

        const std::string sink_name = StreamNamePart(stmt.dest);
        IStreamChannel* matched = nullptr;
        bool ambiguous = false;
        stream_factory->List([&](const char*, const char* name, IStreamChannel* ch) {
            if (!name || !ch) return;
            if (sink_name != name) return;
            if (matched && matched != ch) {
                ambiguous = true;
                return;
            }
            matched = ch;
        });
        if (ambiguous) {
            if (err_out) *err_out = "ambiguous stream sink name: " + sink_name;
            return error::CONFLICT;
        }
        if (!matched) {
            if (err_out) *err_out = "stream sink not found: " + stmt.dest;
            return error::NOT_FOUND;
        }

        auto output = std::shared_ptr<IStreamChannel>(matched, [](IStreamChannel*) {});
        if (output && !output->IsOpened()) {
            (void)output->Open();
        }
        binding->sink_channel = std::static_pointer_cast<IChannel>(output);
        binding->sink_type = ChannelType::kStream;
        return error::OK;
    }

    if (IsDataFrameRef(stmt.dest)) {
        auto* ch_registry = querier_
            ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY))
            : nullptr;
        if (!ch_registry) {
            if (err_out) *err_out = "channel registry unavailable";
            return error::UNAVAILABLE;
        }

        const std::string df_name = DataFrameNamePart(stmt.dest);
        std::shared_ptr<IChannel> df_holder = ch_registry->Get(df_name.c_str());
        if (!df_holder) {
            auto created = std::make_shared<DataFrameChannel>("dataframe", df_name);
            (void)created->Open();
            if (ch_registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(created)) != 0) {
                // 处理并发注册：重查一次
                df_holder = ch_registry->Get(df_name.c_str());
                if (!df_holder) {
                    if (err_out) *err_out = "register dataframe sink failed: " + stmt.dest;
                    return error::INTERNAL_ERROR;
                }
            } else {
                df_holder = std::static_pointer_cast<IChannel>(created);
            }
        }

        auto appendable = std::dynamic_pointer_cast<IAppendableDataFrameChannel>(df_holder);
        if (!appendable) {
            if (err_out) *err_out = "dataframe sink is not appendable: " + stmt.dest;
            return error::BAD_REQUEST;
        }
        if (!appendable->IsOpened()) {
            (void)appendable->Open();
        }

        binding->sink_channel = std::static_pointer_cast<IChannel>(appendable);
        binding->sink_type = ChannelType::kDataFrame;
        return error::OK;
    }

    std::string db_type;
    std::string db_name;
    std::string table_from_dest;
    if (!ParseDatabaseDestination(stmt.dest, &db_type, &db_name, &table_from_dest)) {
        if (err_out) *err_out = "invalid INTO destination: " + stmt.dest +
            ", expected stream.<name>, dataframe.<name>, or <db_type>.<db_name>[.<table>]";
        return error::BAD_REQUEST;
    }

    if (IChannel* existing = FindChannel(stmt.dest); existing) {
        if (!existing->IsOpened()) {
            (void)existing->Open();
        }
        binding->sink_channel = std::shared_ptr<IChannel>(existing, [](IChannel*) {});
        binding->sink_type = existing->Type() ? existing->Type() : "";
        if (binding->sink_type == ChannelType::kDatabase) {
            binding->db_type = db_type;
            binding->db_name = db_name;
            binding->table_name = table_from_dest;
        }
        return error::OK;
    }

    auto* db_factory = querier_
        ? static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY))
        : nullptr;
    if (!db_factory) {
        if (err_out) *err_out = "database factory unavailable";
        return error::UNAVAILABLE;
    }

    IDatabaseChannel* db_raw = db_factory->Get(db_type.c_str(), db_name.c_str());
    if (!db_raw) {
        if (err_out) *err_out = "database channel not found: " + db_type + "." + db_name;
        return error::NOT_FOUND;
    }

    auto db_sink = std::shared_ptr<IDatabaseChannel>(db_raw, [](IDatabaseChannel*) {});
    if (!db_sink->IsOpened()) {
        (void)db_sink->Open();
    }

    binding->sink_channel = std::static_pointer_cast<IChannel>(db_sink);
    binding->sink_type = ChannelType::kDatabase;
    binding->db_type = db_type;
    binding->db_name = db_name;
    binding->table_name = table_from_dest;
    return error::OK;
}

int32_t SchedulerPlugin::ExecuteStreamTask(const SqlStatement& stmt, std::string& rsp) {
    if (!querier_) {
        rsp = MakeErrorJson("querier not initialized");
        return error::INTERNAL_ERROR;
    }

    if (stmt.dest.empty()) {
        rsp = MakeErrorJson("stream task requires INTO destination");
        return error::BAD_REQUEST;
    }
    if (!IsQualifiedDestination(stmt.dest)) {
        rsp = MakeErrorJson("invalid INTO destination: " + stmt.dest);
        return error::BAD_REQUEST;
    }

    std::vector<OperatorRef> parsed_ops = stmt.operators;
    if (parsed_ops.empty() && !stmt.op_category.empty() && !stmt.op_name.empty()) {
        parsed_ops.push_back({stmt.op_category, stmt.op_name});
    }
    if (parsed_ops.empty()) {
        rsp = MakeErrorJson("stream task requires USING stream operator");
        return error::BAD_REQUEST;
    }
    if (parsed_ops.size() != 1) {
        rsp = MakeErrorJson("stream task currently supports single USING operator");
        return error::BAD_REQUEST;
    }
    const OperatorRef& op_ref = parsed_ops[0];

    auto* catalog = static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG));
    if (!catalog) {
        rsp = MakeErrorJson("operator catalog unavailable");
        return error::UNAVAILABLE;
    }
    OperatorStatus status = catalog->QueryStatus(op_ref.category, op_ref.name);
    if (status == OperatorStatus::kNotFound) {
        rsp = MakeErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
        return error::NOT_FOUND;
    }
    if (status == OperatorStatus::kDeactivated) {
        rsp = MakeErrorJson("operator is deactivated: " + op_ref.category + "." + op_ref.name);
        return error::CONFLICT;
    }

    std::vector<std::shared_ptr<IStreamChannel>> source_channels;
    source_channels.reserve(stmt.sources.size());
    for (const auto& source_name : stmt.sources) {
        IChannel* raw = FindChannel(source_name);
        if (!raw) {
            rsp = MakeErrorJson("source channel not found: " + source_name);
            return error::NOT_FOUND;
        }
        if (std::string(raw->Type()) != ChannelType::kStream) {
            rsp = MakeErrorJson("source channel is not stream type: " + source_name);
            return error::BAD_REQUEST;
        }
        auto* stream_ch = dynamic_cast<IStreamChannel*>(raw);
        if (!stream_ch) {
            rsp = MakeErrorJson("source channel cast to IStreamChannel failed: " + source_name);
            return error::BAD_REQUEST;
        }
        source_channels.push_back(
            std::shared_ptr<IStreamChannel>(stream_ch, [](IStreamChannel*) {}));
    }
    if (source_channels.empty()) {
        rsp = MakeErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    const std::unordered_map<std::string, std::string> with_params =
        !stmt.operator_with_params.empty()
            ? stmt.operator_with_params[0]
            : stmt.with_params;
    if (with_params.find("sink_table") != with_params.end()) {
        rsp = MakeErrorJson("sink_table is not supported for stream tasks; use INTO <db_type>.<db_name>.<table>");
        return error::BAD_REQUEST;
    }

    SinkBinding sink_binding;
    std::string sink_error;
    const int32_t sink_rc = ResolveStreamSink(stmt, &sink_binding, &sink_error);
    if (sink_rc != error::OK) {
        rsp = MakeErrorJson(sink_error);
        return sink_rc;
    }
    auto output = sink_binding.sink_channel;
    if (!output) {
        rsp = MakeErrorJson("resolve stream sink failed: output channel is null");
        return error::INTERNAL_ERROR;
    }
    StreamSinkContext sink_ctx;
    sink_ctx.sink_channel = output.get();
    sink_ctx.sink_type = sink_binding.sink_type;
    sink_ctx.into_raw = stmt.dest;
    sink_ctx.db_type = sink_binding.db_type;
    sink_ctx.db_name = sink_binding.db_name;
    sink_ctx.table_name = sink_binding.table_name;

    const std::string task_id = NextStreamTaskId();
    std::shared_ptr<FanInStreamChannel> fanin;
    std::shared_ptr<IStreamChannel> source = source_channels[0];
    if (source_channels.size() > 1) {
        fanin = std::make_shared<FanInStreamChannel>(
            "fanin", task_id + ".fanin", source_channels);
        source = fanin;
    }

    if (!stmt.where_clause.empty()) {
        std::vector<std::string> unsupported;
        const int filter_rc = source->SetFilter(stmt.where_clause.c_str(), &unsupported);
        if (filter_rc != 0) {
            rsp = MakeErrorJson("stream source SetFilter failed");
            return error::BAD_REQUEST;
        }
        if (!unsupported.empty()) {
            std::ostringstream oss;
            oss << "WHERE pushdown not fully supported:";
            for (size_t i = 0; i < unsupported.size(); ++i) {
                oss << (i == 0 ? " " : ", ") << unsupported[i];
            }
            rsp = MakeErrorJson(oss.str());
            return error::BAD_REQUEST;
        }
    }

    auto first_holder = CreateOperator(op_ref.category, op_ref.name);
    if (!first_holder) {
        rsp = MakeErrorJson("operator create failed: " + op_ref.category + "." + op_ref.name);
        return error::NOT_FOUND;
    }
    auto first_stream_op = std::dynamic_pointer_cast<IStreamOperator>(first_holder);
    if (!first_stream_op) {
        rsp = MakeErrorJson("operator is not stream operator: " + op_ref.category + "." + op_ref.name);
        return error::BAD_REQUEST;
    }

    ParallelStrategy strategy = first_stream_op->GetParallelStrategy();
    int parallelism = std::max(1, first_stream_op->GetParallelism());
    if (strategy == ParallelStrategy::NONE) {
        parallelism = 1;
    }
    if (sink_ctx.sink_type != ChannelType::kStream) {
        if (strategy != ParallelStrategy::NONE || parallelism != 1) {
            LOG_WARN("ExecuteStreamTask: sink=%s(type=%s) forces single-writer, downgrade strategy=%d parallelism=%d -> NONE/1",
                     stmt.dest.c_str(), sink_ctx.sink_type.c_str(),
                     static_cast<int>(strategy), parallelism);
        }
        strategy = ParallelStrategy::NONE;
        parallelism = 1;
    }

    std::shared_ptr<FanOutStreamChannel> fanout;
    std::shared_ptr<SharedSpmcState> spmc_state;
    std::vector<std::shared_ptr<IStreamChannel>> input_ports;
    input_ports.reserve(static_cast<size_t>(parallelism));
    std::shared_ptr<IStreamChannel> open_target = source;

    if (strategy == ParallelStrategy::NONE) {
        input_ports.push_back(source);
    } else if (strategy == ParallelStrategy::STATELESS) {
        const size_t source_cap = source->Capacity();
        const size_t ring_size = std::max<size_t>(source_cap > 0 ? source_cap : 64,
                                                  static_cast<size_t>(parallelism) * 64);
        spmc_state = std::make_shared<SharedSpmcState>(
            "spmc", task_id + ".spmc", source, ring_size);
        for (int i = 0; i < parallelism; ++i) {
            input_ports.push_back(std::make_shared<SharedSpmcInputView>(spmc_state, static_cast<uint32_t>(i)));
        }
    } else if (strategy == ParallelStrategy::KEYED) {
        RingStreamChannelOptions partition_opts;
        const size_t source_cap = source->Capacity();
        if (source_cap > 0) {
            partition_opts.ring_size = NextPowerOfTwo(std::max<size_t>(64, source_cap));
        }
        fanout = std::make_shared<FanOutStreamChannel>(
            "fanout",
            task_id + ".fanout",
            source,
            static_cast<size_t>(parallelism),
            FanOutMode::ROUTE_BY_PARTITION_ID,
            first_stream_op->GetPartitionSpec(),
            partition_opts);
        open_target = fanout;
        for (int i = 0; i < parallelism; ++i) {
            auto part = fanout->GetPartition(static_cast<size_t>(i));
            if (!part) {
                rsp = MakeErrorJson("fanout partition create failed");
                return error::INTERNAL_ERROR;
            }
            input_ports.push_back(std::make_shared<FanOutPartitionView>(fanout, part));
        }
    } else {
        rsp = MakeErrorJson("unsupported stream parallel strategy");
        return error::BAD_REQUEST;
    }

    if (input_ports.empty()) {
        rsp = MakeErrorJson("stream input ports build failed");
        return error::INTERNAL_ERROR;
    }

    const std::string with_params_json = MakeWithParamsJson(with_params);
    const std::shared_ptr<arrow::Schema> static_schema = source->GetOutputSchema();

    auto task = std::make_shared<StreamTask>(task_id, &stream_runtime_);

    for (size_t i = 0; i < input_ports.size(); ++i) {
        std::shared_ptr<IOperator> op_holder;
        if (i == 0) {
            op_holder = first_holder;
        } else {
            op_holder = CreateOperator(op_ref.category, op_ref.name);
        }
        if (!op_holder) {
            rsp = MakeErrorJson("operator create failed for shard");
            return error::INTERNAL_ERROR;
        }

        auto stream_op = std::dynamic_pointer_cast<IStreamOperator>(op_holder);
        if (!stream_op) {
            rsp = MakeErrorJson("stream operator cast failed for shard");
            return error::INTERNAL_ERROR;
        }

        int init_rc = stream_op->Init(with_params_json.c_str(), sink_ctx);
        if (init_rc != 0) {
            const std::string err = stream_op->LastError().empty()
                ? "stream operator Init failed"
                : stream_op->LastError();
            rsp = MakeErrorJson(err);
            return error::BAD_REQUEST;
        }

        if (static_schema) {
            int schema_rc = stream_op->OnSchemaReady(static_schema);
            if (schema_rc != 0) {
                const std::string err = stream_op->LastError().empty()
                    ? "stream operator OnSchemaReady failed"
                    : stream_op->LastError();
                rsp = MakeErrorJson(err);
                return error::BAD_REQUEST;
            }
        }

        task->AddShard(std::make_shared<ShardRunner>(
            static_cast<uint32_t>(i),
            input_ports[i],
            stream_op,
            output,
            task.get(),
            static_schema != nullptr));
    }
    task->PrepareForRun(static_cast<uint32_t>(input_ports.size()), CurrentTimeMs());

    const int open_rc = open_target->Open();
    if (open_rc != 0) {
        rsp = MakeErrorJson("open stream source failed");
        return error::INTERNAL_ERROR;
    }
    if (spmc_state) {
        const int spmc_rc = spmc_state->Start();
        if (spmc_rc != 0) {
            open_target->Cancel();
            rsp = MakeErrorJson("start shared spmc forwarder failed");
            return error::INTERNAL_ERROR;
        }
    }

    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        stream_tasks_[task->Id()] = task;
    }
    for (const auto& shard : task->Shards()) {
        stream_runtime_.TrySchedule(shard);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status");
    w.String("submitted");
    w.Key("stream_task_id");
    w.String(task->Id().c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }

    const std::string sql_text = doc["sql"].GetString();
    SqlParser parser;
    SqlStatement stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        rsp = MakeErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }
    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        rsp = MakeErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }
    return ExecuteStreamTask(stmt, rsp);
}

int32_t SchedulerPlugin::HandleStreamStop(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();

    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(task_id);
        if (it == stream_tasks_.end()) {
            rsp = MakeErrorJson("stream task not found: " + task_id);
            return error::NOT_FOUND;
        }
        task = it->second;
    }

    task->RequestStop();
    task->Join();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    WriteTaskSnapshotJson(&w, task->Snapshot());
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamStatus(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();

    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(task_id);
        if (it == stream_tasks_.end()) {
            rsp = MakeErrorJson("stream task not found: " + task_id);
            return error::NOT_FOUND;
        }
        task = it->second;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    WriteTaskSnapshotJson(&w, task->Snapshot());
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamList(const std::string&, const std::string&, std::string& rsp) {
    std::vector<TaskSnapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        snapshots.reserve(stream_tasks_.size());
        for (const auto& kv : stream_tasks_) {
            if (kv.second) snapshots.push_back(kv.second->Snapshot());
        }
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("tasks");
    w.StartArray();
    for (const auto& s : snapshots) {
        WriteTaskSnapshotJson(&w, s);
    }
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleExecute ---
int32_t SchedulerPlugin::HandleExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }
    std::string sql_text = doc["sql"].GetString();

    static constexpr size_t kMaxSqlLength = 64 * 1024;
    if (sql_text.size() > kMaxSqlLength) {
        rsp = MakeErrorJson("SQL too long (max 64KB)");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        rsp = MakeErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }

    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        rsp = MakeErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    std::vector<IChannel*> input_channels;
    input_channels.reserve(stmt.sources.size());
    for (const auto& name : stmt.sources) {
        IChannel* ch = FindChannel(name);
        if (!ch) {
            rsp = MakeErrorJson("source channel not found: " + name);
            return IsDataFrameRef(name) ? error::NOT_FOUND : error::BAD_REQUEST;
        }
        input_channels.push_back(ch);
    }

    bool has_stream_source = false;
    bool has_non_stream_source = false;
    for (auto* ch : input_channels) {
        if (!ch) continue;
        if (std::string(ch->Type()) == ChannelType::kStream) {
            has_stream_source = true;
        } else {
            has_non_stream_source = true;
        }
    }
    if (has_stream_source) {
        if (has_non_stream_source) {
            rsp = MakeErrorJson("mixed stream and non-stream sources are not supported");
            return error::BAD_REQUEST;
        }
        return ExecuteStreamTask(stmt, rsp);
    }

    std::vector<std::shared_ptr<IOperator>> op_holders;
    std::vector<IOperator*> op_chain;
    std::vector<OperatorRef> parsed_ops = stmt.operators;
    if (parsed_ops.empty() && !stmt.op_category.empty() && !stmt.op_name.empty()) {
        parsed_ops.push_back({stmt.op_category, stmt.op_name});
    }
    if (!parsed_ops.empty()) {
        auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
        if (!catalog) {
            rsp = MakeErrorJson("operator catalog unavailable");
            return error::UNAVAILABLE;
        }
        for (const auto& op_ref : parsed_ops) {
            OperatorStatus status = catalog->QueryStatus(op_ref.category, op_ref.name);
            if (status == OperatorStatus::kNotFound) {
                rsp = MakeErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            if (status == OperatorStatus::kDeactivated) {
                rsp = MakeErrorJson("operator is deactivated: " + op_ref.category + "." + op_ref.name);
                return error::CONFLICT;
            }
            auto holder = FindOperator(op_ref.category, op_ref.name);
            if (!holder) {
                rsp = MakeErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            op_chain.push_back(holder.get());
            op_holders.push_back(std::move(holder));
        }
    }

    try {
        if (!op_chain.empty()) {
            for (size_t i = 0; i < op_chain.size(); ++i) {
                const auto& params = (i < stmt.operator_with_params.size())
                    ? stmt.operator_with_params[i]
                    : (i == 0 ? stmt.with_params : std::unordered_map<std::string, std::string>{});
                for (const auto& kv : params) {
                    op_chain[i]->Configure(kv.first.c_str(), kv.second.c_str());
                }
            }
        }

        std::shared_ptr<DataFrameChannel> temp_sink;
        std::shared_ptr<IDataFrameChannel> named_df_sink;
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            if (!IsQualifiedDestination(stmt.dest)) {
                rsp = MakeErrorJson("invalid INTO destination: " + stmt.dest +
                                    ", expected dataframe.<name> or <type>.<name>[.<table>]");
                return error::BAD_REQUEST;
            }
            if (IsDataFrameRef(stmt.dest)) {
                if (!ch_registry) {
                    rsp = MakeErrorJson("channel registry unavailable");
                    return error::INTERNAL_ERROR;
                }
                std::string df_name = DataFrameNamePart(stmt.dest);
                auto ch = std::make_shared<DataFrameChannel>("dataframe", df_name);
                ch->Open();
                named_df_sink = ch;
                sink = ch.get();
            } else {
                sink = FindChannel(stmt.dest);
                if (!sink) {
                    rsp = MakeErrorJson("destination channel not found: " + stmt.dest);
                    return error::NOT_FOUND;
                }
            }
        } else {
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        int rc = 0;
        int64_t affected_rows = 0;
        std::string exec_error;
        std::string sink_type(sink->Type());

        if (input_channels.size() > 1 && op_chain.empty()) {
            rsp = MakeErrorJson("multi-source FROM requires USING operator");
            return error::BAD_REQUEST;
        }
        if (input_channels.size() > 1) {
            for (const auto& source_name : stmt.sources) {
                if (!IsDataFrameRef(source_name)) {
                    rsp = MakeErrorJson("multi-source FROM only supports dataframe.* in Sprint 10");
                    return error::BAD_REQUEST;
                }
            }
            if (!stmt.where_clause.empty()) {
                rsp = MakeErrorJson("multi-source FROM does not support WHERE in Sprint 10");
                return error::BAD_REQUEST;
            }
        }

        if (op_chain.empty()) {
            if (input_channels.size() != 1) {
                rsp = MakeErrorJson("invalid source count");
                return error::BAD_REQUEST;
            }
            IChannel* source = input_channels[0];
            std::string source_type(source->Type());
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt, &affected_rows, &exec_error);
        } else {
            rc = ExecuteWithOperatorChain(Span<IChannel*>(input_channels), sink, op_chain, sink_type, stmt,
                                          &affected_rows, &exec_error);
        }

        if (rc != 0) {
            std::string err = exec_error;
            if (err.empty() && !op_chain.empty()) err = op_chain.back()->LastError();
            if (err.empty()) err = "execution failed";
            std::string stage = ExtractStageFromExecutionError(err);
            if (stage.empty()) stage = "execute";
            rsp = MakeExecutionErrorJson(err, "OP_EXEC_FAIL", stage);
            return error::INTERNAL_ERROR;
        }

        // INTO dataframe.<name>：覆盖语义（已存在则先注销，再注册新结果）
        if (!stmt.dest.empty() && IsDataFrameRef(stmt.dest) && named_df_sink) {
            std::string df_name = DataFrameNamePart(stmt.dest);
            if (ch_registry->Get(df_name.c_str())) {
                (void)ch_registry->Unregister(df_name.c_str());
            }
            if (ch_registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(named_df_sink)) != 0) {
                rsp = MakeErrorJson("failed to register dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto registered = ch_registry->Get(df_name.c_str());
            if (!registered) {
                rsp = MakeErrorJson("failed to fetch registered dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto* registered_df = dynamic_cast<IDataFrameChannel*>(registered.get());
            if (!registered_df) {
                rsp = MakeErrorJson("registered channel is not dataframe: " + df_name);
                return error::INTERNAL_ERROR;
            }
            sink = registered_df;
            sink_type = sink->Type();
        }

        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        int64_t row_count = 0;
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
            row_count = result.RowCount();
        } else if (sink_type == ChannelType::kDatabase) {
            row_count = affected_rows;
        }

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("status"); w.String("completed");
        w.Key("rows"); w.Int64(row_count);
        w.Key("result_row_count"); w.Int64(row_count);
        w.Key("result_target"); w.String(stmt.dest.c_str());
        w.Key("data"); w.RawValue(result_json.c_str(), result_json.size(), rapidjson::kArrayType);
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;

    } catch (const std::exception& e) {
        std::string err = std::string("internal error: ") + e.what();
        LOG_ERROR("SchedulerPlugin::HandleExecute: exception: %s", err.c_str());
        rsp = MakeErrorJson(err);
        return error::INTERNAL_ERROR;
    } catch (...) {
        LOG_ERROR("SchedulerPlugin::HandleExecute: unknown exception");
        rsp = MakeErrorJson("internal error: unknown exception");
        return error::INTERNAL_ERROR;
    }
}

// --- HandleGetChannels ---
int32_t SchedulerPlugin::HandleGetChannels(const std::string&, const std::string&, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();

    // 内部通道表
    for (auto& [key, ch_ptr] : channels_) {
        auto* ch = ch_ptr.get();
        w.StartObject();
        w.Key("category"); w.String(ch->Category());
        w.Key("name"); w.String(ch->Name());
        w.Key("type"); w.String(ch->Type());
        w.Key("schema"); w.String(ch->Schema());
        w.EndObject();
    }

    // 具名 DataFrame 通道（CatalogPlugin 注册中心）
    if (ch_registry) {
        ch_registry->List([&w](const char* name, std::shared_ptr<IChannel> ch) {
            if (!name || !ch) return;
            w.StartObject();
            w.Key("category"); w.String(ch->Category());
            w.Key("name"); w.String(name);
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
        });
    }

    // 静态注册的通道（通过 IQuerier）
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&w](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            w.StartObject();
            w.Key("category"); w.String(ch->Category());
            w.Key("name"); w.String(ch->Name());
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
            return 0;
        });

        // 数据库通道（通过 IDatabaseFactory）
        auto* factory = static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY));
        if (factory) {
            factory->List([&w](const char* type, const char* name, const char* config_json) {
                w.StartObject();
                w.Key("category"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(ChannelType::kDatabase);
                // 从 config_json 提取 database 字段作为 schema 展示
                std::string db_label;
                if (config_json) {
                    rapidjson::Document cfg;
                    cfg.Parse(config_json);
                    if (!cfg.HasParseError() && cfg.IsObject()) {
                        if (cfg.HasMember("database") && cfg["database"].IsString()) {
                            db_label = cfg["database"].GetString();
                        } else if (cfg.HasMember("path") && cfg["path"].IsString()) {
                            db_label = cfg["path"].GetString();
                        }
                    }
                }
                w.Key("schema"); w.String(db_label.c_str());
                w.EndObject();
            });
        }

        // 流式通道（通过 IStreamFactory）
        auto* stream_factory = static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY));
        if (stream_factory) {
            stream_factory->List([&w](const char* type, const char* name, IStreamChannel* stream_ch) {
                if (!stream_ch || !type || !name) return;
                w.StartObject();
                w.Key("category"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(stream_ch->Type());
                w.Key("schema"); w.String(stream_ch->Schema());
                w.EndObject();
            });
        }
    }

    w.EndArray();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleQueryStreamChannels ---
int32_t SchedulerPlugin::HandleQueryStreamChannels(const std::string&,
                                                   const std::string&,
                                                   std::string& rsp) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("channels");
    w.StartArray();

    auto* stream_factory = querier_ ? static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY)) : nullptr;
    if (stream_factory) {
        stream_factory->List([&w](const char* type, const char* name, IStreamChannel* stream_ch) {
            if (!type || !name || !stream_ch) return;
            const char* status = "running";
            if (stream_ch->IsFinished() && stream_ch->IsEmpty()) {
                status = "stopped";
            } else if (stream_ch->IsFinished()) {
                status = "draining";
            }

            w.StartObject();
            w.Key("type");
            w.String(type);
            w.Key("name");
            w.String(name);
            w.Key("status");
            w.String(status);
            w.EndObject();
        });
    }

    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandlePreviewDataframe ---
// POST /channels/dataframe/preview — Body: {"category":"...","name":"..."} 或 {"name":"..."}
int32_t SchedulerPlugin::HandlePreviewDataframe(const std::string&, const std::string& req, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = R"({"error":"missing 'name'"})";
        return error::BAD_REQUEST;
    }
    std::string category = "dataframe";
    if (doc.HasMember("category") && doc["category"].IsString()) {
        category = doc["category"].GetString();
    }
    std::string name = doc["name"].GetString();
    int page = 1;
    int page_size = 20;
    if (doc.HasMember("page") && doc["page"].IsInt()) page = doc["page"].GetInt();
    if (doc.HasMember("page_size") && doc["page_size"].IsInt()) page_size = doc["page_size"].GetInt();
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 20;
    if (page_size > 100) page_size = 100;
    std::string key     = category + "." + name;

    // 先在内部通道表查找
    IChannel* raw_ch = nullptr;
    auto it = channels_.find(key);
    if (it != channels_.end()) {
        raw_ch = it->second.get();
    }

    // 再去 IQuerier 静态注册通道查找
    if (!raw_ch && querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            if (IEquals(ch->Category(), category) && std::string(ch->Name()) == name) {
                raw_ch = ch;
                return 1;  // 找到，停止遍历
            }
            return 0;
        });
    }

    if (!raw_ch) {
        if (IEquals(category, "dataframe") && ch_registry) {
            auto named = ch_registry->Get(name.c_str());
            auto* named_df = dynamic_cast<IDataFrameChannel*>(named.get());
            if (named_df) raw_ch = named_df;
        }
    }

    if (!raw_ch) {
        rsp = "{\"error\":\"channel not found: " + key + "\"}";
        return error::NOT_FOUND;
    }

    auto* df_ch = dynamic_cast<IDataFrameChannel*>(raw_ch);
    if (!df_ch) {
        rsp = R"({"error":"not a dataframe channel"})";
        return error::BAD_REQUEST;
    }

    DataFrame data;
    if (df_ch->Read(&data) != 0 || data.RowCount() == 0) {
        rsp = R"({"columns":[],"types":[],"data":[],"rows":0})";
        return error::OK;
    }
    const auto schema = data.GetSchema();
    const int rows = data.RowCount();
    const int start = (page - 1) * page_size;
    const int end = std::min(rows, start + page_size);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("columns");
    w.StartArray();
    for (const auto& f : schema) w.String(f.name.c_str());
    w.EndArray();
    w.Key("types");
    w.StartArray();
    for (const auto& f : schema) w.String(DataTypeName(f.type));
    w.EndArray();
    w.Key("data");
    w.StartArray();
    for (int r = start; r < end; ++r) {
        const auto row = data.GetRow(r);
        w.StartArray();
        for (const auto& v : row) {
            std::visit(
                [&](auto&& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, int32_t>) w.Int(val);
                    else if constexpr (std::is_same_v<T, int64_t>) w.Int64(val);
                    else if constexpr (std::is_same_v<T, uint32_t>) w.Uint(val);
                    else if constexpr (std::is_same_v<T, uint64_t>) w.Uint64(val);
                    else if constexpr (std::is_same_v<T, float>) w.Double(val);
                    else if constexpr (std::is_same_v<T, double>) w.Double(val);
                    else if constexpr (std::is_same_v<T, std::string>) w.String(val.c_str());
                    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                        w.String(reinterpret_cast<const char*>(val.data()), val.size());
                    else if constexpr (std::is_same_v<T, bool>) w.Bool(val);
                },
                v);
        }
        w.EndArray();
    }
    w.EndArray();
    w.Key("rows");
    w.Int(rows);
    w.Key("page");
    w.Int(page);
    w.Key("page_size");
    w.Int(page_size);
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}
// --- HandleRefreshOperators ---
int32_t SchedulerPlugin::HandleRefreshOperators(const std::string&, const std::string&, std::string& rsp) {
    if (!querier_) {
        rsp = R"({"error":"querier not initialized"})";
        return error::INTERNAL_ERROR;
    }
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        int rc = bridge->Refresh();
        if (rc == 0) {
            rsp = R"({"status":"refreshed"})";
            return error::OK;
        } else {
            rsp = R"({"error":"refresh failed"})";
            return error::INTERNAL_ERROR;
        }
    } else {
        rsp = R"({"error":"bridge not available"})";
        return error::NOT_FOUND;
    }
}

// ==================== 数据库通道动态管理端点（Epic 6）====================
// 已移交 DatabasePlugin 处理，此处删除

}  // namespace scheduler
}  // namespace flowsql
