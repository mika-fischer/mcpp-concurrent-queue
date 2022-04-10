// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "mcpp/concurrent-queue/oneshot.hpp"
#include "mcpp/concurrent-queue/synchronized_value.hpp"
#include "mcpp/concurrent-queue/thread_parker.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <exception>
#include <optional>

namespace mcpp::concurrent_queue {

enum class try_op_status { success, closed, wouldblock };
enum class op_status { success, closed };

constexpr auto to_try_op_status(op_status s) noexcept -> try_op_status {
    switch (s) {
        case op_status::success:
            return try_op_status::success;
        case op_status::closed:
            return try_op_status::closed;
        default:
            std::terminate();
    }
}

constexpr auto to_op_status(try_op_status s) noexcept -> op_status {
    switch (s) {
        case try_op_status::success:
            return op_status::success;
        case try_op_status::closed:
            return op_status::closed;
        default:
            std::terminate();
    }
}

template <typename T>
struct recv_result {
    op_status status;
    std::optional<T> value;

    friend auto operator==(recv_result lhs, recv_result rhs) noexcept -> bool {
        return lhs.status == rhs.status && lhs.value == rhs.value;
    }

    friend auto operator!=(recv_result lhs, recv_result rhs) noexcept -> bool { return !(lhs == rhs); }
};

// TODO: Use std::expected or something like that
template <typename T>
struct try_recv_result {
    try_op_status status;
    std::optional<T> value;

    operator recv_result<T>() && { return {to_op_status(status), std::move(value)}; }

    friend auto operator==(try_recv_result lhs, try_recv_result rhs) noexcept -> bool {
        return lhs.status == rhs.status && lhs.value == rhs.value;
    }

    friend auto operator!=(try_recv_result lhs, try_recv_result rhs) noexcept -> bool { return !(lhs == rhs); }
};

template <typename T>
struct Chan;

class pending_op_base {
  public:
    pending_op_base() = default;
    virtual ~pending_op_base() = default;

    pending_op_base(const pending_op_base &) = delete;
    pending_op_base(pending_op_base &&) = delete;
    auto operator=(const pending_op_base &) -> pending_op_base & = delete;
    auto operator=(pending_op_base &&) -> pending_op_base & = delete;
};

template <typename T>
class pending_op : public pending_op_base {
  private:
    synchronized_value<Chan<T>> *chan_{nullptr};

  protected:
    explicit pending_op() = default;
    explicit pending_op(synchronized_value<Chan<T>> &chan) : chan_(&chan) {}

    [[nodiscard]] auto empty() const noexcept -> bool { return chan_ == nullptr; }
    auto chan() noexcept -> synchronized_value<Chan<T>> & { return *chan_; }
};

template <typename T>
class pending_send : public pending_op<T> {
  public:
    using pending_op<T>::pending_op;
    virtual auto try_resolve(op_status status) noexcept -> std::optional<T> = 0;
    auto try_cancel() noexcept -> bool;
};

template <typename T>
class pending_recv : public pending_op<T> {
  public:
    using pending_op<T>::pending_op;
    virtual auto try_resolve(T &&value) noexcept -> bool = 0;
    virtual auto try_resolve(const T &value) noexcept -> bool = 0;
    virtual void resolve_closed() noexcept = 0;
    auto try_cancel() noexcept -> bool;
};

using sync_oneshot = oneshot<thread_parker<> &>;

template <typename T>
class blocking_send : public pending_send<T> {
  private:
    sync_oneshot oneshot_{thread_parker<>::this_thread_parker()};
    std::optional<op_status> status_;
    T *move_value_{nullptr};
    const T *copy_value_{nullptr};

  public:
    explicit blocking_send(synchronized_value<Chan<T>> &chan, T &val) : pending_send<T>(chan), move_value_(&val) {}
    explicit blocking_send(synchronized_value<Chan<T>> &chan, const T &val)
        : pending_send<T>(chan), copy_value_(&val) {}

    auto try_resolve(op_status status) noexcept -> std::optional<T> override {
        return oneshot_.try_resolve([&]() -> std::optional<T> {
            status_ = status;
            if (status_ == op_status::success) {
                return move_value_ ? std::move(*move_value_) : *copy_value_;
            }
            return std::nullopt;
        });
    }

    template <typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto try_send(std::optional<std::chrono::time_point<Clock, Duration>> deadline = std::nullopt) -> try_op_status {
        // TODO: Handle exception in wait_done_until & wait_done
        if (auto done = oneshot_.wait_done(deadline)) {
            return make_try_send_status(*done);
        }
        if (oneshot_.try_cancel() && this->try_cancel()) {
            return try_op_status::wouldblock;
        }
        return make_try_send_status(*oneshot_.wait_done());
    }

