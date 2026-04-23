/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "replay_runner.h"

#include <common/error_code.h>

namespace flowsql {
namespace baseline {

namespace {

std::string CopyKey(const BaselineStringRef& key) {
    if (!key.data || key.size == 0) return "";
    return std::string(key.data, key.size);
}

template <typename TSeries>
void FillWindowSummary(int64_t request_bucket_start,
                       int64_t request_bucket_end,
                       TSeries* out) {
    if (!out) return;
    out->window.request_bucket_start = request_bucket_start;
    out->window.request_bucket_end = request_bucket_end;
    out->window.observation_count = out->points.size();
    out->window.has_data = !out->points.empty();
    if (!out->points.empty()) {
        out->window.first_bucket_id = out->points.front().bucket_id;
        out->window.last_bucket_id = out->points.back().bucket_id;
    }
}

}  // namespace

ValueReplayRunner::ValueReplayRunner(std::string expected_key)
    : expected_key_(std::move(expected_key)) {
    series_.key = expected_key_;
}

int ValueReplayRunner::Push(const ValueObservation& obs) {
    if (obs.bucket_id < 0 || obs.value < 0) return error::BAD_REQUEST;

    const std::string key = CopyKey(obs.key);
    if (key.empty() || key != expected_key_) return error::BAD_REQUEST;
    if (!series_.points.empty() && obs.bucket_id < series_.points.back().bucket_id) {
        return error::BAD_REQUEST;
    }

    series_.points.push_back(ValueReplayPoint{
        obs.bucket_id,
        obs.value,
        obs.sample_count});
    return error::OK;
}

void ValueReplayRunner::Finalize(int64_t request_bucket_start,
                                 int64_t request_bucket_end,
                                 ValueReplaySeries* out) const {
    if (!out) return;
    *out = series_;
    FillWindowSummary(request_bucket_start, request_bucket_end, out);
}

RatioReplayRunner::RatioReplayRunner(std::string expected_key)
    : expected_key_(std::move(expected_key)) {
    series_.key = expected_key_;
}

int RatioReplayRunner::Push(const RatioObservation& obs) {
    if (obs.bucket_id < 0 || obs.numerator < 0 || obs.denominator <= 0) {
        return error::BAD_REQUEST;
    }

    const std::string key = CopyKey(obs.key);
    if (key.empty() || key != expected_key_) return error::BAD_REQUEST;
    if (!series_.points.empty() && obs.bucket_id < series_.points.back().bucket_id) {
        return error::BAD_REQUEST;
    }

    series_.points.push_back(RatioReplayPoint{
        obs.bucket_id,
        obs.numerator,
        obs.denominator});
    return error::OK;
}

void RatioReplayRunner::Finalize(int64_t request_bucket_start,
                                 int64_t request_bucket_end,
                                 RatioReplaySeries* out) const {
    if (!out) return;
    *out = series_;
    FillWindowSummary(request_bucket_start, request_bucket_end, out);
}

}  // namespace baseline
}  // namespace flowsql
