// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>

namespace mcpp::concurrent_queue {

// TODO:
// - support for std::stop_token
// - support for boost::this_thread::interruption_requested

template <typename mutex_type = std::mutex, typename cvar_type = std::condition_variable,
          typename lock_type = std::unique_lock<mutex_type>>
class thread_parker_generic {
  private:
    enum class state : int { resumed, suspended, notified };
    std::atomic<state> state_{state::resumed};
    mutex_type mutex_;
    cvar_type cvar_;

  public:
    static auto this_thread_parker() noexcept -> thread_parker_generic & {
        thread_local thread_parker_generic parker;
        return parker;
    }

    auto resumed() -> bool { return state_.load() == state::resumed; }

    void resume() noexcept {
        // printf("%p resume\n", this);
        // fflush(stdout);
        auto old_state = state_.exchange(state::notified);
        switch (old_state) {
            case state::suspended:
                // lock mutex to handle case where suspended thread has set state to suspended but is not yet waiting on
                // cvar
                { auto lock = lock_type{mutex_}; }
                cvar_.notify_one();
                return;
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

        if (auto lock = lock_type(mutex_); try_state_change(state::resumed, state::suspended)) {
            if (!deadline) {
                cvar_.wait(lock, try_resume);
                return true;
            }
            if (cvar_.wait_until(lock, *deadline, try_resume)) {
                return true;
            }
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