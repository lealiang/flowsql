/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_CHANNEL_H_

#include "ichannel.h"
#include "idataframe.h"

namespace flowsql {

// {0xe2f3a4b5-cdef-0123-4567-89abcdef0123}
const Guid IID_DATAFRAME_CHANNEL = {0xe2f3a4b5, 0xcdef, 0x0123, {0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23}};

/**
 * @brief DataFrame 通道接口。
 *
 * 约定：Write 使用替换语义，Read 使用快照语义（非破坏性读取）。
 */
interface IDataFrameChannel : public IChannel {
    /**
     * @brief 写入 DataFrame 到通道。
     * @param df 输入 DataFrame 指针（非空）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Write(IDataFrame* df) = 0;

    /**
     * @brief 读取通道当前快照到指定 DataFrame。
     * @param df 输出 DataFrame 指针（非空）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Read(IDataFrame* df) = 0;
};

// {0xa7b8c9d0-e1f2-3456-789a-bcdef0123456}
const Guid IID_APPENDABLE_DATAFRAME_CHANNEL = {0xa7b8c9d0, 0xe1f2, 0x3456, {0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56}};

/**
 * @brief 可追加写的 DataFrame 通道扩展接口。
 */
interface IAppendableDataFrameChannel : public IDataFrameChannel {
    /**
     * @brief 将数据追加到通道尾部。
     * @param df 输入 DataFrame 指针（非空）。
     * @return 0 表示成功，非 0 表示失败。
     *
     * 要求 schema 与当前通道一致；若通道为空则可使用 df 的 schema 初始化。
     */
    virtual int Append(IDataFrame* df) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_CHANNEL_H_
