// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <atomic>

namespace mcpp::concurrent_queue {

class spinlock {
  private:
    static constexpr bool LOCKED = true;
    static constexpr bool UNLOCKED = false;

    std::atomic<bool> flag_{false};

  public:
    void lock() noexcept {
        while (flag_.exchange(LOCKED, std::memory_order_acquire) != UNLOCKED) {
            do {
                // spin without writing while locked
            } while (flag_.load(std::memory_order_relaxed) == LOCKED);
        }
    }

    void unlock() noexcept { flag_.store(UNLOCKED, std::memory_order_release); }
};

} // namespace mcpp::concurrent_queue