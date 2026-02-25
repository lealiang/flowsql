/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-10-08 17:21:40
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef SRC_0_2_FAST_IQUERIER_HPP_
#define SRC_0_2_FAST_IQUERIER_HPP_
#include <common/guid.h>
#include <common/typedef.h>
#include <exception>
#include <sstream>

namespace flowsql {
interface IQuerier {
    typedef int (*fntraverse)(void*);
    virtual int Traverse(const Guid& iid, fntraverse proc) = 0;
    virtual void* First(const Guid& iid) = 0;
};

class IQuerierGetter {
 public:
    static IQuerier* getiquerier() {
        static IQuerierGetter _iqueriergetter;
        return _iqueriergetter.get();
    }

 private:
    typedef IQuerier* (*fngetiquerier)();
    fngetiquerier fngetiquerier_ = nullptr;
    IQuerierGetter() {
        fngetiquerier_ = (fngetiquerier)getprocaddress(nullptr, "getiquerier");
        if (!fngetiquerier_) {
            printf("getprocaddress of getiquerier faild!\n");
        }
    }

    IQuerier* get() {
        if (fngetiquerier_) return fngetiquerier_();
        return nullptr;
    }
};

/* sample
flowsql::IQuerier* iquerier = flowsql::IQuerierGetter::getiquerier();
ILog* _ilog = reinterpret_cast<ILog*>(iquerier->First(flowsql::IID_LOG));
*/
class QuerierGuard {
 public:
    static QuerierGuard First(const Guid& iid) {
        IQuerier* iquerier = IQuerierGetter::getiquerier();
        void* inter = iquerier->First(iid);
        if (!inter) {
            std::stringstream ss_except;
            ss_except << iid << " interface not found!";
            throw std::runtime_error(ss_except.str());
        }
        return QuerierGuard(inter);
    }

    template <typename T>
    operator T*() {
        return reinterpret_cast<T*>(inter_);
    }

 private:
    explicit QuerierGuard(void* inter) : inter_(inter) {}
    void* inter_ = nullptr;
};

/* sample
try {
ILog* _ilog = QuerierGuard::First(flowsql::IID_LOG);
}catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return -1;
}
*/

}  // namespace flowsql

#endif  // SRC_0_2_FAST_IQUERIER_HPP_
