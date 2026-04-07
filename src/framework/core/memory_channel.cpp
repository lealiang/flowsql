/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "memory_channel.h"

namespace flowsql {

namespace {

bool SchemaCompatible(const std::vector<Field>& lhs,
                      const std::vector<Field>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].name != rhs[i].name || lhs[i].type != rhs[i].type) {
            return false;
        }
    }
    return true;
}

}  // namespace

int MemoryChannel::Open() {
    opened_ = true;
    return 0;
}

int MemoryChannel::Close() {
    opened_ = false;
    data_.Clear();
    return 0;
}

int MemoryChannel::Write(IDataFrame* df) {
    if (!opened_ || !df) return -1;
    // 替换语义
    data_.Clear();
    data_.SetSchema(df->GetSchema());
    for (int32_t i = 0; i < df->RowCount(); ++i) {
        data_.AppendRow(df->GetRow(i));
    }
    return 0;
}

int MemoryChannel::Append(IDataFrame* df) {
    if (!opened_ || !df) return -1;

    const std::vector<Field> incoming_schema = df->GetSchema();
    if (incoming_schema.empty()) return 0;

    const std::vector<Field> current_schema = data_.GetSchema();
    if (current_schema.empty()) {
        data_.SetSchema(incoming_schema);
    } else if (!SchemaCompatible(current_schema, incoming_schema)) {
        return -1;
    }

    for (int32_t i = 0; i < df->RowCount(); ++i) {
        if (data_.AppendRow(df->GetRow(i)) != 0) return -1;
    }
    return 0;
}

int MemoryChannel::Read(IDataFrame* df) {
    if (!opened_ || !df) return -1;
    // 快照语义
    df->SetSchema(data_.GetSchema());
    for (int32_t i = 0; i < data_.RowCount(); ++i) {
        df->AppendRow(data_.GetRow(i));
    }
    return 0;
}

}  // namespace flowsql
