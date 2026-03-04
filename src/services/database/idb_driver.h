#ifndef _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_

#include <common/typedef.h>
#include <framework/interfaces/idatabase_channel.h>

#include <string>
#include <unordered_map>

namespace flowsql {
namespace database {

// IDbDriver — 数据库驱动抽象
// 封装具体数据库客户端库的连接管理和数据操作
interface IDbDriver {
    virtual ~IDbDriver() = default;

    // 连接管理
    virtual int Connect(const std::unordered_map<std::string, std::string>& params) = 0;
    virtual int Disconnect() = 0;
    virtual bool IsConnected() = 0;

    // 数据操作（创建 Reader/Writer，生命周期由调用者管理）
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;

    // 元数据
    virtual const char* DriverName() = 0;
    virtual const char* LastError() = 0;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_
