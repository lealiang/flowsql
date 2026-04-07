/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "framework/core/sql_text_splitter.h"

#include <cctype>

namespace flowsql {
namespace {

enum class LexState {
    kNormal,
    kSingleQuote,
    kDoubleQuote,
    kBacktick,
    kLineComment,
    kBlockComment,
};

bool IsWhitespace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsWhitespace(text[begin])) ++begin;
    while (end > begin && IsWhitespace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

void SetError(SqlTextSplitError* err, size_t statement_index, const std::string& message) {
    if (!err) return;
    err->statement_index = statement_index;
    err->message = message;
}

}  // namespace

int SplitSqlText(const std::string& sql_text,
                 std::vector<std::string>* statements,
                 SqlTextSplitError* err) {
    if (!statements) return -1;
    statements->clear();
    if (err) {
        err->statement_index = 0;
        err->message.clear();
    }
    if (Trim(sql_text).empty()) {
        SetError(err, 0, "sql_text is empty");
        return -1;
    }

    std::string current;
    current.reserve(sql_text.size());
    bool has_content = false;
    LexState state = LexState::kNormal;

    for (size_t i = 0; i < sql_text.size(); ++i) {
        const char ch = sql_text[i];
        const char next = (i + 1 < sql_text.size()) ? sql_text[i + 1] : '\0';

        switch (state) {
            case LexState::kLineComment:
                current.push_back(ch);
                if (ch == '\n') state = LexState::kNormal;
                continue;
            case LexState::kBlockComment:
                current.push_back(ch);
                if (ch == '*' && next == '/') {
                    current.push_back(next);
                    ++i;
                    state = LexState::kNormal;
                }
                continue;
            case LexState::kSingleQuote:
                current.push_back(ch);
                if (ch == '\'' && next == '\'') {
                    current.push_back(next);
                    ++i;
                    continue;
                }
                if (ch == '\'') state = LexState::kNormal;
                has_content = true;
                continue;
            case LexState::kDoubleQuote:
                current.push_back(ch);
                if (ch == '"' && next == '"') {
                    current.push_back(next);
                    ++i;
                    continue;
                }
                if (ch == '"') state = LexState::kNormal;
                has_content = true;
                continue;
            case LexState::kBacktick:
                current.push_back(ch);
                if (ch == '`' && next == '`') {
                    current.push_back(next);
                    ++i;
                    continue;
                }
                if (ch == '`') state = LexState::kNormal;
                has_content = true;
                continue;
            case LexState::kNormal:
                break;
        }

        if (ch == '-' && next == '-') {
            current.push_back(ch);
            current.push_back(next);
            ++i;
            state = LexState::kLineComment;
            continue;
        }
        if (ch == '/' && next == '*') {
            current.push_back(ch);
            current.push_back(next);
            ++i;
            state = LexState::kBlockComment;
            continue;
        }

        if (ch == '\'') {
            current.push_back(ch);
            state = LexState::kSingleQuote;
            has_content = true;
            continue;
        }
        if (ch == '"') {
            current.push_back(ch);
            state = LexState::kDoubleQuote;
            has_content = true;
            continue;
        }
        if (ch == '`') {
            current.push_back(ch);
            state = LexState::kBacktick;
            has_content = true;
            continue;
        }

        if (ch == ';') {
            if (!has_content) {
                SetError(err, statements->size(), "empty SQL statement");
                return -1;
            }
            const std::string stmt = Trim(current);
            if (stmt.empty()) {
                SetError(err, statements->size(), "empty SQL statement");
                return -1;
            }
            statements->push_back(stmt);
            current.clear();
            has_content = false;
            continue;
        }

        current.push_back(ch);
        if (!IsWhitespace(ch)) has_content = true;
    }

    if (state != LexState::kNormal) {
        SetError(err, statements->size(), "unterminated quote or comment in sql_text");
        return -1;
    }
    if (has_content) {
        const std::string tail = Trim(current);
        if (tail.empty()) {
            SetError(err, statements->size(), "empty SQL statement");
            return -1;
        }
        statements->push_back(tail);
    }
    if (statements->empty()) {
        SetError(err, 0, "sql_text is empty");
        return -1;
    }
    return 0;
}

}  // namespace flowsql