  private:
    auto make_try_send_status(oneshot_result result) noexcept -> try_op_status {
        switch (result) {
            case oneshot_result::resolved:
                return to_try_op_status(*status_);
            case oneshot_result::canceled:
                return try_op_status::wouldblock;
            default:
                std::terminate();
        }
    }
};

template <typename T>
class blocking_recv : public pending_recv<T> {
  private:
    sync_oneshot *oneshot_{nullptr};
    std::optional<recv_result<T>> result_;

  public:
    explicit blocking_recv() = default;
    explicit blocking_recv(synchronized_value<Chan<T>> &chan, sync_oneshot &oneshot)
        : pending_recv<T>(chan), oneshot_(&oneshot) {}

    void emplace(synchronized_value<Chan<T>> &chan, sync_oneshot &oneshot) {
        assert(this->chan_ == nullptr);
        assert(oneshot_ == nullptr);
        this->chan_ = &chan;
        oneshot_ = &oneshot;
    }

    auto try_resolve(T &&value) noexcept -> bool override {
        return oneshot_->try_resolve([&] {
            result_ = recv_result<T>{op_status::success, std::move(value)};
            return true;
        });
    }

    auto try_resolve(const T &value) noexcept -> bool override {
        return oneshot_->try_resolve([&] {
            result_ = recv_result<T>{op_status::success, value};
            return true;
        });
    }

    void resolve_closed() noexcept override {
        oneshot_->try_resolve([&] {
            result_ = recv_result<T>{op_status::closed, std::nullopt};
            return true;
        });
    }

    auto recv() -> recv_result<T> {
        // TODO: Handle exception in wait_done
        switch (*oneshot_->wait_done()) {
            case oneshot_result::resolved:
                return std::move(*result_);
            case oneshot_result::canceled:
                std::terminate();
        }
    }

    template <typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto try_recv(std::optional<std::chrono::time_point<Clock, Duration>> deadline = {}) -> try_recv_result<T> {
        // TODO: Handle exception in wait_done_until & wait_done
        if (auto done = oneshot_->wait_done(deadline)) {
            return make_try_recv_result(*done);
        }
        if (oneshot_->try_cancel() && this->try_cancel()) {
            return {try_op_status::wouldblock, std::nullopt};
        }
        return make_try_recv_result(*oneshot_->wait_done());
    }

  private:
    auto make_try_recv_result(oneshot_result result) noexcept -> try_recv_result<T> {
        switch (result) {
            case oneshot_result::resolved:
                return {to_try_op_status(result_->status), std::move(result_->value)};
            case oneshot_result::canceled:
                return {try_op_status::wouldblock, std::nullopt};
            default:
                std::terminate();
        }
    }
};

template <typename T>
class pending_op_queue {
  private:
    std::deque<T *> pending_ops_;

  public:
    auto push(T *op) -> void { pending_ops_.push_back(op); }

    auto pop() -> T * {
        if (pending_ops_.empty()) {
            return nullptr;
        }
        auto op = std::move(pending_ops_.front());
        pending_ops_.pop_front();
        return op;
    }

    auto remove(const T *op) -> bool {
        auto it = std::find(pending_ops_.begin(), pending_ops_.end(), op);
        if (it == pending_ops_.end()) {
            return false;
        }
        pending_ops_.erase(it);
        return true;
    };
};

template <typename T>
struct Chan {
    std::deque<T> queue;
    pending_op_queue<pending_send<T>> pending_sends;
    pending_op_queue<pending_recv<T>> pending_recvs;
};

template <typename T>
inline auto pending_send<T>::try_cancel() noexcept -> bool {
    return this->empty() || this->chan().lock()->pending_sends.remove(this);
}

template <typename T>
inline auto pending_recv<T>::try_cancel() noexcept -> bool {
    return this->empty() || this->chan().lock()->pending_recvs.remove(this);
}

class concurrent_queue_base {
  protected:
    const std::optional<std::size_t> capacity_;
    std::atomic<bool> closed_{false};

  public:
    explicit concurrent_queue_base(std::optional<std::size_t> capacity) : capacity_(capacity) {}

    auto capacity() const noexcept -> const std::optional<std::size_t> & { return capacity_; }
    auto closed() const noexcept -> bool { return closed_.load(std::memory_order_seq_cst); }
};

template <typename T>
class concurrent_queue : public concurrent_queue_base {
  private:
    synchronized_value<Chan<T>> chan_;
    using chan_guard_type = typename synchronized_value<Chan<T>>::guard_proxy;

