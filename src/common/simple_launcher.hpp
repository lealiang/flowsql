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
 * Author       : fei.wang@foxmail.com
 * Date         : 2020-10-07 17:54:31
 * LastEditors  : Pericles
 * LastEditTime : 2020-10-26 22:27:36
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
