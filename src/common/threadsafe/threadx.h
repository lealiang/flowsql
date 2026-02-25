/*
 * Author       : LIHUO
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Date         : 2020-10-22 15:36:01
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_COMMON_THREADSAFE_THREADX_H_
#define _FLOWSQL_COMMON_THREADSAFE_THREADX_H_

#include "../../common/logger_helper.h"
#include "../../common/singleton.h"

#ifndef __APPLE__
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#endif

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <functional>

#include <map>
#include <mutex>

namespace flowsql {

class threadx {
 private:
    /* data */

 public:
    threadx(/* args */) : is_stop_(true), handle_(0) {
#ifndef __APPLE__
        this->cpu_count_ = get_nprocs();
#else
        this->cpu_count_ = 2;
#endif
    }

    ~threadx() {
        this->stop();
        this->join();
    }

    threadx(std::function<void()>&& f) : handle_(0) {
        this->func_ = std::move(f);
        this->start();
        return;
    }

 public:
    inline void stop() { is_stop_ = true; }

    inline bool is_stop() const { return is_stop_; }

    inline operator bool() const { return this->handle_ != 0; }

    inline void start(std::function<void()>&& f) {
        this->func_ = std::move(f);
        this->start();
    }

    void detach() { this->handle_ = 0; }

    void join() {
        if (0 == this->handle_) {
            return;
        }

        void* ret;
        pthread_join(this->handle_, &ret);
        this->handle_ = 0;
        return;
    }

    bool joinable() const { return this->handle_ != 0; }

    void set_affinity(int32_t cpuno) {
#ifndef __APPLE__
        int ret = 0;
        // Only for linux
        cpuno_ = cpuno;
        if (cpuno > 0 && cpuno < this->cpu_count_) {
            CPU_SET(cpuno, &bitmask_);
            ret = pthread_setaffinity_np(handle_, sizeof(bitmask_), &bitmask_);
        } else {
            memset(&bitmask_, 0xff, sizeof(bitmask_));
            ret = pthread_setaffinity_np(handle_, sizeof(bitmask_), &bitmask_);
        }
        if (ret != 0) {
            LOG_E() << "pthread_setaffinity_np, cpuid: " << cpuno << " fail, ret: " << ret;
        }
#endif
    }

 protected:
    inline void start() {
        pthread_create(&handle_, nullptr, &threadx::thread_worker, this);
        return;
    }

    void run() {
        if (this->func_) {
            this->func_();
        } else {
            printf("errror thread func\r\n");
        }
        return;
    }

    static void* thread_worker(void* args) {
        threadx* pthis = (threadx*)args;
        pthis->is_stop_ = false;
        pthis->run();
        return 0;
    }

 protected:
    bool is_stop_;

    std::function<void()> func_;
    pthread_t handle_;

    int32_t cpu_count_;
    int32_t cpuno_;

#ifndef __APPLE__
    cpu_set_t bitmask_;
#endif
};

class thread_name_mgr : public singleton<thread_name_mgr> {
 public:
    void reg(int tid, const std::string& name) {
        std::lock_guard<std::mutex> gd(this->mutex_);

        this->tid_map_[tid] = name;
    }

    bool find(int tid, std::string& name) {
        std::lock_guard<std::mutex> gd(this->mutex_);

        auto itr = this->tid_map_.find(tid);
        if (itr != this->tid_map_.end()) {
            name = itr->second;
            return true;
        }
        return false;
    }

    void view(const std::function<void(int /*tid*/, const char* name)>& f) {
        std::lock_guard<std::mutex> gd(this->mutex_);

        for (auto itr = this->tid_map_.begin(); itr != this->tid_map_.end(); ++itr) {
            f(itr->first, itr->second.c_str());
        }
    }

    void report_thread_memory(const std::function<void(const char* name /*thread name*/, int64_t memory_used)>& f) {
        default_allocator* alloctor = default_allocator::instance();
        alloctor->report([&](int tid, int64_t mm) {
            std::string name;
            if (this->find(tid, name)) {
                f(name.c_str(), mm);
            } else {
                char tid_buf[20];
                snprintf(tid_buf, sizeof(tid_buf), "%d", tid);
                f(tid_buf, mm);
            }
        });
    }

 protected:
    std::mutex mutex_;
    std::map<int, std::string> tid_map_;
};

}  // namespace flowsql

#ifndef _WIN32

#ifdef __APPLE__
#define set_threadx_name()
#else
#define set_threadx_name()                                           \
    {                                                                \
        int tid = gettid();                                          \
        char ___buff[300];                                           \
        snprintf(___buff, sizeof(___buff), "%s%s", "xd-", __func__); \
        prctl(PR_SET_NAME, (unsigned long)___buff, 0, 0, 0);         \
        fast::thread_name_mgr::instance()->reg(tid, ___buff);        \
    }
#endif

#else
#define set_threadx_name()
#endif

#endif
