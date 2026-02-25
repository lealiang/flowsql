/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-10-07 17:54:31
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef SRC_0_2_FAST_SIMPLE_LAUNCHER_HPP_
#define SRC_0_2_FAST_SIMPLE_LAUNCHER_HPP_

#include <stdint.h>
#include <iostream>
#include <string>

namespace flowsql {
class Launcher {
 public:
    template <typename Proc>
    int Launch(int argc, char* argv[], const Proc&& proc) {
        return proc();
    }

    template <typename Proc>
    void Console(const std::string& prompt, const Proc&& proc) {
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
};
}  // namespace flowsql

#endif  // SRC_0_2_FAST_SIMPLE_LAUNCHER_HPP_
