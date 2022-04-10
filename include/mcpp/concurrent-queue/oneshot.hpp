// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <atomic>
#include <chrono>
#include <optional>

namespace mcpp::concurrent_queue {

enum class oneshot_result : int { resolved, canceled };

class basic_oneshot {
  public:
    enum class state : int { available, reserved, cancel_requested, resolved, canceled };

  private:
    std::atomic<state> state_{state::available};

  public:
    auto done() const -> bool {
        auto state = state_.load();
        return state == state::resolved || state == state::canceled;
    }

    template <typename WorkFunc, typename WakeFunc>
    auto try_resolve(WorkFunc work, WakeFunc wake) noexcept -> decltype(work()) {
        auto old_state = state::available;
        if (state_.compare_exchange_strong(old_state, state::reserved)) {
            auto result = work();
            state_.store(state::resolved);
            wake();
            return result;
        }
        if (old_state == state::cancel_requested) {
            if (state_.compare_exchange_strong(old_state, state::canceled)) {
                wake();
            }
        }
        return {};
    }

    auto try_cancel() noexcept -> bool {
        auto expected = state::available;
        return state_.compare_exchange_strong(expected, state::cancel_requested);
    }

    template <typename SuspendFunc>
    auto wait_done(SuspendFunc suspend) const -> std::optional<oneshot_result> {
        suspend();
        switch (state_.load()) {
            case state::resolved:
                return oneshot_result::resolved;
            case state::canceled:
                return oneshot_result::canceled;
            default:
                return std::nullopt;
        }
    }
};

template <class SuspendResume>
class oneshot {
  private:
    basic_oneshot oneshot_;
    SuspendResume suspend_resume_;

  public:
    explicit oneshot(SuspendResume sr) noexcept : suspend_resume_(sr) {}

    auto done() const -> bool { return oneshot_.done(); }

    template <typename WorkFunc>
    auto try_resolve(WorkFunc work) noexcept {
        return oneshot_.try_resolve(std::move(work), [&] { suspend_resume_.resume(); });
    }

    auto try_cancel() noexcept -> bool { return oneshot_.try_cancel(); }

    template <typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto wait_done(std::optional<std::chrono::time_point<Clock, Duration>> deadline) const
        -> std::optional<oneshot_result> {
        return oneshot_.wait_done([&] { suspend_resume_.suspend(deadline); });
    }

    auto wait_done() const -> std::optional<oneshot_result> { return wait_done({}); }

    template <typename Clock, typename Duration>
    auto wait_done(std::chrono::time_point<Clock, Duration> deadline) const -> std::optional<oneshot_result> {
        return wait_done({deadline});
    }

    template <typename Rep, typename Period>
    auto wait_done(std::chrono::duration<Rep, Period> timeout) const -> std::optional<oneshot_result> {
        return wait_done({std::chrono::steady_clock::now() + timeout});
    }

    template <typename Clock, typename Duration>
    auto wait_done_until(std::chrono::time_point<Clock, Duration> deadline) const -> std::optional<oneshot_result> {
        return wait_done({deadline});
    }

    template <typename Rep, typename Period>
    auto wait_done_for(std::chrono::duration<Rep, Period> timeout) const -> std::optional<oneshot_result> {
        return wait_done({std::chrono::steady_clock::now() + timeout});
    }
};

} // namespace mcpp::concurrent_queue