    template <typename U>
    auto try_send_impl(chan_guard_type &chan, U &&msg) -> try_op_status {
        if (closed()) {
            return try_op_status::closed;
        }
        while (auto *recv = chan->pending_recvs.pop()) {
            if (recv->try_resolve(std::forward<U>(msg))) {
                return try_op_status::success;
            }
        }
        if (!capacity_ || chan->queue.size() < *capacity_) {
            chan->queue.emplace_back(std::forward<U>(msg));
            return try_op_status::success;
        }
        return try_op_status::wouldblock;
    }

    auto try_recv_impl(chan_guard_type &chan) -> try_recv_result<T> {
        if (!chan->queue.empty()) {
            auto result = try_recv_result<T>{try_op_status::success, std::move(chan->queue.front())};
            chan->queue.pop_front();
            // Try to resolve one pending send and move into queue
            while (auto *send = chan->pending_sends.pop()) {
                if (auto value = send->try_resolve(op_status::success)) {
                    chan->queue.emplace_back(std::move(*value));
                    break;
                }
            }
            return result;
        }
        while (auto *send = chan->pending_sends.pop()) {
            if (auto value = send->try_resolve(op_status::success)) {
                return try_recv_result<T>{try_op_status::success, std::move(*value)};
            }
        }
        return {closed() ? try_op_status::closed : try_op_status::wouldblock, std::nullopt};
    }

    template <typename U, typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto blocking_send_impl(U &&msg, std::optional<std::chrono::time_point<Clock, Duration>> deadline = std::nullopt)
        -> try_op_status {
        auto chan = chan_.lock();
        if (auto status = try_send_impl(chan, std::forward<U>(msg)); status != try_op_status::wouldblock) {
            return status;
        }
        auto send_op = blocking_send<T>(chan_, std::forward<U>(msg));
        chan->pending_sends.push(&send_op);
        chan.unlock();
        return send_op.try_send(deadline);
    }

    template <typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto blocking_recv_impl(std::optional<std::chrono::time_point<Clock, Duration>> deadline = {})
        -> try_recv_result<T> {
        auto chan = chan_.lock();
        if (auto try_recv_result = try_recv_impl(chan); try_recv_result.status != try_op_status::wouldblock) {
            return try_recv_result;
        }
        auto oneshot = sync_oneshot{thread_parker<>::this_thread_parker()};
        auto recv_op = blocking_recv<T>(chan_, oneshot);
        chan->pending_recvs.push(&recv_op);
        chan.unlock();
        return recv_op.try_recv(deadline);
    }

  public:
    explicit concurrent_queue(std::optional<std::size_t> capacity) : concurrent_queue_base(capacity) {}
    auto size() -> std::size_t {
        auto chan = chan_.lock();
        return chan->queue.size();
    }
    auto empty() -> bool { return size() == 0; }
    auto full() -> bool { return capacity_ ? size() >= *capacity_ : false; }

    void close() {
        closed_.store(true, std::memory_order_relaxed);
        auto chan = chan_.lock();
        while (auto send = chan->pending_sends.pop()) {
            send->try_resolve(op_status::closed);
        }
        while (auto recv = chan->pending_recvs.pop()) {
            recv->resolve_closed();
        }
    }

    // non-blocking operations
    auto try_send(T &&msg) -> try_op_status {
        auto chan = chan_.lock();
        return try_send_impl(chan, std::move(msg));
    }

    auto try_send(const T &msg) -> try_op_status {
        auto chan = chan_.lock();
        return try_send_impl(chan, msg);
    }

    auto try_recv() -> try_recv_result<T> {
        auto chan = chan_.lock();
        return try_recv_impl(chan);
    }

    // blocking operations
    auto send(T &&msg) -> op_status { return to_op_status(blocking_send_impl(std::move(msg))); }
    auto send(const T &msg) -> op_status { return to_op_status(blocking_send_impl(msg)); }

    template <typename Clock, typename Duration>
    auto try_send_until(T &&msg, std::chrono::time_point<Clock, Duration> deadline) -> try_op_status {
        return blocking_send_impl(std::move(msg), deadline);
    }

    template <typename Clock, typename Duration>
    auto try_send_until(const T &msg, std::chrono::time_point<Clock, Duration> deadline) -> try_op_status {
        return blocking_send_impl(msg, deadline);
    }

    template <typename Rep, typename Period>
    auto try_send_for(T &&msg, std::chrono::duration<Rep, Period> timeout) -> try_op_status {
        return blocking_send_impl(std::move(msg), {std::chrono::steady_clock::now() + timeout});
    }

