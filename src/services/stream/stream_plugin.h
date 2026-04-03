#ifndef _FLOWSQL_SERVICES_STREAM_STREAM_PLUGIN_H_
#define _FLOWSQL_SERVICES_STREAM_STREAM_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/istream_factory.h>
#include <framework/interfaces/istream_manager.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct sqlite3;

namespace flowsql {
namespace stream {

class __attribute__((visibility("default"))) StreamPlugin : public IPlugin,
                                                            public IStreamFactory,
                                                            public IStreamManager {
 public:
    StreamPlugin() = default;
    ~StreamPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IStreamFactory
    IStreamChannel* Get(const char* type, const char* name) override;
    void List(std::function<void(const char* type, const char* name, IStreamChannel*)> callback) override;

    // IStreamManager
    int AddChannel(const std::string& type,
                   const std::string& name,
                   const std::string& option) override;
    int ModifyChannel(const std::string& type,
                      const std::string& name,
                      const std::string& option) override;
    int RemoveChannel(const std::string& type,
                      const std::string& name) override;
    void QueryChannels(std::function<void(const std::string& type,
                                          const std::string& name,
                                          const std::string& option,
                                          const std::string& status)> callback) override;

 private:
    struct StreamChannelConfig {
        std::string type;
        std::string name;
        std::string option;
    };

    int LoadFromYamlLocked();
    int LoadFromStoreLocked();
    int PersistAllIfStoreEmptyLocked();
    int BuildChannelsLocked();
    int BuildOneChannelLocked(const StreamChannelConfig& cfg,
                              std::shared_ptr<IStreamChannel>* out);
    int EnsureStoreReadyLocked();
    int UpsertStoreLocked(const StreamChannelConfig& cfg);
    int DeleteStoreLocked(const std::string& type, const std::string& name);
    bool StoreEnabled() const { return !db_path_.empty(); }

    static std::string MakeKey(const std::string& type, const std::string& name);

    std::unordered_map<std::string, StreamChannelConfig> configs_;
    std::unordered_map<std::string, std::shared_ptr<IStreamChannel>> channels_;
    std::mutex mutex_;

    IQuerier* querier_ = nullptr;
    std::string config_file_;
    std::string db_path_;
    sqlite3* db_ = nullptr;
    static thread_local std::string last_error_;
};

}  // namespace stream
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_STREAM_STREAM_PLUGIN_H_
