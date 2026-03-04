#include "sqlite_driver.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace flowsql {
namespace database {

// ============================================================
// SqliteBatchReader — 流式读取器
// ============================================================
class SqliteBatchReader : public IBatchReader {
 public:
    SqliteBatchReader(sqlite3_stmt* stmt, std::shared_ptr<arrow::Schema> schema)
        : stmt_(stmt), schema_(std::move(schema)) {}

    ~SqliteBatchReader() override {
        if (stmt_) sqlite3_finalize(stmt_);
    }

    int GetSchema(const uint8_t** buf, size_t* len) override {
        if (!schema_) return -1;

        // 序列化 Arrow Schema 为 IPC 格式
        auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
        auto writer = arrow::ipc::MakeStreamWriter(sink, schema_).ValueOrDie();
        // 写入空 batch 以包含 schema
        writer->Close();
        auto buffer = sink->Finish().ValueOrDie();

        schema_buf_.assign(reinterpret_cast<const char*>(buffer->data()), buffer->size());
        *buf = reinterpret_cast<const uint8_t*>(schema_buf_.data());
        *len = schema_buf_.size();
        return 0;
    }

    int Next(const uint8_t** buf, size_t* len) override {
        if (!stmt_ || cancelled_ || done_) return 1;

        // 构建 Arrow builders
        int ncols = schema_->num_fields();
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders(ncols);
        for (int i = 0; i < ncols; ++i) {
            auto status = arrow::MakeBuilder(arrow::default_memory_pool(),
                                             schema_->field(i)->type(), &builders[i]);
            if (!status.ok()) {
                last_error_ = "failed to create builder: " + status.ToString();
                return -1;
            }
        }

        // 读取 batch_size_ 行
        int rows = 0;
        bool done = false;
        while (rows < batch_size_ && !cancelled_) {
            int rc = sqlite3_step(stmt_);
            if (rc == SQLITE_DONE) {
                done = true;
                break;
            }
            if (rc != SQLITE_ROW) {
                last_error_ = sqlite3_errmsg(sqlite3_db_handle(stmt_));
                return -1;
            }

            // 逐列读取值并追加到 builder
            for (int col = 0; col < ncols; ++col) {
                int col_type = sqlite3_column_type(stmt_, col);
                auto* builder = builders[col].get();
                auto arrow_type = schema_->field(col)->type();

                if (col_type == SQLITE_NULL) {
                    builder->AppendNull();
                    continue;
                }

                // 根据 Arrow Schema 类型写入
                if (arrow_type->id() == arrow::Type::INT64) {
                    static_cast<arrow::Int64Builder*>(builder)->Append(
                        sqlite3_column_int64(stmt_, col));
                } else if (arrow_type->id() == arrow::Type::DOUBLE) {
                    static_cast<arrow::DoubleBuilder*>(builder)->Append(
                        sqlite3_column_double(stmt_, col));
                } else if (arrow_type->id() == arrow::Type::STRING) {
                    auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
                    static_cast<arrow::StringBuilder*>(builder)->Append(text ? text : "");
                } else if (arrow_type->id() == arrow::Type::BINARY) {
                    auto blob = sqlite3_column_blob(stmt_, col);
                    int blob_len = sqlite3_column_bytes(stmt_, col);
                    static_cast<arrow::BinaryBuilder*>(builder)->Append(
                        reinterpret_cast<const uint8_t*>(blob), blob_len);
                } else {
                    // 默认当字符串处理
                    auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
                    static_cast<arrow::StringBuilder*>(builder)->Append(text ? text : "");
                }
            }
            ++rows;
        }

        // 没有数据且已读完
        if (rows == 0 && done) {
            done_ = true;
            return 1;
        }

        // 标记已读完（本批有数据但 statement 已结束）
        if (done) done_ = true;

        // 构建 RecordBatch
        std::vector<std::shared_ptr<arrow::Array>> arrays(ncols);
        for (int i = 0; i < ncols; ++i) {
            auto result = builders[i]->Finish();
            if (!result.ok()) {
                last_error_ = "failed to finish array: " + result.status().ToString();
                return -1;
            }
            arrays[i] = *result;
        }
        auto batch = arrow::RecordBatch::Make(schema_, rows, arrays);

        // 序列化为 IPC stream 格式
        auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
        auto writer = arrow::ipc::MakeStreamWriter(sink, schema_).ValueOrDie();
        auto status = writer->WriteRecordBatch(*batch);
        if (!status.ok()) {
            last_error_ = "IPC serialize failed: " + status.ToString();
            writer->Close();  // 失败路径也需关闭 writer，释放底层资源
            return -1;
        }
        writer->Close();
        auto buffer = sink->Finish().ValueOrDie();

        batch_buf_.assign(reinterpret_cast<const char*>(buffer->data()), buffer->size());
        *buf = reinterpret_cast<const uint8_t*>(batch_buf_.data());
        *len = batch_buf_.size();
        return 0;
    }

