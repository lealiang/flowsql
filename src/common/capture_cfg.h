/*
 * Author       : kun.dong
 * Copyright (C) 2020-06 - FAST
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Date         : 2020-10-27 10:04:30
 * LastEditors  : kun.dong
 * LastEditTime : 2021-03-16 10:08:30
 */
#ifndef _FAST_CAPTURE_CFG_H_
#define _FAST_CAPTURE_CFG_H_

#include <common/file_util.h>
#include <common/logger_helper.h>
#include <common/path_util.h>
#include <common/special_path.h>
#include <common/variable.h>
#include <common/variable_ar.h>
#include <fast/define.h>

#include <map>
#include <string>
#include <vector>

namespace flowsql {

class capture_cfg {
 private:
 public:
    capture_cfg() {}
    ~capture_cfg() {}

    struct info_t {
        std::string name_;
        bool capture_;
    };
    typedef std::map<std::string, info_t> adpater_list_type;

    bool load() {
        bool load_result = false;
        capture_list_.clear();

        variable capture_cfg;
        std::string capt_str;
        auto capt_path = special_path::get_capture_info_path();

        do {
            if (!file_util::is_file_exists(capt_path.c_str())) {
                LOG_I() << capt_path << " not exist";
                break;
            }

            file_util::get_file_content(capt_path, capt_str);

            if (!from_json(capt_str, capture_cfg)) {
                LOG_W() << "parse json content error: " << capt_str;
                break;
            }

            if (!capture_cfg.is_array()) {
                LOG_I() << "no adapter to capture";
                break;
            }

            for (auto itr = capture_cfg.array_.begin(); itr != capture_cfg.array_.end(); ++itr) {
                variable& v = *itr;
                info_t info;
                info.name_ = v[kAdapter].to_string();
                info.capture_ = v[kCapture].to_bool();
                capture_list_[info.name_] = info;
            }

            load_result = true;

            if (capture_list_.size() == 0) {
                LOG_I() << "no adapter capture configured!";
            }

        } while (false);

        return load_result;
    }

    const adpater_list_type& get_cap_list() const { return this->capture_list_; }

    uint32_t get_capture_count() const {
        uint32_t counter = 0;
        for (auto& item : this->capture_list_) {
            if (item.second.capture_) {
                ++counter;
            }
        }
        return counter;
    }

    bool is_capture(const std::string& name) const {
        auto itr = this->capture_list_.find(name);
        if (itr != this->capture_list_.end()) {
            return itr->second.capture_;
        }
        return false;
    }

    void save(const std::vector<std::string>& cap_list) {
        variable capture_cfg;
        capture_cfg.set_type(e_type_array);
        for (auto itr = cap_list.begin(); itr != cap_list.end(); ++itr) {
            variable t;
            t[kAdapter] = *itr;
            t[kCapture] = true;
            capture_cfg.emplace_back(t);
        }

        auto capt_path = special_path::get_capture_info_path();
        auto str_cfg = to_json(capture_cfg);
        file_util::write_file(capt_path, (uint8_t*)str_cfg.c_str(), str_cfg.size());
    }

 protected:
    // std::vector<std::string> capture_list_;
    std::map<std::string, info_t> capture_list_;
};

}  // namespace flowsql

#endif