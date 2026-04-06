#ifndef _FLOWSQL_SERVICES_SCHEDULER_JSON_CODEC_H_
#define _FLOWSQL_SERVICES_SCHEDULER_JSON_CODEC_H_

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "broadcast_hub.h"
#include "stream_task.h"
#include "stream_task_group.h"

namespace flowsql {
namespace scheduler {

struct GroupNodeResolvedSourceMeta;

const char* StreamTaskStatusName(StreamTaskStatus status);
bool IsTerminalStreamTaskStatus(StreamTaskStatus status);

void WriteTaskSnapshotJson(rapidjson::Writer<rapidjson::StringBuffer>* w,
                           const TaskSnapshot& s);
void WriteGroupSnapshotJson(rapidjson::Writer<rapidjson::StringBuffer>* w,
                            const StreamGroupSnapshot& s,
                            const std::vector<BroadcastHubSnapshot>* share_sets,
                            const std::unordered_map<std::string, GroupNodeResolvedSourceMeta>* node_sources);

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_SCHEDULER_JSON_CODEC_H_
