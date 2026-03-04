#include "database_channel.h"

#include <cstdio>

namespace flowsql {
namespace database {

DatabaseChannel::DatabaseChannel(const std::string& type, const std::string& name,
                                 std::unique_ptr<IDbDriver> driver,
                                 const std::unordered_map<std::string, std::string>& params)
    : type_(type), name_(name), driver_(std::move(driver)), params_(params) {}

DatabaseChannel::~DatabaseChannel() {
    if (opened_) Close();
}

int DatabaseChannel::Open() {
    if (opened_) return 0;
    if (!driver_) return -1;

    int ret = driver_->Connect(params_);
    if (ret == 0) opened_ = true;
    return ret;
}

int DatabaseChannel::Close() {
    if (!opened_) return 0;
    if (driver_) driver_->Disconnect();
    opened_ = false;
    return 0;
}

int DatabaseChannel::CreateReader(const char* query, IBatchReader** reader) {
    if (!opened_ || !driver_) return -1;
    return driver_->CreateReader(query, reader);
}

int DatabaseChannel::CreateWriter(const char* table, IBatchWriter** writer) {
    if (!opened_ || !driver_) return -1;
    return driver_->CreateWriter(table, writer);
}

bool DatabaseChannel::IsConnected() {
    if (!driver_) return false;
    return driver_->IsConnected();
}

}  // namespace database
}  // namespace flowsql
