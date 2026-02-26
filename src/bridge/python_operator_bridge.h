#ifndef _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_
#define _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_

#include <httplib.h>

#include <memory>
#include <string>

#include "framework/interfaces/ioperator.h"

namespace flowsql {
namespace bridge {

// Python 算子的元数据
struct OperatorMeta {
    std::string catelog;
    std::string name;
    std::string description;
    OperatorPosition position = OperatorPosition::DATA;
};

// PythonOperatorBridge — 实现 IOperator，将 Work() 转发给 Python Worker
// 每个实例持有独立的 httplib::Client（线程安全，无锁）
class PythonOperatorBridge : public IOperator {
 public:
    PythonOperatorBridge(const OperatorMeta& meta, const std::string& host, int port);
    ~PythonOperatorBridge() override = default;

    // IPlugin
    int Option(const char*) override { return 0; }
    int Load() override { return 0; }
    int Unload() override { return 0; }

    // IOperator 元数据
    std::string Catelog() override { return meta_.catelog; }
    std::string Name() override { return meta_.name; }
    std::string Description() override { return meta_.description; }
    OperatorPosition Position() override { return meta_.position; }

    // 核心：将 Work 转发给 Python Worker
    int Work(IDataFrame* in, IDataFrame* out) override;

    // 配置转发
    int Configure(const char* key, const char* value) override;

 private:
    OperatorMeta meta_;
    std::unique_ptr<httplib::Client> client_;
    std::string host_;
    int port_;
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_
