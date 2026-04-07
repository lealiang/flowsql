#include "stream_execution_plan.h"

namespace flowsql {
namespace scheduler {

LeaseToken::LeaseToken(std::function<void()> releaser)
    : releaser_(std::move(releaser)), active_(true) {}

LeaseToken::LeaseToken(LeaseToken&& other) noexcept
    : releaser_(std::move(other.releaser_)), active_(other.active_) {
    other.active_ = false;
}

LeaseToken& LeaseToken::operator=(LeaseToken&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    releaser_ = std::move(other.releaser_);
    active_ = other.active_;
    other.active_ = false;
    return *this;
}

LeaseToken::~LeaseToken() {
    Reset();
}

void LeaseToken::Commit() {
    active_ = false;
}

void LeaseToken::Reset() {
    if (!active_) return;
    active_ = false;
    if (releaser_) releaser_();
}

}  // namespace scheduler
}  // namespace flowsql
