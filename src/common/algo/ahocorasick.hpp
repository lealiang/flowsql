/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-25 02:39:51
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FAST_BASE_ALGO_AC_HPP_
#define _FAST_BASE_ALGO_AC_HPP_

#include <string.h>
#include "list_base.hpp"
#include "objects_pool.hpp"

namespace flowsql {
namespace ahocorasick {

typedef unsigned char char_t;

struct Userdata {
    void* udata = nullptr;
    Userdata* next = nullptr;
};

struct Pattern {
    char_t* string;
    int32_t length;
    int32_t id;
    int32_t position = -1;
    Userdata* userdata = nullptr;
    Pattern* next = nullptr;  // the pattern with same suffix
};

struct Node {
    int32_t id = 0;
    int32_t depth = 0;           // depth: distance between this node and the root
    Node** outgoing;             // goto table[min_char_, max_char_]
    Node* failure = nullptr;     // failure jump
    Pattern* pattern = nullptr;  // matched pattern
};

class Searcher {
 public:
    void Preprocess(const char* pat, int32_t len) {
        for (int32_t pos = 0; pos < len; ++pos) {
            char_t ch = (char_t)pat[pos];
            if (ch < min_char_) {
                min_char_ = ch;
            } else if (ch > max_char_) {
                max_char_ = ch;
            }
        }
    }

    void Initialize(char_t minchar, char_t maxchar, bool longest_match) {
        min_char_ = minchar;
        max_char_ = maxchar;
        longest_match_ = longest_match;
        root_ = create_node();
    }

    // offset: -1 Unspecified position
    int32_t Patterning(const char* pat, int32_t len, void* userdata, int32_t offset) {
        Node* curnode = root_;
        bool newpattern = false;
        Pattern* patt = nullptr;
        for (int32_t pos = 0; pos < len; ++pos) {
            char_t alpha = (char_t)pat[pos];
            Node* nextnode = curnode->outgoing[alpha - min_char_];
            if (!nextnode) {
                newpattern = true;
                nextnode = create_node();
                nextnode->depth = curnode->depth + 1;
                curnode->outgoing[alpha - min_char_] = nextnode;
            }
            curnode = nextnode;
        }

        if (newpattern) {
            patt = create_pattern(pat, len, userdata, offset);
            curnode->pattern = patt;
        } else {
            patt = find_pattern(curnode->pattern, pat, len, offset);
            if (patt) {
                list::insert_tail(patt->userdata, create_user_data(userdata));
            } else {
                patt = create_pattern(pat, len, userdata, offset);
                if (offset == -1) {
                    list::insert_tail(curnode->pattern, patt);
                } else {
                    list::insert_head(curnode->pattern, patt);
                }
            }
        }

        return patt->id;
    }

    int32_t Compile() {
        char_t alphas[2048];
        traverse_setfailure(root_, alphas);
        return 0;
    }

    int32_t Search(const char* text, int32_t len) {
        Node* finder = root_;
        for (int32_t position = 0; position < len;) {
            char_t alpha = text[position];
            if (alpha < min_char_ || alpha > max_char_) {
                ++position;
                finder = root_;
            } else {
                Node* next = finder->outgoing[text[position] - min_char_];
                if (!next) {
                    if (finder->failure) {
                        finder = finder->failure;
                    } else {
                        ++position;
                    }

                } else {
                    finder = next;
                    ++position;

                    // check output
                    if (finder->pattern) {
                        return finder->pattern->id;
                    }
                }
            }
        }

        return -1;
    }

    /*
        return:
            -1: No hitting, 0: Hitting
        confirm:
            typedef int32_t (*Confirm)(int32_t pattern, int32_t offset)
            return 0: Confirmed, 1: Not confirmed
    */
    template <typename Confirm>
    int32_t Search(const char* text, int32_t len, Confirm confirm) {
        Node* finder = root_;
        int32_t hitting = -1;
        for (int32_t position = 0; position < len;) {
            char_t alpha = text[position];
            if (alpha < min_char_ || alpha > max_char_) {
                ++position;
                finder = root_;
            } else {
                Node* next = finder->outgoing[alpha - min_char_];
                if (!next) {
                    if (finder->failure) {
                        finder = finder->failure;
                    } else {
                        ++position;
                    }

                } else {
                    finder = next;

                    // check output
                    if (finder->pattern) {
                        hitting = check_output(finder, position, confirm);
                        if (0 == hitting) {
                            break;
                        }
                    }
                    ++position;
                }
            }
        }

        // Search out
        return hitting;
    }

