// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "mcpp/concurrent-queue/thread_parker.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <exception>
#include <optional>

namespace mcpp::concurrent_queue {

class oneshot {
  public:
    enum class state : int { available, reserved, cancel_requested, resolved, canceled };
    enum class result : int { resolved, canceled };

  private:
    std::atomic<state> state_{state::available};

  public:
    oneshot() = default;
    virtual ~oneshot() = default;

    oneshot(const oneshot &) = delete;
    oneshot(oneshot &&) = delete;
    auto operator=(const oneshot &) -> oneshot & = delete;
    auto operator=(oneshot &&) -> oneshot & = delete;

  protected:
    virtual void resume() const noexcept = 0;
    virtual auto suspend(std::optional<std::chrono::steady_clock::time_point> deadline) const -> bool = 0;

    auto try_reserve() noexcept -> bool {
        auto old_state = state::available;
        if (state_.compare_exchange_strong(old_state, state::reserved)) {
            return true;
        }
        if (old_state == state::cancel_requested) {
            if (state_.compare_exchange_strong(old_state, state::canceled)) {
                resume();
            }
        }
        return false;
    }

  public:
    auto available() const noexcept -> bool {
        // TODO Inefficient
        auto state = state_.load();
        while (state == state::reserved) {
            state = state_.load();
        }
        return state == state::available;
    }

    auto done() const -> bool {
        auto state = state_.load();
        return state == state::resolved || state == state::canceled;
    }

    auto wait_done(std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt)
        -> std::optional<result> {
        auto result = suspend(deadline);
        assert(result || deadline);
        auto state = state_.load();
        assert(state != state::reserved && state != state::cancel_requested);
        assert(deadline || state != state::available);
        switch (state) {
            case state::resolved:
                return result::resolved;
            case state::canceled:
                return result::canceled;
            case state::available:
                assert(deadline);
                return std::nullopt;
            case state::reserved:
                assert(false);
                std::terminate();
            case state::cancel_requested:
            default:
                assert(false);
                std::terminate();
        }
    }

    template <typename Rep, typename Period>
    auto wait_done(std::chrono::duration<Rep, Period> timeout) const -> std::optional<result> {
        return wait_done({std::chrono::steady_clock::now() + timeout});
    }

    template <typename WorkFunc>
    auto try_resolve(WorkFunc work) noexcept -> decltype(work()) {
        if (!try_reserve()) {
            return {};
        }
        auto result = work();
        state_.store(state::resolved);
        resume();
        return result;
    }

    auto try_cancel() noexcept -> bool {
        auto expected = state::available;
        return state_.compare_exchange_strong(expected, state::cancel_requested);
    }

    template <typename WorkFunc>
    friend auto try_resolve2(oneshot &oneshot1, oneshot &oneshot2, WorkFunc work) noexcept -> decltype(work()) {
        assert(&oneshot1 != &oneshot2); // TODO: Fall back to single oneshot try_resolve
        if (!oneshot1.try_reserve()) {
            return {};
        }
        if (!oneshot2.try_reserve()) {
            // TODO: This is possibly racy...
            oneshot1.state_.store(state::available);
            return {};
        }
        auto result = work();
        oneshot1.state_.store(state::resolved);
        oneshot2.state_.store(state::resolved);
        // printf("%p oneshot resume\n", &oneshot1);
        oneshot1.resume();
        // printf("%p oneshot resume\n", &oneshot2);
        oneshot2.resume();
        return result;
    }
};

class sync_oneshot : public oneshot {
  private:
    thread_parker &parker_{thread_parker::this_thread_parker()};

  public:
    sync_oneshot() {
        // printf("%p oneshot %p parker\n", this, &parker_);
    }
    ~sync_oneshot() {
        assert(parker_.resumed());
        // printf("%p oneshot destroyed\n", this);
    }

  protected:
    void resume() const noexcept override { parker_.resume(); }
    auto suspend(std::optional<std::chrono::steady_clock::time_point> deadline) const -> bool override {
        return parker_.suspend(deadline);
    }
};

} // namespace mcpp::concurrent_queue