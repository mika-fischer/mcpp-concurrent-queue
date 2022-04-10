// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <memory>
#include <mutex>

namespace mcpp::concurrent_queue {

template <typename T, typename mutex_type = std::mutex>
class synchronized_value {
  private:
    T value_;
    mutex_type mutex_;

  public:
    synchronized_value() = default;

    template <typename... Args>
    synchronized_value(Args &&...args) : value_(std::forward<Args>(args)...) {}

    class guard_proxy {
      private:
        struct deleter {
            void operator()(synchronized_value *value_ptr) {
                if (value_ptr) {
                    value_ptr->mutex_.unlock();
                }
            }
        };
        std::unique_ptr<synchronized_value, deleter> value_ptr_;

      public:
        explicit guard_proxy(synchronized_value &sv) : value_ptr_(&sv) { value_ptr_->mutex_.lock(); }

        auto operator*() -> T & { return value_ptr_->value_; }
        auto operator->() -> T * { return &value_ptr_->value_; }

        void unlock() { value_ptr_.reset(); }
    };
    static_assert(sizeof(guard_proxy) == sizeof(synchronized_value *));
    auto lock() -> guard_proxy { return guard_proxy(*this); }
};

} // namespace mcpp::concurrent_queue