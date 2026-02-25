/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-11 14:10:29
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_COMMON_NIC_MGR_H_
#define _FLOWSQL_COMMON_NIC_MGR_H_

#include "define.h"

#include <common/file_util.h>
#include <common/logger_helper.h>
#include <common/singleton.h>
#include <common/special_path.h>
#include <common/variable_ar.h>

#include <map>

#include <functional>
#include <string>
#include <vector>

namespace flowsql {

struct NIC_cfg_t {
    std::string name;    // 网卡名字
    int index;           // 网卡的索引标识
    std::string pci;     // 网卡的pci信息
    std::string speed;   // 网卡的网速
    std::string driver;  // 驱动
};

struct if_conf_t {
    std::vector<NIC_cfg_t> ifs_;
};

class nic_mgr : public singleton<nic_mgr> {
 public:
    bool load() {
        auto path = special_path::get_if_config_path();
        if (!path_util::is_exists(path)) {
            LOG_E() << "nic path not exist: " << path;
            return false;
        }

        std::string content;
        file_util::get_file_content(path, content);
        return load(content.c_str());
    }

    bool load(const char* option) {
        this->nic_map_.clear();

        if (nullptr == option) {
            return false;
        }

        variable var;
        if (!from_json(option, var)) {
            LOG_E() << "load nic config error: " << option;
            return false;
        }

        auto& ifs = var[kIfs];
        if (!ifs.is_array()) {
            LOG_E() << "cfg miss ifs";
            return false;
        }

        for (auto& item : ifs.array_) {
            NIC_cfg_t nic;
            nic.index = item[kIndex].to_int64();
            nic.name = item[kName].to_string();
            nic.pci = item[kPci].to_string();
            nic.speed = item[kSpeed].to_string();
            nic.driver = item[kDriver].to_string();

            auto pos = nic.pci.find(":");
            if (pos == std::string::npos) {
                nic.pci.clear();
            }

            LOG_D() << "nic item (" << nic.name << " -- " << nic.index << ")";
            this->nic_map_[nic.name] = nic;
            assert(this->nic_map_.size() > 0);
        }

        is_loaded_ = true;
        return this->nic_map_.size() > 0;
    }

    int find_index(const std::string& if_name) {
        assert(is_loaded_);
        auto itr = nic_map_.find(if_name);
        if (itr == nic_map_.end()) {
            return -1;
        }
        return itr->second.index;
    }

    bool find_nic_by_ifname(const std::string& if_name, NIC_cfg_t& nic) {
        assert(is_loaded_);
        auto itr = nic_map_.find(if_name);
        if (itr == nic_map_.end()) {
            return false;
        }
        nic = itr->second;
        return true;
    }

    bool find_nic_by_pci(const std::string& pci_name, NIC_cfg_t& nic) {
        assert(is_loaded_);
        for (auto& n : this->nic_map_) {
            if (n.second.pci == pci_name) {
                nic = n.second;
                return true;
            }
        }
        return false;
    }

    void foreach (const std::function<void(const NIC_cfg_t& nic)>& f) {
        for (auto& adap : this->nic_map_) {
            f(adap.second);
        }
    }

    bool empty() const { return this->nic_map_.empty(); }

    void clear() { this->nic_map_.clear(); }

 protected:
    bool is_loaded_;
    std::map<std::string, NIC_cfg_t> nic_map_;
};

class nic_storer {
 public:
    void add_adapter_info(const std::string& name, const std::string& pci, const std::string& speed,
                          const std::string& driver, int index) {
        NIC_cfg_t nic;
        nic.name = name;
        nic.pci = pci;
        nic.speed = speed;
        nic.driver = driver;
        nic.index = index;

        ifcfg_.ifs_.emplace_back(nic);
    }

    bool save(const std::string& cfg_path) {
        variable var;
        auto& ifs = var[kIfs];

        for (auto& item : this->ifcfg_.ifs_) {
            variable v;
            v[kName] = item.name;
            v[kIndex] = item.index;
            v[kPci] = item.pci;
            v[kDriver] = item.driver;
            v[kSpeed] = item.speed;

            ifs.emplace_back(v);
        }

        auto str = to_json(var);
        LOG_D() << "save nic content: " << str;
        return file_util::write_file(cfg_path, (uint8_t*)str.c_str(), str.size());
    }

 protected:
    if_conf_t ifcfg_;
};

}  // namespace flowsql

#endif