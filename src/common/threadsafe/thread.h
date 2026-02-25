/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-07-01 11:24:50
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FAST_COMMON_TS_THREAD_H_
#define _FAST_COMMON_TS_THREAD_H_

#include <assert.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace flowsql {
class thread : public std::thread {
 public:
    // thread() noexcept = default;
    thread() : cpuno_(-1) {
#ifndef __APPLE__
        CPU_ZERO(&bitmask_);
#endif
    }
    thread(thread &) = delete;
    thread(const thread &) = delete;
    thread(thread &&__t) noexcept { this->swap(std::move(__t)); }

    template <typename _Callable, typename... _Args>
    explicit thread(_Callable &&__f, _Args &&... __args) : std::thread(__f, __args...), cpuno_(-1) {
#ifndef __APPLE__
        CPU_ZERO(&bitmask_);
#endif
    }

    thread &swap(thread &&__t) {
#ifndef __APPLE__
        name_ = __t.name_;
        cpuno_ = __t.cpuno_;
        memcpy(&bitmask_, &__t.bitmask_, sizeof(bitmask_));
        std::thread::swap(__t);
        __t.name_.clear();
        CPU_ZERO(&__t.bitmask_);
        __t.cpuno_ = -1;
#endif
        return *this;
    }

    thread &operator()(const char *name) {
        set_thread_name(name);
        return *this;
    }

    thread &operator()(int32_t cpuno) {
        set_affinity(cpuno);
        return *this;
    }

    inline const std::string &name() const { return name_; }

    inline int32_t affinity() const { return cpuno_; }

 protected:
    void set_thread_name(const char *name) {
        // Only for linux
        if (name) {
#ifndef __APPLE__
            pthread_setname_np(native_handle(), name);
#else
            pthread_setname_np(name);
#endif
            name_.assign(name);
        }
    }

    void set_affinity(int32_t cpuno) {
#ifndef __APPLE__
        // Only for linux
        cpuno_ = cpuno;
        if (cpuno > 0) {
            CPU_SET(cpuno, &bitmask_);
            pthread_setaffinity_np(native_handle(), sizeof(bitmask_), &bitmask_);
        } else {
            memset(&bitmask_, 0xff, sizeof(bitmask_));
            pthread_setaffinity_np(native_handle(), sizeof(bitmask_), &bitmask_);
        }
#endif
    }

 protected:
    std::string name_;
    int32_t cpuno_;

#ifndef __APPLE__
    cpu_set_t bitmask_;
#endif
};

// fast::thread(fnRoutine, argv)("threadname")(1).detach()

class threadpool {
 public:
    template <typename _Callable, typename... _Args>
    explicit threadpool(int32_t tcap, int32_t cpuids[], const char *name)
        : thread_num_(tcap), cpuids_(cpuids, cpuids + tcap), name_(name) {}

    template <typename _Callable, typename... _Args>
    void run(_Callable &&__f, _Args &&... __args) {
        for (int32_t tno = 0; tno < thread_num_; ++tno) {
            std::string threadname;
            std::stringstream ss;
            ss << name_ << '-' << tno << '-' << cpuids_[tno];
            ss >> threadname;
            ss.clear();
            fast::thread(__f, __args...)(threadname.c_str())(cpuids_[tno]).detach();
        }
    }

 protected:
    int32_t thread_num_;
    std::vector<int32_t> cpuids_;
    std::string name_;
};

typedef void *(*fn_task_proc)(void *arg);
class task {
 public:
    explicit task() {}

    ~task() {
        running_ = false;
        thread_.detach();
    }

 public:
    bool run(int32_t cpuno) {
        // 获取系统的cpu个数
        long processors = sysconf(_SC_NPROCESSORS_ONLN);
        if (processors <= 0) {
            std::cout << "Thread::Create() failed,reason:current cpu number <= 0" << std::endl;
            return false;
        }

        assert(cpuno <= processors);
        cpuno_ = cpuno;
        thread_.swap(fast::thread(task::entrance, this));

        // 设置线程名称
        std::stringstream ss;
        std::string taskname;
        ss << "task-" << thread_.native_handle() << '-' << cpuno;
        ss >> taskname;
        ss.clear();
        thread_(taskname.c_str())(cpuno);
        return true;
    }

    void wait() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void stop() { running_ = false; }

    //线程函数
    static void *entrance(void *arg) {
        task *pThis = (task *)arg;
        pThis->running_ = true;

        pThis->routine(arg);

        return nullptr;
    }

    /*获取线程名称*/
    const std::string &name() const { return thread_.name(); }

    /*获取执行线程handle*/
    size_t handle() {
#ifndef __APPLE__
        return thread_.native_handle();
#else
        return 0;
#endif
    }

    /*获取线程所属的cpu*/
    int32_t cpuno() const { return thread_.affinity(); }

 protected:
    virtual void *routine(void *arg) = 0;

 protected:
    bool running_ = false;  //标识线程是否在运行

 private:
    int32_t cpuno_;
    fast::thread thread_;
};
}  // namespace flowsql

#endif  //_FAST_COMMON_TS_THREAD_H_
