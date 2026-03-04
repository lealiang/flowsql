#include "sql_parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace flowsql {

void SqlParser::SkipWhitespace() {
    while (pos_ < end_ && std::isspace(*pos_)) ++pos_;
}

bool SqlParser::MatchKeyword(const char* keyword) {
    SkipWhitespace();
    size_t len = strlen(keyword);
    if (pos_ + len > end_) return false;

    // 大小写不敏感匹配
    for (size_t i = 0; i < len; ++i) {
        if (std::toupper(pos_[i]) != std::toupper(keyword[i])) return false;
    }
    // 关键字后必须是空白或结尾
    if (pos_ + len < end_ && !std::isspace(pos_[len]) && pos_[len] != '\0') return false;

    pos_ += len;
    return true;
}

// 读取标识符：字母、数字、下划线、点、连字符
std::string SqlParser::ReadIdentifier() {
    SkipWhitespace();
    const char* start = pos_;
    while (pos_ < end_ && (std::isalnum(*pos_) || *pos_ == '_' || *pos_ == '.' || *pos_ == '-')) {
        ++pos_;
    }
    return std::string(start, pos_);
}

// 读取值：支持带引号的字符串或普通标识符
std::string SqlParser::ReadValue() {
    SkipWhitespace();
    if (pos_ < end_ && (*pos_ == '"' || *pos_ == '\'')) {
        char quote = *pos_++;
        const char* start = pos_;
        while (pos_ < end_ && *pos_ != quote) ++pos_;
        std::string val(start, pos_);
        if (pos_ < end_) ++pos_;  // 跳过结束引号
        return val;
    }
    // 无引号：读到逗号、空白或结尾
    const char* start = pos_;
    while (pos_ < end_ && !std::isspace(*pos_) && *pos_ != ',') ++pos_;
    return std::string(start, pos_);
}

SqlStatement SqlParser::Parse(const std::string& sql) {
    SqlStatement stmt;
    pos_ = sql.c_str();
    end_ = pos_ + sql.size();

    // SELECT
    if (!MatchKeyword("SELECT")) {
        stmt.error = "expected SELECT";
        return stmt;
    }

    // 列选择：* 或 col1, col2, ...
    SkipWhitespace();
    if (pos_ < end_ && *pos_ == '*') {
        ++pos_;
        // columns 为空表示 SELECT *
    } else {
        // 读取列名列表
        while (true) {
            std::string col = ReadIdentifier();
            if (col.empty()) {
                stmt.error = "expected column name or * after SELECT";
                return stmt;
            }
            stmt.columns.push_back(col);

            SkipWhitespace();
            if (pos_ < end_ && *pos_ == ',') {
                ++pos_;  // 跳过逗号
            } else {
                break;
            }
        }
    }

    // FROM <source>
    if (!MatchKeyword("FROM")) {
        stmt.error = "expected FROM";
        return stmt;
    }
    stmt.source = ReadIdentifier();
    if (stmt.source.empty()) {
        stmt.error = "expected source channel name after FROM";
        return stmt;
    }

    // [WHERE <condition>]（可选）
    // WHERE 子句读取到下一个关键字（USING/WITH/INTO）或结尾
    {
        const char* saved = pos_;
        if (MatchKeyword("WHERE")) {
            SkipWhitespace();
            const char* where_start = pos_;
            // 向前扫描直到遇到 USING/WITH/INTO 关键字或结尾
            while (pos_ < end_) {
                const char* check = pos_;
                SkipWhitespace();
                if (MatchKeyword("USING") || MatchKeyword("WITH") || MatchKeyword("INTO")) {
                    pos_ = check;  // 回退到关键字之前
                    break;
                }
                pos_ = check;
                if (pos_ < end_) ++pos_;
            }
            // 去除尾部空白
            const char* where_end = pos_;
            while (where_end > where_start && std::isspace(*(where_end - 1))) --where_end;
            stmt.where_clause = std::string(where_start, where_end);

            if (stmt.where_clause.empty()) {
                stmt.error = "expected condition after WHERE";
                return stmt;
            }

            // 安全验证
            if (!ValidateWhereClause(stmt.where_clause)) {
                stmt.error = "WHERE clause contains forbidden keywords: " + stmt.where_clause;
                return stmt;
            }
        } else {
            pos_ = saved;
        }
    }

    // [USING <catelog.name>]（可选）
    const char* saved_pos = pos_;
    if (MatchKeyword("USING")) {
        std::string op_full = ReadIdentifier();
        auto dot = op_full.find('.');
        if (dot == std::string::npos || dot == 0 || dot == op_full.size() - 1) {
            stmt.error = "expected catelog.name format after USING, got: " + op_full;
            return stmt;
        }
        stmt.op_catelog = op_full.substr(0, dot);
        stmt.op_name = op_full.substr(dot + 1);
    } else {
        pos_ = saved_pos;  // 回退，USING 不存在
    }

    // [WITH key=val, ...]
    if (MatchKeyword("WITH")) {
        while (true) {
            std::string key = ReadIdentifier();
            if (key.empty()) break;

            SkipWhitespace();
            if (pos_ >= end_ || *pos_ != '=') {
                stmt.error = "expected = after WITH key: " + key;
                return stmt;
            }
            ++pos_;  // 跳过 =

            std::string val = ReadValue();
            stmt.with_params[key] = val;

            SkipWhitespace();
            if (pos_ < end_ && *pos_ == ',') {
                ++pos_;  // 跳过逗号
            } else {
                break;
            }
        }
    }

    // [INTO <dest>]
    if (MatchKeyword("INTO")) {
        stmt.dest = ReadIdentifier();
        if (stmt.dest.empty()) {
            stmt.error = "expected destination channel name after INTO";
            return stmt;
        }
    }

    return stmt;
}

// 验证 WHERE 子句安全性
// 拒绝包含 DDL/DML 危险关键字的条件
bool SqlParser::ValidateWhereClause(const std::string& clause) {
    // 转为大写进行检查
    std::string upper = clause;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // 危险关键字列表
    static const char* forbidden[] = {
        "DROP", "DELETE", "INSERT", "UPDATE", "ALTER", "CREATE",
        "EXEC", "EXECUTE", "TRUNCATE", "GRANT", "REVOKE",
        "--", "/*", "*/", ";",
    };

    for (const char* kw : forbidden) {
        std::string keyword(kw);
        auto pos = upper.find(keyword);
        if (pos != std::string::npos) {
            // 对于字母关键字，确保是独立单词（前后不是字母数字下划线）
            if (std::isalpha(keyword[0])) {
                bool word_start = (pos == 0 || !std::isalnum(upper[pos - 1]));
                bool word_end = (pos + keyword.size() >= upper.size() ||
                                 !std::isalnum(upper[pos + keyword.size()]));
                if (word_start && word_end) return false;
            } else {
                return false;  // 符号类直接拒绝
            }
        }
    }
    return true;
}

}  // namespace flowsql
