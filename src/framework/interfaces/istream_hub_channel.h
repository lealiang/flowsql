#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_HUB_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_HUB_CHANNEL_H_

#include <framework/interfaces/istream_channel.h>

#include <memory>

namespace flowsql {

interface __attribute__((visibility("default"))) IStreamHubChannel : public IStreamChannel {
    virtual const char* HubMode() const = 0;  // split|merge
    virtual size_t PartitionCount() const = 0;
    virtual std::shared_ptr<IStreamChannel> GetPartition(size_t idx) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_HUB_CHANNEL_H_
