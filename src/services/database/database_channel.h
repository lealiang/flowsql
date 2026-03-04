#ifndef _FLOWSQL_SERVICES_DATABASE_DATABASE_CHANNEL_H_
#define _FLOWSQL_SERVICES_DATABASE_DATABASE_CHANNEL_H_

#include <framework/interfaces/idatabase_channel.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "idb_driver.h"

namespace flowsql {
namespace database {

// DatabaseChannel — 通用数据库通道实现
// 持有 IDbDriver，委托所有数据库操作给驱动
class DatabaseChannel : public IDatabaseChannel {
 public:
    DatabaseChannel(const std::string& type, const std::string& name,
                    std::unique_ptr<IDbDriver> driver,
                    const std::unordered_map<std::string, std::string>& params);
    ~DatabaseChannel() override;

    // IChannel
    const char* Catelog() override { return type_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return "database"; }
    const char* Schema() override { return "[]"; }  // TODO: 查询数据库元数据

    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }

    // IDatabaseChannel（委托给 driver_）
    int CreateReader(const char* query, IBatchReader** reader) override;
    int CreateWriter(const char* table, IBatchWriter** writer) override;
    bool IsConnected() override;

 private:
    std::string type_;
    std::string name_;
    std::unique_ptr<IDbDriver> driver_;
    std::unordered_map<std::string, std::string> params_;
    bool opened_ = false;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DATABASE_CHANNEL_H_
