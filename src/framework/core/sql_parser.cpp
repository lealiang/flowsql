#include "sql_parser.h"

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

    // *
    SkipWhitespace();
    if (pos_ >= end_ || *pos_ != '*') {
        stmt.error = "expected * after SELECT";
        return stmt;
    }
    ++pos_;

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

    // USING <catelog.name>
    if (!MatchKeyword("USING")) {
        stmt.error = "expected USING";
        return stmt;
    }
    std::string op_full = ReadIdentifier();
    auto dot = op_full.find('.');
    if (dot == std::string::npos || dot == 0 || dot == op_full.size() - 1) {
        stmt.error = "expected catelog.name format after USING, got: " + op_full;
        return stmt;
    }
    stmt.op_catelog = op_full.substr(0, dot);
    stmt.op_name = op_full.substr(dot + 1);

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

}  // namespace flowsql