 protected:
    /******************************************************************************
     * find failure node for the given node.
     ******************************************************************************/
    void setfailure(Node* node, char_t* alphas) {
        for (int32_t i = 1; i < node->depth; ++i) {
            Node* m = root_;
            for (int32_t j = i; j < node->depth && m; ++j) {
                m = m->outgoing[alphas[j]];
            }

            if (m) {
                node->failure = m;
                break;
            }
        }
        if (!node->failure) {
            node->failure = root_;
        }
    }

    /******************************************************************************
     * Traverse all nodes using DFS (Depth First Search), meanwhile it set
     * the failure node for every node it passes through. this function must be
     * called after adding last pattern to automata. i.e. after calling this you
     * can not add further pattern.
     ******************************************************************************/
    void traverse_setfailure(Node* node, char_t* alphas) {
        for (char_t c = 0; c <= max_char_ - min_char_; ++c) {
            Node* next = node->outgoing[c];
            if (next) {
                alphas[node->depth] = c;

                /* At every node look for its failure node */
                setfailure(next, alphas);

                /* Recursively call itself to traverse all nodes */
                traverse_setfailure(next, alphas);
            }
        }
    }

    inline Node* create_node() {
        Node* node = node_pool_.Alloc();
        node->outgoing = create_goto_table();
        node->id = node_count_++;
        return node;
    }

    inline Pattern* create_pattern(const char* pat, int32_t len, void* userdata, int32_t offset) {
        Pattern* pattern = pattern_pool_.Alloc();
        pattern->id = pattern_count_++;
        pattern->userdata = create_user_data(userdata);
        pattern->next = nullptr;
        pattern->string = copy_string(pat, len);
        pattern->length = len;
        pattern->position = -1;
        if (-1 != offset) {
            pattern->position = offset + len - 1;
        }

        return pattern;
    }

    Pattern* find_pattern(Pattern* head, const char* pat, int32_t len, int32_t offset) {
        for (Pattern* patt = head; patt; patt = patt->next) {
            if (patt->position = offset + len - 1) {
                return patt;
            }
        }
        return nullptr;
    }

    inline Node** create_goto_table() {
        return (Node**)buffer_pool_.Alloc(sizeof(Node*) * (max_char_ - min_char_ + 1));
    }

    inline Userdata* create_user_data(void* udata) {
        Userdata* userdata = (Userdata*)buffer_pool_.Alloc(sizeof(Userdata));
        userdata->udata = udata;
        return userdata;
    }

    inline char_t* copy_string(const char* pat, int32_t len) {
        char_t* str = (char_t*)buffer_pool_.Alloc(len + 1);
        memcpy(str, pat, len);
        str[len] = 0;
        return str;
    }

    template <typename Confirm>
    int32_t check_output(Node* finder, int32_t position, Confirm confirm) {
        int32_t hitting = -1;
        if (longest_match_) {
            for (Pattern* patt = finder->pattern; patt; patt = patt->next) {
                if (patt->position == -1 || patt->position == position) {
                    if (0 == confirm(patt->id, position + 1 - patt->length)) {
                        hitting = 0;
                        break;
                    }
                }
            }
        } else {
            Node* recursion = finder;
            do {
                for (Pattern* patt = recursion->pattern; patt; patt = patt->next) {
                    if (patt->position == -1 || patt->position == position) {
                        if (0 == confirm(patt->id, position + 1 - patt->length)) {
                            hitting = 0;
                            break;
                        }
                    }
                }
            } while (recursion = recursion->failure);
        }

        return hitting;
    }

 protected:
    char_t min_char_ = 255;
    char_t max_char_ = 0;
    int32_t node_count_ = 0;
    int32_t pattern_count_ = 0;
    bool longest_match_ = true;
    Node* root_ = nullptr;
    BufferPool<1024 * 1024> buffer_pool_;
    ObjectsPool<Node, 1024> node_pool_;
    ObjectsPool<Pattern, 1024> pattern_pool_;
};

}  // namespace ahocorasick
}  // namespace flowsql

#endif  //_FAST_BASE_ALGO_AC_HPP_
