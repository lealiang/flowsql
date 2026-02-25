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
 * Author       : fei.wang
 * Date         : 2020-12-20 12:14:48
 * LastEditors  : kun.dong
 * LastEditTime : 2021-01-10 10:14:08
 */
#ifndef SRC_FAST_BLOCK_BUFFER_H_
#define SRC_FAST_BLOCK_BUFFER_H_

#include <concurrentqueue/concurrentqueue.h>
#include <stdint.h>

#ifndef __APPLE__
#include <malloc.h>
#endif

namespace flowsql {
#pragma pack(push)
#pragma pack(1)
struct Mem {
    uint32_t proc_ident_;    // process identity
    uint32_t thread_ident_;  // thread identity
    uint32_t serial_no_;     // serial_no
    uint32_t free_sec_;      // free seconds?
    uint32_t called_cnt_;    // called count
    uint32_t data_len_;      // total data length
    uint32_t data_off_;      // data offset for write
    // uint32_t read_off_;      // read offset (for read)
};
#pragma pack(pop)

class ThreadBuffer {
 public:
    ThreadBuffer();
    ~ThreadBuffer();

 public:
    inline Mem* get_submit_buff(uint64_t process_msec);
    inline void revert_submit_buff(Mem* buff);
    template <typename Garbage>
    inline Mem* get_inused_buff();
    inline void rel_inused_buff(Mem*);
    inline Mem* get_exit_buff();

 private:
    uint32_t m_last_alloc_sec;
    uint64_t m_last_process_msec;
    std::atomic<Mem*> m_inused_buff;
    Mem* m_null_buff;
    moodycamel::ConcurrentQueue<Mem*> m_submit_queue;
};

class ThreadBufferQueue : public moodycamel::ConcurrentQueue<ThreadBuffer*> {
 public:
    static ThreadBufferQueue* instanse() {
        static ThreadBufferQueue _tbq;
        return &_tbq;
    }
};

template <int32_t _BLOCK_SIZE = 4UL << 20, int64_t _MAX_SIZE = 4UL << 30, int32_t _ALIGNED_SIZE = 0x10>
class ThreadBufferGarbage : public moodycamel::ConcurrentQueue<Mem*> {
 public:
    static ThreadBufferGarbage* instanse() {
        static ThreadBufferGarbage _tbg;
        return &_tbg;
    }

    Mem* allocate() {
        Mem* _buff = nullptr;
        if (!try_dequeue(_buff)) {
            if (alloc_bytes_ >= _MAX_SIZE || !(_buff = (Mem*)memalign(_ALIGNED_SIZE, /*sizeof(Mem) + */ _BLOCK_SIZE))) {
                return nullptr;
            }
            memset(_buff, 0, sizeof(Mem));
            alloc_bytes_ += _BLOCK_SIZE;
        }
        _buff->data_len_ = _BLOCK_SIZE /* + sizeof(Mem)*/;
        _buff->serial_no_ = serial_no_.fetch_add(1);
        _buff->called_cnt_++;
        _buff->proc_ident_ = 0;    // (int32_t)getpid();
        _buff->thread_ident_ = 0;  //(long int)syscall(SYS_gettid);
        _buff->data_off_ = sizeof(Mem);
        return _buff;
    }

    void delocate(Mem* buff) {
        if (!enqueue(buff)) {
            free(buff);
        }
    }

 private:
    ThreadBufferGarbage() {}
    ~ThreadBufferGarbage() {
        Mem* _buff = nullptr;
        while (try_dequeue(_buff)) {
            free(_buff);
        }
    }

 private:
    typedef std::atomic<std::uint64_t> atomic_uint64_t;
    atomic_uint64_t serial_no_ = 0;
    atomic_uint64_t alloc_bytes_ = 0;
};

ThreadBuffer::ThreadBuffer() : m_last_alloc_sec(9), m_last_process_msec(0), m_inused_buff(NULL) {
    ThreadBufferQueue::instanse()->enqueue(this);
}

ThreadBuffer::~ThreadBuffer() {
    // do release.
}

inline Mem* ThreadBuffer::get_submit_buff(uint64_t process_msec) {
    Mem* submit_buff = NULL;
    if (process_msec == 0 || process_msec - m_last_process_msec > 10) {
        if (!m_submit_queue.try_dequeue(submit_buff)) {
            submit_buff = m_inused_buff.exchange(NULL);
        }
    }
    m_last_process_msec = submit_buff ? process_msec : m_last_process_msec;
    return submit_buff;
}

template <typename Garbage>
Mem* ThreadBuffer::get_inused_buff() {
    Mem* inused_buff = m_inused_buff.exchange(nullptr);
    if (!inused_buff) {
        // mem alloc form global allocator.
        inused_buff = Garbage::instanse()->allocate();
    }
    return inused_buff;
}  // namespace flowsql

void ThreadBuffer::rel_inused_buff(Mem* buff) {
    if (buff->data_off_ + 64 >= buff->data_len_) {
        m_submit_queue.enqueue(buff);
        buff = nullptr;
    }
    m_inused_buff.exchange(buff);
}

Mem* ThreadBuffer::get_exit_buff() { return m_inused_buff.exchange(NULL); }

}  // namespace flowsql

#endif  // SRC_FAST_BLOCK_BUFFER_H_
