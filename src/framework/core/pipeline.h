#ifndef _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_
#define _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_

#include <memory>
#include <string>
#include <vector>

#include "dataframe.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ioperator.h"

namespace flowsql {

enum class PipelineState : int32_t {
    IDLE = 0,
    RUNNING,
    STOPPED,
    FAILED
};

class Pipeline {
 public:
    Pipeline() = default;
    ~Pipeline() = default;

    void Run();
    void Stop();
    PipelineState State() const { return state_; }

 private:
    friend class PipelineBuilder;

    std::vector<IChannel*> sources_;
    IOperator* operator_ = nullptr;
    IChannel* sink_ = nullptr;
    int32_t batch_size_ = 1000;
    bool running_ = false;
    PipelineState state_ = PipelineState::IDLE;
};

class PipelineBuilder {
 public:
    PipelineBuilder& AddSource(IChannel* channel);
    PipelineBuilder& SetOperator(IOperator* op);
    PipelineBuilder& SetSink(IChannel* channel);
    PipelineBuilder& SetBatchSize(int32_t size);
    std::unique_ptr<Pipeline> Build();

 private:
    std::vector<IChannel*> sources_;
    IOperator* operator_ = nullptr;
    IChannel* sink_ = nullptr;
    int32_t batch_size_ = 1000;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_
