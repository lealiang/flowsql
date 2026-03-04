#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_

#include <sqlite3.h>

#include <string>
#include <unordered_map>

#include "../idb_driver.h"

namespace flowsql {
namespace database {

// SqliteDriver — SQLite 数据库驱动
// 使用 FULLMUTEX 模式保证多线程安全，WAL 模式提升并发读写性能
class SqliteDriver : public IDbDriver {
 public:
    SqliteDriver() = default;
    ~SqliteDriver() override;

    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override;
    bool IsConnected() override { return db_ != nullptr; }

    int CreateReader(const char* query, IBatchReader** reader) override;
    int CreateWriter(const char* table, IBatchWriter** writer) override;

    const char* DriverName() override { return "sqlite"; }
    const char* LastError() override { return last_error_.c_str(); }

 private:
    sqlite3* db_ = nullptr;
    std::string db_path_;
    std::string last_error_;
    bool readonly_ = false;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_
