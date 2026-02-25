#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_

#include <common/loader.hpp>

#include <string>

#include "idata_entity.h"

namespace flowsql {

// {0xc1d2e3f4-abcd-ef01-2345-6789abcdef01}
const Guid IID_CHANNEL = {0xc1d2e3f4, 0xabcd, 0xef01, {0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01}};

interface IChannel : public IPlugin {
    // 元数据
    virtual std::string Catelog() = 0;
    virtual std::string Name() = 0;
    virtual std::string Description() = 0;

    // 生命周期
    virtual int Open() = 0;
    virtual int Close() = 0;
    virtual bool IsOpened() const = 0;

    // 数据操作
    virtual int Put(IDataEntity* entity) = 0;
    virtual IDataEntity* Get() = 0;

    // 批量刷新
    virtual int Flush() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_
