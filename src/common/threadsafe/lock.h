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
#ifndef _FA_COMMON_TS_LOCK_H_
#define _FA_COMMON_TS_LOCK_H_

#if defined(__linux__) || defined(__APPLE__)
#include <sys/time.h>
#include <unistd.h>
#else
#include <windows.h>
#endif  // __linux__

#include <emmintrin.h>

#include <condition_variable>
#include <mutex>
#include <shared_mutex>

namespace flowsql {
class lockless {
 public:
    inline void lock() {}
    inline void unlock() {}
    inline bool try_lock() { return true; }
};

typedef std::mutex lock;                 // C++11
typedef std::shared_timed_mutex rwlock;  // C++14

#ifdef __linux__
class spinlock {
 public:
    spinlock() { locked_ = 0; }

    inline void lock() {
#ifdef __x86_64__
        int lock_val = 1;
        asm volatile(
            "1:\n"
            "xchg %[locked], %[lv]\n"
            "test %[lv], %[lv]\n"
            "jz 3f\n"
            "2:\n"
            "pause\n"
            "cmpl $0, %[locked]\n"
            "jnz 2b\n"
            "jmp 1b\n"
            "3:\n"
            : [ locked ] "=m"(locked_), [ lv ] "=q"(lock_val)
            : "[lv]"(lock_val)
            : "memory");
#elif __aarch64__
        int exp = 0;

        while (!__atomic_compare_exchange_n(&locked_, &exp, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            while (__atomic_load_n(&locked_, __ATOMIC_RELAXED)) _mm_pause();
            exp = 0;
        }
#endif
    }

    inline bool try_lock() {
#ifdef __x86_64__
        int lockval = 1;
        asm volatile("xchg %[locked], %[lockval]"
                     : [ locked ] "=m"(locked_), [ lockval ] "=q"(lockval)
                     : "[lockval]"(lockval)
                     : "memory");
        return lockval == 0;
#elif __aarch64__
        int exp = 0;
        return __atomic_compare_exchange_n(&locked_, &exp, 1, 0, /* disallow spurious failure */
                                           __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
#endif
    }

    inline void unlock() {
#ifdef __x86_64__
        int unlock_val = 0;
        asm volatile("xchg %[locked], %[ulv]\n"
                     : [ locked ] "=m"(locked_), [ ulv ] "=q"(unlock_val)
                     : "[ulv]"(unlock_val)
                     : "memory");
#elif __aarch64__
        __atomic_store_n(&locked_, 0, __ATOMIC_RELEASE);
#endif
    }

 private:
    volatile int locked_; /**< lock status 0 = unlocked, 1 = locked */
};
#else
typedef std::mutex spinlock;
#endif  // __linux__

// lock scope guard
typedef std::shared_lock<rwlock> rlocker;
typedef std::lock_guard<rwlock> wlocker;
typedef std::lock_guard<lock> locker;
typedef std::lock_guard<spinlock> spinlocker;

/* How to use */
/*
std::shared_mutex rwlock;

void readthread()
{
  fast::rlocker rlock(rwlock);
  ...
}
void writethread()
{
  fast::wlocker wlock(rwlock);
  ...
}
*/

class condition {
 public:
    template <typename _Predicate>
    void wait(_Predicate __p) {
        std::unique_lock<std::mutex> __lock(m_mlock);
        while (!__p()) m_cv.wait(__lock, __p);
    }

    void wait() noexcept {
        std::unique_lock<std::mutex> __lock(m_mlock);
        m_cv.wait(__lock);
    }

    void notify_one() noexcept { m_cv.notify_one(); }

    void notify_all() noexcept { m_cv.notify_all(); }

 private:
    std::mutex m_mlock;            // 全局互斥锁.
    std::condition_variable m_cv;  // 全局条件变量.
};

}  // namespace flowsql

#endif  //_FA_COMMON_TS_LOCK_H_