    void Cancel() override { cancelled_ = true; }
    void Close() override {
        if (stmt_) {
            sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
    }
    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

 private:
    sqlite3_stmt* stmt_ = nullptr;
    std::shared_ptr<arrow::Schema> schema_;
    std::string schema_buf_;
    std::string batch_buf_;
    std::string last_error_;
    bool cancelled_ = false;
    bool done_ = false;
    int batch_size_ = 1024;
};

// ============================================================
// SqliteBatchWriter — 批量写入器
// ============================================================
class SqliteBatchWriter : public IBatchWriter {
 public:
    SqliteBatchWriter(sqlite3* db, const std::string& table)
        : db_(db), table_(table), start_time_(std::chrono::steady_clock::now()) {}

    ~SqliteBatchWriter() override = default;

    int Write(const uint8_t* buf, size_t len) override {
        if (!db_) {
            last_error_ = "database not connected";
            return -1;
        }

        // 反序列化 Arrow IPC stream → RecordBatch
        auto arrow_buf = arrow::Buffer::Wrap(buf, static_cast<int64_t>(len));
        auto input = std::make_shared<arrow::io::BufferReader>(arrow_buf);
        auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(input);
        if (!stream_result.ok()) {
            last_error_ = "IPC deserialize failed: " + stream_result.status().ToString();
            return -1;
        }
        auto stream_reader = *stream_result;

        std::shared_ptr<arrow::RecordBatch> batch;
        while (stream_reader->ReadNext(&batch).ok() && batch) {
            // 首次写入时自动建表
            if (!table_created_) {
                if (CreateTable(*batch->schema()) != 0) return -1;
                table_created_ = true;
            }
            if (InsertBatch(*batch) != 0) return -1;
        }
        return 0;
    }

    int Flush() override { return 0; }

    void Close(BatchWriteStats* stats) override {
        if (stats) {
            auto end = std::chrono::steady_clock::now();
            stats->rows_written = rows_written_;
            stats->bytes_written = bytes_written_;
            stats->elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    end - start_time_).count();
        }
    }

    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

 private:
    // Arrow 类型 → SQLite 列类型字符串
    static std::string ArrowTypeToSqlite(const std::shared_ptr<arrow::DataType>& type) {
        switch (type->id()) {
            case arrow::Type::INT8:
            case arrow::Type::INT16:
            case arrow::Type::INT32:
            case arrow::Type::INT64:
            case arrow::Type::UINT8:
            case arrow::Type::UINT16:
            case arrow::Type::UINT32:
            case arrow::Type::UINT64:
            case arrow::Type::BOOL:
                return "INTEGER";
            case arrow::Type::FLOAT:
            case arrow::Type::DOUBLE:
                return "REAL";
            case arrow::Type::STRING:
                return "TEXT";
            case arrow::Type::BINARY:
                return "BLOB";
            default:
                return "TEXT";
        }
    }

