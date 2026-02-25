#ifndef _FAST_OPTION_LOADER_H_
#define _FAST_OPTION_LOADER_H_

#include <common/logger_helper.h>
#include <common/singleton.h>

#include <json_struct.h>

namespace flowsql {

template <typename T>
class option_loader : public singleton<option_loader<T>> {
 private:
 public:
    option_loader() {}
    ~option_loader() {}

    bool load(const char* option) {
        JS::ParseContext ctx(option);
        JS::Error e = ctx.parseTo(this->opt_);
        if (JS::Error::NoError == e) {
            return true;
        } else {
            LOG_E() << "option_loader error: " << option;
            return false;
        }
    }

    T& opt() { return opt_; }
    const T& opt() const { return opt_; }

 protected:
    T opt_;
};

}  // namespace flowsql

#endif
