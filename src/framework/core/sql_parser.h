#ifndef _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_
#define _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_

#include <string>
#include <unordered_map>

namespace flowsql {

// SQL 解析结果
struct SqlStatement {
    std::string source;       // FROM 后的源通道名
    std::string op_catelog;   // USING 后的算子 catelog
    std::string op_name;      // USING 后的算子 name
    std::unordered_map<std::string, std::string> with_params;  // WITH key=val,...
    std::string dest;         // INTO 后的目标通道名（可选，空表示直接返回结果）
    std::string error;        // 解析错误信息（空表示成功）
};

// 递归下降 SQL 解析器
// 语法：SELECT * FROM <source> USING <catelog.name> [WITH key=val,...] [INTO <dest>]
class SqlParser {
 public:
    SqlStatement Parse(const std::string& sql);

 private:
    // 词法辅助
    void SkipWhitespace();
    bool MatchKeyword(const char* keyword);
    std::string ReadIdentifier();
    std::string ReadValue();

    const char* pos_ = nullptr;
    const char* end_ = nullptr;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_
