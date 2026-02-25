#include "memory_channel.h"

namespace flowsql {

int MemoryChannel::Open() {
    opened_ = true;
    return 0;
}

int MemoryChannel::Close() {
    opened_ = false;
    while (!queue_.empty()) queue_.pop();
    return 0;
}

int MemoryChannel::Put(IDataEntity* entity) {
    if (!opened_ || !entity) return -1;
    queue_.push(entity->Clone());
    return 0;
}

IDataEntity* MemoryChannel::Get() {
    if (!opened_ || queue_.empty()) return nullptr;
    last_get_ = queue_.front();
    queue_.pop();
    return last_get_.get();
}

}  // namespace flowsql
