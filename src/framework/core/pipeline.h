#ifndef _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_
#define _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_

#include <atomic>
#include <memory>

#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ioperator.h"

namespace flowsql {

enum class PipelineState : int32_t {
    IDLE = 0,
    RUNNING,
    STOPPED,
    FAILED
};

// Pipeline — 纯连接器，只负责将 source 和 sink 通道交给算子
class Pipeline {
 public:
    Pipeline() = default;
    ~Pipeline() = default;

    void Run();
    void Stop();
    PipelineState State() const { return state_.load(); }
    const std::string& ErrorMessage() const { return error_message_; }

 private:
    friend class PipelineBuilder;

    IChannel* source_ = nullptr;
    IOperator* operator_ = nullptr;
    IChannel* sink_ = nullptr;
    std::atomic<PipelineState> state_{PipelineState::IDLE};
    std::string error_message_;
};

class PipelineBuilder {
 public:
    PipelineBuilder& SetSource(IChannel* channel);
    PipelineBuilder& SetOperator(IOperator* op);
    PipelineBuilder& SetSink(IChannel* channel);
    std::unique_ptr<Pipeline> Build();

 private:
    IChannel* source_ = nullptr;
    IOperator* operator_ = nullptr;
    IChannel* sink_ = nullptr;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_
