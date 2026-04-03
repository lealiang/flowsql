#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_FACTORY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_FACTORY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>

namespace flowsql {

interface IStreamChannel;

// {0xe5f6a7b8-cdef-0123-4567-89abcdef0123}
const Guid IID_STREAM_FACTORY = {0xe5f6a7b8, 0xcdef, 0x0123,
                                 {0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23}};

// IStreamFactory — 流式通道工厂接口
// 负责流式通道查找与枚举，供 Scheduler 按 type.name 进行 source 解析
interface IStreamFactory {
    virtual ~IStreamFactory() = default;

    // 按 type + name 查找已注册流式通道
    // 返回值：通道指针（工厂持有所有权），失败返回 nullptr
    virtual IStreamChannel* Get(const char* type, const char* name) = 0;

    // 列出所有已注册流式通道（用于管理面展示）
    virtual void List(std::function<void(const char* type, const char* name,
                                         IStreamChannel*)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_FACTORY_H_
