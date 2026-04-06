#ifndef _FLOWSQL_FRAMEWORK_CORE_SQL_TEXT_SPLITTER_H_
#define _FLOWSQL_FRAMEWORK_CORE_SQL_TEXT_SPLITTER_H_

#include <string>
#include <vector>

namespace flowsql {

struct SqlTextSplitError {
    size_t statement_index = 0;  // 0-based index
    std::string message;
};

// Split `sql_text` by semicolon in lexical-normal state.
// Returns 0 on success; non-zero on invalid sql_text.
int SplitSqlText(const std::string& sql_text,
                 std::vector<std::string>* statements,
                 SqlTextSplitError* err = nullptr);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_SQL_TEXT_SPLITTER_H_
