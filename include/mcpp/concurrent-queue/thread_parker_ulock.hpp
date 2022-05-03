// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

// #include <sys/ulock.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <exception>
#include <optional>
#include <sys/errno.h>

#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_THREAD 0x00000200
#define ULF_NO_ERRNO 0x01000000

extern "C" {
extern auto __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us) -> int;
extern auto __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value) -> int;
}

namespace mcpp::concurrent_queue {

class thread_parker_ulock {
  private:
    enum class state : int { resumed, suspended, notified };
    std::atomic<state> state_{state::resumed};

  public:
    static auto this_thread_parker() noexcept -> thread_parker_ulock & {
        thread_local thread_parker_ulock parker;
        return parker;
    }

    auto resumed() -> bool { return state_.load() == state::resumed; }

    void resume() noexcept {
        auto old_state = state_.exchange(state::notified);
        switch (old_state) {
            case state::suspended:
                while (true) {
                    auto res = __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &state_, 0u);
                    if (res >= 0) {
                        return;
                    }
                    switch (-res) {
                        case EINTR:
                        case EFAULT:
                            continue;
                        case ENOENT:
                            return;
                        default:
                            assert(false);
                            std::terminate();
                    }
                }
            case state::resumed:
                // Already resumed (for instance due to timeout)
                return;
            case state::notified:
            default:
                // Someone else already notified, not supported!
                assert(false);
                std::terminate();
        }
    }

    template <typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto suspend(std::optional<std::chrono::time_point<Clock, Duration>> deadline) -> bool {
        const auto try_resume = [this] { return try_state_change(state::notified, state::resumed); };

        // check if already notified
        if (try_resume()) {
            return true;
        }

        if (try_state_change(state::resumed, state::suspended)) {
            do {
                uint32_t timeout_us = 0;
                if (deadline) {
                    timeout_us =
                        std::chrono::duration_cast<std::chrono::microseconds>(deadline.value() - Clock::now()).count();
                }
                int res = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &state_,
                                       static_cast<uint64_t>(state::suspended), timeout_us);
                if (res >= 0) {
                    if (try_resume()) {
                        return true;
                    }
                    continue;
                }
                switch (-res) {
                    case EINTR:
                    case EFAULT:
                        continue;
                    case ETIMEDOUT:
                        return false;
                    default:
                        assert(false);
                        std::terminate();
                }
            } while (state_.load() == state::suspended);
        }

        // timeout or suspend failed, set back to resumed state
        switch (state_.exchange(state::resumed)) {
            case state::notified:
                return true;
            case state::suspended:
                return false;
            case state::resumed:
            default:
                // should never happen
                assert(false);
                std::terminate();
        }
    }

    auto suspend() -> bool { return suspend({}); }

    template <typename Clock, typename Duration>
    auto suspend(std::chrono::time_point<Clock, Duration> deadline) -> bool {
        return suspend({deadline});
    }

    template <typename Rep, typename Period>
    auto suspend(std::chrono::duration<Rep, Period> duration) -> bool {
        return suspend({std::chrono::steady_clock::now() + duration});
    }

    template <typename Clock, typename Duration>
    auto suspend_until(std::chrono::time_point<Clock, Duration> deadline) -> bool {
        return suspend(deadline);
    }

    template <typename Rep, typename Period>
    auto suspend_for(std::chrono::duration<Rep, Period> duration) -> bool {
        return suspend({std::chrono::steady_clock::now() + duration});
    }

  private:
    auto try_state_change(state old_state, state new_state) -> bool {
        return state_.compare_exchange_strong(old_state, new_state);
    };
};

} // namespace mcpp::concurrent_queue