    template <typename Rep, typename Period>
    auto try_send_for(const T &msg, std::chrono::duration<Rep, Period> timeout) -> try_op_status {
        return blocking_send_impl(msg, {std::chrono::steady_clock::now() + timeout});
    }

    auto recv() -> recv_result<T> { return blocking_recv_impl(); }

    template <typename Clock, typename Duration>
    auto try_recv_until(std::chrono::time_point<Clock, Duration> deadline) -> try_recv_result<T> {
        return blocking_recv_impl(deadline);
    }

    template <typename Rep, typename Period>
    auto try_recv_for(std::chrono::duration<Rep, Period> timeout) -> try_recv_result<T> {
        return blocking_recv_impl({std::chrono::steady_clock::now() + timeout});
    }
};

template <typename T>
class send_iterator {
  private:
    concurrent_queue<T> *queue_{nullptr};

  public:
    using iterator_category = std::output_iterator_tag;
    using value_type = void;
    using difference_type = void;
    using pointer = void;
    using reference = void;

    send_iterator() = default;
    explicit send_iterator(concurrent_queue<T> &queue) : queue_(&queue) {}

    auto operator*() -> send_iterator & { return *this; }
    auto operator++() -> send_iterator & { return *this; }
    auto operator++(int) -> send_iterator & { return *this; }
    auto operator=(const T &value) -> send_iterator & { return handle_send_result(queue_->send(value)); }
    auto operator=(T &&value) -> send_iterator & { return handle_send_result(queue_->send(std::move(value))); }

    auto operator==(const send_iterator &other) -> bool { return queue_ == other.queue_; }
    auto operator!=(const send_iterator &other) -> bool { return queue_ != other.queue_; }

  private:
    auto handle_send_result(op_status result) -> send_iterator & {
        if (result != op_status::success) {
            queue_ = nullptr;
            // TODO: ???
            throw result;
        }
        return *this;
    }
};

template <typename T>
class recv_iterator {
  private:
    concurrent_queue<T> *queue_{nullptr};
    recv_result<T> value_;

  public:
    using iterator_category = std::input_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T *;
    using reference = T &&;

    class arrow_proxy {
      private:
        T value_;

      public:
        explicit arrow_proxy(T &value) : value_(std::move(value)) {}
        auto operator*() -> reference { return std::move(value_); }
        auto operator->() -> pointer { return &value_; }
    };

    recv_iterator() = default;
    explicit recv_iterator(concurrent_queue<T> &queue) : queue_(&queue) { next(); }

    auto operator*() -> reference { return std::move(*value_.value); }
    auto operator->() -> pointer { return &value_.value; }
    auto operator++() -> recv_iterator & {
        next();
        return *this;
    }
    auto operator++(int) -> arrow_proxy {
        auto proxy = arrow_proxy(*value_.value);
        next();
        return proxy;
    }

    auto operator==(const recv_iterator &other) -> bool { return queue_ == other.queue_; }
    auto operator!=(const recv_iterator &other) -> bool { return queue_ != other.queue_; }

  private:
    void next() {
        value_ = queue_->recv();
        if (value_.status == op_status::closed) {
            queue_ = nullptr;
        }
    }
};

class shared_queue_base {
  private:
    // TODO: A bit wasteful to have both these counts and also the refcount in the shared_ptr
    std::atomic<std::size_t> tx_count_{0};
    std::atomic<std::size_t> rx_count_{0};

  protected:
    void inc_tx() { tx_count_.fetch_add(1, std::memory_order_relaxed); }
    void inc_rx() { rx_count_.fetch_add(1, std::memory_order_relaxed); }
    auto dec_tx() -> bool { return tx_count_.fetch_sub(1, std::memory_order_relaxed) == 1; }
    auto dec_rx() -> bool { return rx_count_.fetch_sub(1, std::memory_order_relaxed) == 1; }
};

template <typename T>
class queue_sender;

template <typename T>
class queue_receiver;

template <typename T>
class shared_queue : public shared_queue_base, public concurrent_queue<T> {
  public:
    using concurrent_queue<T>::concurrent_queue;
    friend class queue_sender<T>;
    friend class queue_receiver<T>;
};

template <typename T>
class queue_sender {
  private:
    std::shared_ptr<shared_queue<T>> queue_;

  public:
    queue_sender() noexcept = default;
    explicit queue_sender(std::shared_ptr<shared_queue<T>> queue) noexcept : queue_(std::move(queue)) { inc(); }
    queue_sender(const queue_sender &other) noexcept : queue_(other.queue_) { inc(); }
    queue_sender(queue_sender &&other) noexcept : queue_(std::exchange(other.queue_, nullptr)) {}

