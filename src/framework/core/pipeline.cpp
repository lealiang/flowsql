#include "pipeline.h"

namespace flowsql {

void Pipeline::Run() {
    state_ = PipelineState::RUNNING;
    running_ = true;
    DataFrame in_frame, out_frame;

    while (running_) {
        in_frame.Clear();
        out_frame.Clear();

        // 从 source channels 批量读取，组装 DataFrame
        for (auto* ch : sources_) {
            for (int i = 0; i < batch_size_; ++i) {
                IDataEntity* entity = ch->Get();
                if (!entity) break;
                in_frame.AppendEntity(entity);
            }
        }

        if (in_frame.RowCount() == 0) break;

        // 调用算子
        if (operator_->Work(&in_frame, &out_frame) != 0) {
            state_ = PipelineState::FAILED;
            return;
        }

        // 将结果写回 sink channel
        if (sink_) {
            for (int32_t i = 0; i < out_frame.RowCount(); ++i) {
                auto entity = out_frame.GetEntity(i);
                sink_->Put(entity.get());
            }
        }
    }

    if (state_ == PipelineState::RUNNING) {
        state_ = PipelineState::STOPPED;
    }
}

void Pipeline::Stop() {
    running_ = false;
}

// --- PipelineBuilder ---

PipelineBuilder& PipelineBuilder::AddSource(IChannel* channel) {
    sources_.push_back(channel);
    return *this;
}

PipelineBuilder& PipelineBuilder::SetOperator(IOperator* op) {
    operator_ = op;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetSink(IChannel* channel) {
    sink_ = channel;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetBatchSize(int32_t size) {
    batch_size_ = size;
    return *this;
}

std::unique_ptr<Pipeline> PipelineBuilder::Build() {
    auto pipeline = std::make_unique<Pipeline>();
    pipeline->sources_ = std::move(sources_);
    pipeline->operator_ = operator_;
    pipeline->sink_ = sink_;
    pipeline->batch_size_ = batch_size_;
    return pipeline;
}

}  // namespace flowsql
