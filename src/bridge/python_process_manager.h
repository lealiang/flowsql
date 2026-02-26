#ifndef _FLOWSQL_BRIDGE_PYTHON_PROCESS_MANAGER_H_
#define _FLOWSQL_BRIDGE_PYTHON_PROCESS_MANAGER_H_

#include <string>

namespace flowsql {
namespace bridge {

class PythonProcessManager {
 public:
    PythonProcessManager() = default;
    ~PythonProcessManager();

    // 启动 Python Worker 进程
    int Start(const std::string& host, int port, const std::string& operators_dir,
              const std::string& python_path = "python3");

    // 等待 Worker 就绪（轮询 /health）
    int WaitReady(int timeout_ms = 5000);

    // 停止 Worker 进程（SIGTERM → 等待 → SIGKILL）
    void Stop();

    // 检查进程是否存活
    bool IsAlive() const;

    int Port() const { return port_; }
    const std::string& Host() const { return host_; }

 private:
    pid_t pid_ = -1;
    std::string host_;
    int port_ = 0;
    int max_restart_ = 3;
    int restart_count_ = 0;
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_PYTHON_PROCESS_MANAGER_H_
