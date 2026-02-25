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
 * Date         : 2020-08-18 13:32:13
 * LastEditors  : Pericels
 * LastEditTime : 2022-06-16 19:11:08
 */

#ifndef SRC_0_2_FAST_TYPEDEF_H_
#define SRC_0_2_FAST_TYPEDEF_H_

#ifndef __APPLE__
#include <linux/limits.h>
#include <byteswap.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define __LITTLE_ENDIAN_BITFIELD

#undef interface
#define interface struct

#define CACHE_LINE_SIZE 64

#ifdef __linux__
#define FAST_MEM_ALIGN(a) __attribute__((__aligned__(a)))
#define FAST_MEM_ALIGN_PACKED __attribute__((packed))
#else
#define FAST_MEM_ALIGN(a)
#endif

#define FAST_CACHELINE_ALIGN FAST_MEM_ALIGN(CACHE_LINE_SIZE)

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
typedef void *thandle;
#define loadlibrary(dll_name) dlopen(dll_name, RTLD_NOW)
#define getprocaddress(handle, func_name) dlsym(handle, func_name)
#define freelibrary(handle) dlclose(handle)
#define getlasterror dlerror
#else
#include <Windows.h>
typedef HMODULE thandle;
#define loadlibrary(dll_name) LoadLibraryA(dll_name)
#define freelibrary(handle) FreeLibrary(handle)
#define getprocaddress(handle, func_name) GetProcAddress(handle, func_name)
#define getlasterror GetLastError
#endif

#if defined(__linux__) || defined(__APPLE__)
#ifdef __cplusplus
#define EXPORT_API extern "C" __attribute__((visibility("default")))
#define IMPORT_API extern "C" __attribute__((visibility("default")))
#else
#define EXPORT_API __attribute__((visibility("default")))
#define IMPORT_API __attribute__((visibility("default")))
#endif  // __cplusplus
#else   // __linux__
#ifdef __cplusplus
#define EXPORT_API extern "C" __declspec(dllexport)
#define IMPORT_API extern "C" __declspec(dllimport)
#else
#define EXPORT_API __declspec(dllexport)
#define IMPORT_API __declspec(dllimport)
#endif  // __cplusplus
#endif  // __linux__

typedef __int128_t int128_t;
typedef __uint128_t uint128_t;

#define e2i(x) static_cast<int32_t>(x)

#define h2n16(x) bswap_16(x)
#define h2n32(x) bswap_32(x)
#define h2n64(x) bswap_64(x)
#define n2h16(x) bswap_16(x)
#define n2h32(x) bswap_32(x)
#define n2h128(x) (bswap_64(x >> 64) | bswap_64(x))

#endif  // SRC_0_2_FAST_TYPEDEF_H_