    int CreateTable(const arrow::Schema& schema) {
        std::string sql = "CREATE TABLE IF NOT EXISTS " + table_ + " (";
        for (int i = 0; i < schema.num_fields(); ++i) {
            if (i > 0) sql += ", ";
            sql += schema.field(i)->name() + " " + ArrowTypeToSqlite(schema.field(i)->type());
        }
        sql += ")";

        char* errmsg = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            last_error_ = errmsg ? errmsg : "CREATE TABLE failed";
            if (errmsg) sqlite3_free(errmsg);
            return -1;
        }
        return 0;
    }

    int InsertBatch(const arrow::RecordBatch& batch) {
        int ncols = batch.num_columns();
        int nrows = batch.num_rows();
        if (nrows == 0) return 0;

        // 构建 INSERT 语句
        std::string sql = "INSERT INTO " + table_ + " (";
        std::string placeholders = "(";
        for (int i = 0; i < ncols; ++i) {
            if (i > 0) { sql += ", "; placeholders += ", "; }
            sql += batch.schema()->field(i)->name();
            placeholders += "?";
        }
        sql += ") VALUES " + placeholders + ")";

        // BEGIN 事务
        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return -1;
        }

        // 逐行绑定参数并执行
        for (int row = 0; row < nrows; ++row) {
            sqlite3_reset(stmt);
            for (int col = 0; col < ncols; ++col) {
                auto array = batch.column(col);
                if (array->IsNull(row)) {
                    sqlite3_bind_null(stmt, col + 1);
                    continue;
                }
                if (BindValue(stmt, col + 1, array, row) != SQLITE_OK) {
                    last_error_ = sqlite3_errmsg(db_);
                    sqlite3_finalize(stmt);
                    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                    return -1;
                }
            }
            rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                last_error_ = sqlite3_errmsg(db_);
                sqlite3_finalize(stmt);
                sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                return -1;
            }
        }

        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

        rows_written_ += nrows;
        return 0;
    }

    // 绑定 Arrow 列值到 SQLite 参数，返回 SQLITE_OK 或错误码
    static int BindValue(sqlite3_stmt* stmt, int idx,
                         const std::shared_ptr<arrow::Array>& array, int row) {
        int rc = SQLITE_OK;
        switch (array->type_id()) {
            case arrow::Type::INT8:
                rc = sqlite3_bind_int64(stmt, idx, std::static_pointer_cast<arrow::Int8Array>(array)->Value(row));
                break;
            case arrow::Type::INT16:
                rc = sqlite3_bind_int64(stmt, idx, std::static_pointer_cast<arrow::Int16Array>(array)->Value(row));
                break;
            case arrow::Type::INT32:
                rc = sqlite3_bind_int64(stmt, idx, std::static_pointer_cast<arrow::Int32Array>(array)->Value(row));
                break;
            case arrow::Type::INT64:
                rc = sqlite3_bind_int64(stmt, idx, std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
                break;
            case arrow::Type::UINT8:
                rc = sqlite3_bind_int64(stmt, idx, std::static_pointer_cast<arrow::UInt8Array>(array)->Value(row));
                break;
            case arrow::Type::UINT16:
                rc = sqlite3_bind_int64(stmt, idx, std::static_pointer_cast<arrow::UInt16Array>(array)->Value(row));
                break;
            case arrow::Type::UINT32:
                rc = sqlite3_bind_int64(stmt, idx, std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row));
                break;
            case arrow::Type::UINT64:
                rc = sqlite3_bind_int64(stmt, idx, static_cast<int64_t>(
                    std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row)));
                break;
            case arrow::Type::FLOAT:
                rc = sqlite3_bind_double(stmt, idx, std::static_pointer_cast<arrow::FloatArray>(array)->Value(row));
                break;
            case arrow::Type::DOUBLE:
                rc = sqlite3_bind_double(stmt, idx, std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row));
                break;
            case arrow::Type::BOOL:
                rc = sqlite3_bind_int(stmt, idx,
                    std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row) ? 1 : 0);
                break;
            case arrow::Type::STRING: {
                auto view = std::static_pointer_cast<arrow::StringArray>(array)->GetView(row);
                rc = sqlite3_bind_text(stmt, idx, view.data(), static_cast<int>(view.size()),
                                       SQLITE_TRANSIENT);
                break;
            }
            case arrow::Type::BINARY: {
                auto view = std::static_pointer_cast<arrow::BinaryArray>(array)->GetView(row);
                rc = sqlite3_bind_blob(stmt, idx, view.data(), static_cast<int>(view.size()),
                                       SQLITE_TRANSIENT);
                break;
            }
            default:
                rc = sqlite3_bind_null(stmt, idx);
                break;
        }
        return rc;
    }

    sqlite3* db_ = nullptr;
    std::string table_;
    std::string last_error_;
    int64_t rows_written_ = 0;
    int64_t bytes_written_ = 0;
    std::chrono::steady_clock::time_point start_time_;
    bool table_created_ = false;
};

