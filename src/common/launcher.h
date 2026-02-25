/*
 * Copyright (C) 2020-06 - FAST
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Author       : lealiang@qq.com
 * Date         : 2020-08-31 22:21:33
 * LastEditors  : kun.dong
 * LastEditTime : 2020-10-22 13:15:45
 */
#ifndef _FAST_BASE_COMMON_LAUNCHER_H_
#define _FAST_BASE_COMMON_LAUNCHER_H_

#include <getopt.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include "../common/defer.h"
#include "../common/toolkit.h"
#include "typedef.h"

DEFINE_string(proc_type, "", "proc_type");
DEFINE_int32(proc_uuid, 0, "proc_uuid");
DEFINE_bool(singleton, false, "singleton");

namespace flowsql {
class Launcher {
 public:
    template <typename Proc>
    int32_t run(int argc, char* argv[], Proc worker) {
        // 参数解析
        google::ParseCommandLineFlags(&argc, &argv, true);
        defer(google::ShutDownCommandLineFlags());

        proc_type_ = FLAGS_proc_type;
        proc_uuid_ = FLAGS_proc_uuid;
        singleton_ = FLAGS_singleton;

        // set_process_name(argc, argv);

        log_init();
        FLAGS_log_dir = logpath_;
        google::InitGoogleLogging(proc_name_.c_str());
        defer(google::ShutdownGoogleLogging());

        // run with singleton mode.
        if (singleton_) {
            if (0 != launch_singleton()) {
                LOG(ERROR) << proc_name_ << " already running!";
            }
        }

        // 执行工作函数
        return worker();
    }

    template <typename Proc>
    int launch(int argc, char* argv[], const Proc&& proc) {
        // 参数解析
        google::ParseCommandLineFlags(&argc, &argv, true);
        defer(google::ShutDownCommandLineFlags());

        // run with singleton mode.
        if (/*FLAGS_singleton*/ 0) {
            // 检查当前文件名的lock文件是否存在即可
            std::string finger_ = "";
            if (verify_singleton_process(finger_.c_str())) {
                LOG(ERROR) << "process \'" << finger_ << "\' already running!";
                return -1;
            }
        }

        // glog support.
        if (FLAGS_log_dir.empty()) {
            FLAGS_log_dir = get_absolute_process_path();
        }
        google::InitGoogleLogging("");
        defer(google::ShutdownGoogleLogging());

        return proc();
    }

    template <typename Proc>
    void console(const std::string& prompt, const Proc&& proc) {
        // 命令行接口
        do {
            std::cout << prompt << std::endl << '>';
            std::string cmd;
            std::cin >> cmd;
            proc(cmd);
            if (cmd == "exit") {
                LOG(INFO) << " exit.";
                break;
            }
        } while (true);
    }

    template <typename Proc>
    void show_input(const Proc&& proc) {
        // 命令行接口
        do {
            std::cout << '>';
            std::string cmd;
            std::cin >> cmd;
            proc(cmd);
            if (cmd == "exit") {
                LOG(INFO) << " exit.";
                break;
            }
        } while (true);
    }

    inline std::string type() { return proc_type_; }
    inline int32_t uuid() { return proc_uuid_; }
    inline std::string name() { return proc_name_; }

 protected:
    inline void set_process_name(int argc, char* argv[]) {
        // 设置进程名
        proc_name_ = proc_type_ + std::to_string(proc_uuid_);
        set_proc_title(argc, argv, proc_name_.c_str());
    }

    int32_t launch_singleton() {
        // 单件运行
        proc_finger_ = calc_unique_process_fingerprint(get_absolute_process_path(), "%08x-%s-", proc_uuid_,
                                                       proc_type_.c_str());
        return verify_singleton_process(proc_finger_.c_str());
    }

    void log_init() {
        // 初始化日志路径
        std::string logpath = get_absolute_process_path();
        logpath += "/log";
        mkdir(logpath.c_str());
        logpath_ = logpath;
    }

 protected:
    std::string proc_type_ = "test";
    int32_t proc_uuid_ = 0;
    std::string proc_name_ = "test0";
    std::string proc_finger_;
    bool singleton_ = false;
    std::string logpath_ = "/tmp";
};
}  // namespace flowsql
#endif  // _FAST_BASE_COMMON_LAUNCHER_H_
