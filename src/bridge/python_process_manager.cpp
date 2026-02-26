#include "python_process_manager.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern char** environ;

namespace flowsql {
namespace bridge {

PythonProcessManager::~PythonProcessManager() {
    Stop();
}

int PythonProcessManager::Start(const std::string& host, int port, const std::string& operators_dir,
                                const std::string& python_path) {
    if (IsAlive()) {
        printf("PythonProcessManager: Worker already running (pid=%d)\n", pid_);
        return 0;
    }

    host_ = host;
    port_ = port;

    // 构建命令行参数: python3 -m flowsql.worker --host HOST --port PORT --operators-dir DIR
    std::string port_str = std::to_string(port);

    std::vector<const char*> argv;
    argv.push_back(python_path.c_str());
    argv.push_back("-m");
    argv.push_back("flowsql.worker");
    argv.push_back("--host");
    argv.push_back(host.c_str());
    argv.push_back("--port");
    argv.push_back(port_str.c_str());
    argv.push_back("--operators-dir");
    argv.push_back(operators_dir.c_str());
    argv.push_back(nullptr);

    // 使用 posix_spawn 替代 fork+exec（多线程安全）
    pid_t child_pid;
    int ret = posix_spawn(&child_pid, python_path.c_str(), nullptr, nullptr,
                          const_cast<char* const*>(argv.data()), environ);
    if (ret != 0) {
        // posix_spawn 可能找不到绝对路径，尝试 posix_spawnp（PATH 搜索）
        ret = posix_spawnp(&child_pid, python_path.c_str(), nullptr, nullptr,
                           const_cast<char* const*>(argv.data()), environ);
        if (ret != 0) {
            printf("PythonProcessManager: posix_spawnp failed: %s\n", strerror(ret));
            return -1;
        }
    }

    pid_ = child_pid;
    printf("PythonProcessManager: Worker started (pid=%d, port=%d)\n", pid_, port_);
    return 0;
}

int PythonProcessManager::WaitReady(int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    int interval_ms = 100;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= timeout_ms) {
            printf("PythonProcessManager: WaitReady timeout (%dms)\n", timeout_ms);
            return -1;
        }

        if (!IsAlive()) {
            printf("PythonProcessManager: Worker process died during startup\n");
            return -1;
        }

        // 尝试连接 /health
        // 这里用简单的 TCP 连接检测，避免引入 httplib 依赖
        // BridgePlugin 会用 httplib 做完整的 health check
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port_);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            // 设置非阻塞连接超时
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 50000;  // 50ms
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(sock);
                printf("PythonProcessManager: Worker ready (port=%d)\n", port_);
                return 0;
            }
            close(sock);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

void PythonProcessManager::Stop() {
    if (pid_ <= 0) return;

    if (IsAlive()) {
        // SIGTERM 优雅关闭
        kill(pid_, SIGTERM);

        // 等待 2 秒
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int status;
            pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                printf("PythonProcessManager: Worker stopped gracefully (pid=%d)\n", pid_);
                pid_ = -1;
                return;
            }
        }

        // SIGKILL 强制终止
        kill(pid_, SIGKILL);
        int status;
        waitpid(pid_, &status, 0);
        printf("PythonProcessManager: Worker killed (pid=%d)\n", pid_);
    } else {
        // 回收僵尸进程
        int status;
        waitpid(pid_, &status, WNOHANG);
    }

    pid_ = -1;
}

bool PythonProcessManager::IsAlive() const {
    if (pid_ <= 0) return false;
    return kill(pid_, 0) == 0;
}

}  // namespace bridge
}  // namespace flowsql