// ============================================================
// SqliteDriver 实现
// ============================================================
SqliteDriver::~SqliteDriver() {
    if (db_) Disconnect();
}

int SqliteDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    if (db_) return 0;  // 已连接

    auto it = params.find("path");
    db_path_ = (it != params.end()) ? it->second : ":memory:";

    // 使用 FULLMUTEX 模式，保证多线程安全
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;

    // 支持只读模式
    auto ro_it = params.find("readonly");
    if (ro_it != params.end() && ro_it->second == "true") {
        flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
        readonly_ = true;
    }

    int rc = sqlite3_open_v2(db_path_.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return -1;
    }

    // 开启 WAL 模式，提升并发读写性能
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    printf("SqliteDriver: connected to %s\n", db_path_.c_str());
    return 0;
}

int SqliteDriver::Disconnect() {
    if (!db_) return 0;
    sqlite3_close(db_);
    db_ = nullptr;
    printf("SqliteDriver: disconnected from %s\n", db_path_.c_str());
    return 0;
}

int SqliteDriver::CreateReader(const char* query, IBatchReader** reader) {
    if (!db_ || !query || !reader) return -1;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, query, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return -1;
    }

    // 推断 Schema：纯粹使用 sqlite3_column_decltype，不 step+reset
    // 避免 sqlite3_reset 后带 WHERE 的查询返回空结果集的 bug
    int ncols = sqlite3_column_count(stmt);
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (int i = 0; i < ncols; ++i) {
        std::string col_name = sqlite3_column_name(stmt, i);
        std::shared_ptr<arrow::DataType> arrow_type;

        const char* decl = sqlite3_column_decltype(stmt, i);
        if (decl) {
            std::string dtype(decl);
            // 转大写比较
            for (auto& c : dtype) c = std::toupper(c);
            if (dtype.find("INT") != std::string::npos) arrow_type = arrow::int64();
            else if (dtype.find("REAL") != std::string::npos ||
                     dtype.find("FLOAT") != std::string::npos ||
                     dtype.find("DOUBLE") != std::string::npos) arrow_type = arrow::float64();
            else if (dtype.find("BLOB") != std::string::npos) arrow_type = arrow::binary();
            else arrow_type = arrow::utf8();
        } else {
            // decltype 为空（表达式列等），默认 utf8
            arrow_type = arrow::utf8();
        }
        fields.push_back(arrow::field(col_name, arrow_type));
    }

    auto schema = arrow::schema(fields);
    *reader = new SqliteBatchReader(stmt, schema);
    return 0;
}

int SqliteDriver::CreateWriter(const char* table, IBatchWriter** writer) {
    if (!db_ || !table || !writer) return -1;

    if (readonly_) {
        last_error_ = "cannot write to readonly database";
        return -1;
    }

    *writer = new SqliteBatchWriter(db_, table);
    return 0;
}

}  // namespace database
}  // namespace flowsql
