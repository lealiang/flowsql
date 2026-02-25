#ifndef _FLOWSQL_PLUGINS_EXAMPLE_MEMORY_CHANNEL_H_
#define _FLOWSQL_PLUGINS_EXAMPLE_MEMORY_CHANNEL_H_

#include <framework/interfaces/ichannel.h>

#include <memory>
#include <queue>
#include <string>

namespace flowsql {

class MemoryChannel : public IChannel {
 public:
    MemoryChannel() = default;
    ~MemoryChannel() override = default;

    // IPlugin
    int Load() override { return 0; }
    int Unload() override { return 0; }

    // IChannel 元数据
    std::string Catelog() override { return "example"; }
    std::string Name() override { return "memory"; }
    std::string Description() override { return "In-memory queue channel for testing"; }

    // 生命周期
    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_; }

    // 数据操作
    int Put(IDataEntity* entity) override;
    IDataEntity* Get() override;

    // 批量刷新
    int Flush() override { return 0; }

 private:
    bool opened_ = false;
    std::queue<std::shared_ptr<IDataEntity>> queue_;
    std::shared_ptr<IDataEntity> last_get_;  // 保持 Get() 返回指针的生命周期
};

}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_EXAMPLE_MEMORY_CHANNEL_H_
