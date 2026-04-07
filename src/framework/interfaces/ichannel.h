/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_

#include <common/guid.h>
#include <common/typedef.h>

namespace flowsql {

// {0xc1d2e3f4-abcd-ef01-2345-6789abcdef01}
const Guid IID_CHANNEL = {0xc1d2e3f4, 0xabcd, 0xef01, {0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01}};

// 通道类型常量
namespace ChannelType {
    constexpr const char* kDataFrame = "dataframe";
    constexpr const char* kDatabase  = "database";
    constexpr const char* kStream = "stream";
    constexpr const char* kBlockStream = "block_stream";
}  // namespace ChannelType

/**
 * @brief 数据通道抽象基接口，定义通道身份、元数据与生命周期。
 *
 * 具体数据读写能力由子接口扩展（如 IDataFrameChannel、IDatabaseChannel、IStreamChannel）。
 */
interface IChannel {
    virtual ~IChannel() = default;

    /**
     * @brief 返回通道类别（catalog/type 前缀）。
     * @return 通道类别字符串，如 "dataframe"、"mysql"、"stream"。
     */
    virtual const char* Category() = 0;
    /**
     * @brief 返回通道名称（不含类别前缀）。
     * @return 通道名称字符串。
     */
    virtual const char* Name() = 0;

    /**
     * @brief 返回通道大类类型。
     * @return 类型字符串，典型值见 ChannelType 常量。
     */
    virtual const char* Type() = 0;

    /**
     * @brief 返回通道元数据描述。
     * @return 元数据字符串，格式由具体实现定义（例如 Arrow Schema JSON）。
     */
    virtual const char* Schema() = 0;

    /**
     * @brief 打开通道资源。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Open() = 0;
    /**
     * @brief 关闭通道资源。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Close() = 0;
    /**
     * @brief 查询通道是否处于已打开状态。
     * @return true 表示已打开，false 表示未打开。
     */
    virtual bool IsOpened() const = 0;

    /**
     * @brief 刷新通道内部缓冲。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Flush() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_
