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
 * Author       : kun.dong
 * Date         : 2020-11-14 22:08:49
 * LastEditors  : kun.dong
 * LastEditTime : 2020-11-17 15:24:54
 */
#ifndef STATIC_REFL_H
#define STATIC_REFL_H
#include <cstddef>
#include <fast/reflection/meta_maco.hpp>
#include <type_traits>
#include <utility>

#define FIELD_EACH(i, arg)                                                 \
    PAIR(arg);                                                             \
    template <typename T>                                                  \
    struct FIELD<T, i> {                                                   \
        T& obj;                                                            \
        FIELD(T& obj) : obj(obj) {}                                        \
        auto value() -> decltype(auto) { return (obj.STRIP(arg)); }        \
        static constexpr const char* name() { return STRING(STRIP(arg)); } \
    } __attribute__((__aligned__(1)));

#define DEFINE_STRUCT(st, ...)                                                 \
    struct st {                                                                \
        template <typename, size_t>                                            \
        struct FIELD;                                                          \
        static constexpr size_t _field_count_ = GET_ARG_COUNT(__VA_ARGS__);    \
        PASTE(REPEAT_, GET_ARG_COUNT(__VA_ARGS__))(FIELD_EACH, 0, __VA_ARGS__) \
    } __attribute__((__aligned__(1)));

template <typename T, typename = void>
struct IsRefected : std::false_type {};

template <typename T>
struct IsRefected<T, std::void_t<decltype(&T::_field_count_)>> : std::true_type {};

template <typename T>
constexpr static bool IsReflected_v = IsRefected<T>::value;

template <typename T, typename F, size_t... Is>
inline constexpr void forEach(T&& obj, F&& f, std::index_sequence<Is...>) {
    using TDECAY = std::decay_t<T>;
    (void(f(typename TDECAY::template FIELD<T, Is>(std::forward<T>(obj)).name(),
            typename TDECAY::template FIELD<T, Is>(std::forward<T>(obj)).value())),
     ...);
}

template <typename T, typename F>
inline constexpr void forEach(T&& obj, F&& f) {
    forEach(std::forward<T>(obj), std::forward<F>(f), std::make_index_sequence<std::decay_t<T>::_field_count_>{});
}

#endif