    auto operator=(const queue_sender &other) noexcept -> queue_sender & {
        if (&other != this) {
            dec();
            queue_ = other.queue_;
            inc();
        }
        return *this;
    }

    auto operator=(queue_sender &&other) noexcept -> queue_sender & {
        if (&other != this) {
            dec();
            queue_ = std::exchange(other.queue_, nullptr);
        }
        return *this;
    }

    ~queue_sender() { dec(); }

    auto try_send(T &&msg) -> try_op_status { return queue_->try_send(std::move(msg)); }

    auto try_send(const T &msg) -> try_op_status { return queue_->try_send(msg); }

    auto send(T &&msg) -> op_status { return queue_->send(std::move(msg)); }

    auto send(const T &msg) -> op_status { return queue_->send(msg); }

    template <typename Clock, typename Duration>
    auto try_send_until(T &&msg, std::chrono::time_point<Clock, Duration> deadline) -> try_op_status {
        return queue_->try_send_until(std::move(msg), deadline);
    }

    template <typename Clock, typename Duration>
    auto try_send_until(const T &msg, std::chrono::time_point<Clock, Duration> deadline) -> try_op_status {
        return queue_->try_send_until(msg, deadline);
    }

    template <typename Rep, typename Period>
    auto try_send_for(T &&msg, std::chrono::duration<Rep, Period> timeout) -> try_op_status {
        return queue_->try_send_for(std::move(msg), timeout);
    }

    template <typename Rep, typename Period>
    auto try_send_for(const T &msg, std::chrono::duration<Rep, Period> timeout) -> try_op_status {
        return queue_->try_send_for(msg, timeout);
    }

    auto begin() -> send_iterator<T> { return send_iterator<T>(*queue_); };
    auto end() -> send_iterator<T> { return send_iterator<T>(); };

  private:
    void inc() noexcept {
        if (queue_) {
            queue_->inc_tx();
        }
    }
    void dec() noexcept {
        if (queue_ && queue_->dec_tx()) {
            queue_->close();
        }
    }
};

template <typename T>
class queue_receiver {
  private:
    std::shared_ptr<shared_queue<T>> queue_;

  public:
    queue_receiver() noexcept = default;
    explicit queue_receiver(std::shared_ptr<shared_queue<T>> queue) noexcept : queue_(std::move(queue)) { inc(); }
    queue_receiver(const queue_receiver &other) noexcept : queue_(other.queue_) { inc(); }
    queue_receiver(queue_receiver &&other) noexcept : queue_(std::exchange(other.queue_, nullptr)) {}

    auto operator=(const queue_receiver &other) noexcept -> queue_receiver & {
        if (&other != this) {
            dec();
            queue_ = other.queue_;
            inc();
        }
        return *this;
    }

    auto operator=(queue_receiver &&other) noexcept -> queue_receiver & {
        if (&other != this) {
            dec();
            queue_ = std::exchange(other.queue_, nullptr);
        }
        return *this;
    }

    ~queue_receiver() { dec(); }

    auto try_recv() -> try_recv_result<T> { return queue_->try_recv(); }

    auto recv() -> recv_result<T> { return queue_->recv(); }

    template <typename Clock, typename Duration>
    auto try_recv_until(std::chrono::time_point<Clock, Duration> deadline) -> try_recv_result<T> {
        return queue_->try_recv_until(deadline);
    }

    template <typename Rep, typename Period>
    auto try_recv_for(std::chrono::duration<Rep, Period> timeout) -> try_recv_result<T> {
        return queue_->try_recv_for(timeout);
    }

    auto begin() -> recv_iterator<T> { return recv_iterator<T>(*queue_); };
    auto end() -> recv_iterator<T> { return recv_iterator<T>(); };

  private:
    void inc() noexcept {
        if (queue_) {
            queue_->inc_rx();
        }
    }
    void dec() noexcept {
        if (queue_ && queue_->dec_rx()) {
            queue_->close();
        }
    }
};

template <typename T>
struct make_queue_result {
    queue_sender<T> tx;
    queue_receiver<T> rx;
};

template <typename T>
auto make_queue(std::optional<std::size_t> capacity) -> make_queue_result<T> {
    auto tx_shared = std::make_shared<shared_queue<T>>(capacity);
    auto rx_shared = tx_shared;
    return {queue_sender<T>{std::move(tx_shared)}, queue_receiver<T>{std::move(rx_shared)}};
}

} // namespace mcpp::concurrent_queue