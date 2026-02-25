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
 * Author       : Author Name
 * Date         : 2020-08-16 10:54:38
 * LastEditors  : kun.dong
 * LastEditTime : 2021-01-07 14:30:42
 */
#ifndef BASE_COMMON_TOOLKIT_H_
#define BASE_COMMON_TOOLKIT_H_
#include <dirent.h>
#ifndef __APPLE__
#include <linux/limits.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "defer.hpp"

extern char** environ;
namespace flowsql {

/**
 * @ remark: 结尾不是‘/’则认定最后一级为文件而不会创建目录
 * 支持相对路径
 **/
inline int mkdir_parents(const char* fullpath) {
    size_t begincmppath = 0;
    size_t endcmppath = 0;
    char dupstr[260] = {0};

    if (!fullpath) {
        return -1;
    }

    if ('/' != fullpath[0]) {  // Relative path
        char* current = getcwd(nullptr, 0);
        if (!current) {
            return -1;
        }
        begincmppath = strlen(current) + 1;
        snprintf(dupstr, sizeof(dupstr), "%s/%s", current, fullpath);
        free(current);
    } else {
        snprintf(dupstr, sizeof(dupstr), "%s", fullpath);
        begincmppath = 1;
    }

    endcmppath = strlen(dupstr);
    for (size_t i = begincmppath; i < endcmppath; i++) {
        if (dupstr[i] == '/') {
            dupstr[i] = '\0';
            if (::access(dupstr, F_OK) != 0 && ::mkdir(dupstr, S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP |
                                                                   S_IWOTH | S_IXOTH | S_IXGRP | S_IXUSR) != 0) {
                return -1;
            }
            dupstr[i] = '/';
        }
    }
    return 0;
}

inline int mkdir(const char* fullpath) {
    size_t begincmppath = 0;
    size_t endcmppath = 0;
    char dupstr[260] = {0};

    if (!fullpath) {
        return -1;
    }

    if ('/' != fullpath[0]) {  // Relative path
        char* current = getcwd(nullptr, 0);
        if (!current) {
            return -1;
        }
        begincmppath = strlen(current) + 1;
        snprintf(dupstr, sizeof(dupstr), "%s/%s", current, fullpath);
        free(current);
    } else {
        snprintf(dupstr, sizeof(dupstr), "%s", fullpath);
        begincmppath = 1;
    }

    endcmppath = strlen(dupstr);
    for (size_t i = begincmppath; i < endcmppath; i++) {
        if (dupstr[i] == '/') {
            dupstr[i] = '\0';
            if (::access(dupstr, F_OK) != 0 &&
                ::mkdir(dupstr, S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH) != 0) {
                return -1;
            }
            dupstr[i] = '/';
        }
    }

    if (::access(dupstr, F_OK) != 0 &&
        ::mkdir(dupstr, S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH) != 0) {
        return -1;
    }
    return 0;
}

inline int64_t get_file_size(const char* fpname) {
    FILE* fp = fopen(fpname, "rb");
    if (nullptr == fp) {
        return 0;
    }
    int64_t old_pos = ftell(fp);
    fseek(fp, 0, SEEK_END);
    int64_t end_pos = ftell(fp);
    if (end_pos != old_pos) {
        fseek(fp, 0, old_pos);
    }

    return end_pos;
}

inline const char* get_absolute_process_name() {
    static const char* s_abspathname_ptr = nullptr;
    static char s_abspathname[PATH_MAX] = {0};

    if (!s_abspathname_ptr) {
        int rbyte = ::readlink("/proc/self/exe", s_abspathname, PATH_MAX);
        if (rbyte < 0 || rbyte > PATH_MAX) {
            s_abspathname[0] = 0;
        } else {
            s_abspathname[rbyte] = 0;
        }

        s_abspathname_ptr = s_abspathname;
    }

    return s_abspathname_ptr;
}

inline const char* get_absolute_process_path() {
    static const char* s_abspath_ptr = nullptr;
    static char s_abspath[PATH_MAX] = {0};

    if (!s_abspath_ptr) {
        int rbyte = ::readlink("/proc/self/exe", s_abspath, PATH_MAX);
        if (rbyte < 0 || rbyte > PATH_MAX) {
            s_abspath[0] = 0;
        } else {
            s_abspath[rbyte] = 0;
            for (int lp = rbyte - 1; lp >= 0; --lp) {
                if (s_abspath[lp] == '/') {
                    s_abspath[lp] = 0;
                    break;
                }
            }
        }

        s_abspath_ptr = s_abspath;
    }

    return s_abspath_ptr;
}

template <typename... Args>
inline const char* calc_unique_process_fingerprint(const char* abspathname, const char* format, Args... args) {
    static const char* s_fingerprint_ptr = nullptr;
    static char s_fingerprint[PATH_MAX] = {0};
    if (!s_fingerprint_ptr) {
        int len = snprintf(s_fingerprint, PATH_MAX, format, args...);
        int32_t absfnl = strlen(abspathname);
        for (int32_t append = 0; append < absfnl; ++append) {
            if (abspathname[append] == '/' || abspathname[append] == '\\') {
                s_fingerprint[len + append] = '_';
            } else {
                s_fingerprint[len + append] = abspathname[append];
            }
        }
        s_fingerprint[len + absfnl] = 0;
        s_fingerprint_ptr = s_fingerprint;
    }

    return s_fingerprint_ptr;
}

inline int32_t verify_singleton_process(const char* fingerprint) { return 0; }

template <typename Predicate>
static void enum_files(const char* basepath, Predicate _pred) {
    DIR* dir = nullptr;
    struct dirent* dirent = nullptr;
    if ((dir = opendir(basepath)) != nullptr) {
        while ((dirent = readdir(dir)) != NULL) {
            if (dirent->d_name[0] != '.') {
                _pred(basepath, dirent);
            }
        }
    }
    closedir(dir);
}

// Only For Linux
inline void set_proc_title(int argc, char* argv[], const char* title) {
    size_t argv0len = strlen(argv[0]);
    size_t titlelen = strlen(title);
    if (argv0len >= titlelen) {
        strncpy(argv[0], title, argv0len);
    } else {
        size_t size = 0;
        for (int32_t i = 0; environ[i]; ++i) {
            size += strlen(environ[i]) + 1;
        }

        char* last = argv[0];
        for (int i = 0; i < argc; ++i) {
            last += strlen(argv[i]) + 1;
        }

        //// newenv会内存泄漏
        char* newenv = new char[size];
        for (int32_t i = 0; environ[i]; ++i) {
            size_t envsize = strlen(environ[i]) + 1;
            memcpy(newenv, environ[i], envsize);
            environ[i] = newenv;
            newenv += envsize;
            last += envsize;
        }

        --last;

        strncpy(argv[0], title, last - argv[0]);
    }
}

inline int32_t split_argument(const char* cmdline, std::function<int32_t(int32_t, char**)> worker) {
    size_t cmdlen = strlen(cmdline);
    char* argvstr = new char[cmdlen + 1]{0};
    char** argvptr = new char* [(cmdlen + 1) / 2] { 0 };
    defer(delete[] argvptr);
    defer(delete[] argvstr);
    memcpy(argvstr, cmdline, cmdlen);

    enum { IDLE, TOKEN };
    auto fn_isblank = [&](char c) -> bool { return c == ' ' || c == '\t'; };
    int32_t argc = 0;
    int32_t token = IDLE;
    for (size_t i = 0; i < cmdlen; ++i) {
        bool isblank = fn_isblank(argvstr[i]);
        switch (token) {
            case IDLE:
                if (!isblank) {
                    argvptr[argc++] = argvstr + i;
                    token = TOKEN;
                } else {
                    argvstr[i] = 0;
                }
            case TOKEN:
                if (isblank) {
                    argvstr[i] = 0;
                    token = IDLE;
                }
        }
    }

    return worker(argc, argvptr);
}

inline const char* get_file_ext(const char* fname) {
    size_t flen = strlen(fname);
    size_t pos = flen - 1;
    for (; pos >= 0 && fname[pos] != '.'; --pos) {
    }

    return fname + pos + 1;
}
}  // namespace flowsql

#endif  // BASE_COMMON_TOOLKIT_H_
