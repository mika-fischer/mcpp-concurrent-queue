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

class pending_op {
  private:
    oneshot *oneshot_{nullptr};

  public:
    virtual ~pending_op() = default;

    pending_op(const pending_op &) = delete;
    pending_op(pending_op &&) = delete;
    auto operator=(const pending_op &) -> pending_op & = delete;
    auto operator=(pending_op &&) -> pending_op & = delete;

    [[nodiscard]] auto available() const noexcept -> bool {
        assert(oneshot_ != nullptr);
        return oneshot_->available();
    }

  protected:
    explicit pending_op(oneshot *oneshot = nullptr) noexcept : oneshot_{oneshot} {}

    void emplace(oneshot *oneshot) noexcept {
        assert(oneshot_ == nullptr);
        oneshot_ = oneshot;
    }

    auto get_oneshot() noexcept -> oneshot & {
        assert(oneshot_ != nullptr);
        return *oneshot_;
    }
};

template <typename T>
class pending_recv;

template <typename T>
struct Chan;

template <typename T>
class pending_send : public pending_op {
  private:
    T *move_value_{nullptr};
    const T *copy_value_{nullptr};
    std::optional<op_status> status_;

  public:
    pending_send() = default;
    explicit pending_send(T &&value, oneshot *oneshot = nullptr) : pending_op(oneshot), move_value_(&value) {}
    explicit pending_send(const T &value, oneshot *oneshot = nullptr) : pending_op(oneshot), copy_value_(&value) {}

    auto try_resolve(op_status status) noexcept -> std::optional<T> {
        return get_oneshot().try_resolve([&]() -> std::optional<T> {
            status_ = status;
            if (status_ == op_status::success) {
                return move_value_ ? std::move(*move_value_) : *copy_value_;
            }
            return std::nullopt;
        });
    }

    template <typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto try_send(synchronized_value<Chan<T>> &chan,
                  std::optional<std::chrono::time_point<Clock, Duration>> deadline = std::nullopt) -> try_op_status {
        // TODO: Handle exception in wait_done_until & wait_done
        if (auto done = get_oneshot().wait_done(deadline)) {
            return make_try_send_status(*done);
        }
        if (get_oneshot().try_cancel() && chan.lock()->pending_sends.try_remove(this)) {
            // there's nobody on the other side and there never will be, so we set the status to cancelled ourselves
            auto result = get_oneshot().try_resolve([] { return true; });
            assert(!result);
        }
        auto result = make_try_send_status(*get_oneshot().wait_done());
        assert(get_oneshot().done());
        return result;
    }

  private:
    auto make_try_send_status(oneshot::result result) noexcept -> try_op_status {
        switch (result) {
            case oneshot::result::resolved:
                return to_try_op_status(*status_);
            case oneshot::result::canceled:
                return try_op_status::wouldblock;
            default:
                std::terminate();
        }
    }

    friend class pending_recv<T>;
};

template <typename T>
class pending_recv : public pending_op {
  private:
    std::optional<recv_result<T>> result_;

  public:
    explicit pending_recv(oneshot *oneshot = nullptr) : pending_op(oneshot) {}

    auto try_resolve(pending_send<T> &send) noexcept -> bool {
        return try_resolve2(send.get_oneshot(), get_oneshot(), [&]() -> bool {
            result_ = recv_result<T>{op_status::success};
            if (send.move_value_) {
                result_->value.emplace(std::move(*send.move_value_));
            } else {
                result_->value.emplace(*send.copy_value_);
            }
            send.status_ = op_status::success;
            return true;
        });
    }

    auto try_resolve(T &&value) noexcept -> bool {
        return get_oneshot().try_resolve([&] {
            result_ = recv_result<T>{op_status::success, std::move(value)};
            return true;
        });
    }

    auto try_resolve(const T &value) noexcept -> bool {
        return get_oneshot().try_resolve([&] {
            result_ = recv_result<T>{op_status::success, value};
            return true;
        });
    }

    void resolve_closed() noexcept {
        get_oneshot().try_resolve([&] {
            result_ = recv_result<T>{op_status::closed, std::nullopt};
            return true;
        });
    }

    template <typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto try_recv(synchronized_value<Chan<T>> &chan,
                  std::optional<std::chrono::time_point<Clock, Duration>> deadline = {}) -> try_recv_result<T> {
        // TODO: Handle exception in wait_done_until & wait_done
        if (auto done = get_oneshot().wait_done(deadline)) {
            auto result = make_try_recv_result(*done);
            assert(deadline || get_oneshot().done());
            return result;
        }
        if (get_oneshot().try_cancel() && chan.lock()->pending_recvs.try_remove(this)) {
            // there's nobody on the other side and there never will be, so we set the status to cancelled ourselves
            auto result = get_oneshot().try_resolve([] { return true; });
            assert(!result);
        }
        auto result = make_try_recv_result(*get_oneshot().wait_done());
        assert(get_oneshot().done());
        return result;
    }

  private:
    auto make_try_recv_result(oneshot::result result) noexcept -> try_recv_result<T> {
        switch (result) {
            case oneshot::result::resolved:
                return {to_try_op_status(result_->status), std::move(result_->value)};
            case oneshot::result::canceled:
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

    auto empty() -> bool { return pending_ops_.empty(); }
    auto front() -> T * { return pending_ops_.front(); }

    auto pop() -> T * {
        if (pending_ops_.empty()) {
            return nullptr;
        }
        auto op = std::move(pending_ops_.front());
        pending_ops_.pop_front();
        return op;
    }

    auto try_remove(const T *op) -> bool {
        auto it = std::find(pending_ops_.begin(), pending_ops_.end(), op);
        if (it == pending_ops_.end()) {
            return false;
        }
        pending_ops_.erase(it);
        return true;
    };
};

class concurrent_queue_base {
  protected:
    const std::optional<std::size_t> capacity_;
    std::atomic<bool> closed_{false};

  public:
    explicit concurrent_queue_base(std::optional<std::size_t> capacity) : capacity_(capacity) {}

    auto capacity() const noexcept -> const std::optional<std::size_t> & { return capacity_; }
    auto closed() const noexcept -> bool { return closed_.load(std::memory_order_seq_cst); }

    template <typename T>
    friend struct Chan;
};

template <typename T>
struct Chan {
    concurrent_queue_base &queue_;
    std::deque<T> queue;
    pending_op_queue<pending_send<T>> pending_sends;
    pending_op_queue<pending_recv<T>> pending_recvs;

    Chan(concurrent_queue_base &queue) : queue_(queue) {}

    auto push(pending_recv<T> &op) -> bool {
        if (!queue.empty()) {
            if (op.try_resolve(std::move(queue.front()))) {
                queue.pop_front();
                // Try to resolve one pending send and move into queue
                while (auto *send = pending_sends.pop()) {
                    if (auto value = send->try_resolve(op_status::success)) {
                        queue.emplace_back(std::move(*value));
                        break;
                    }
                }
            }
            return false;
        }
        while (!pending_sends.empty()) {
            if (op.try_resolve(*pending_sends.front())) {
                pending_sends.pop();
                return false;
            }
            if (!op.available()) {
                // printf("op canceled\n");
                // op canceled
                return false;
            }
            assert(!pending_sends.front()->available());
            pending_sends.pop();
        }
        if (queue_.closed()) {
            op.resolve_closed();
            return false;
        }
        pending_recvs.push(&op);
        return true;
    }

    auto try_recv_impl() -> try_recv_result<T> {
        if (!queue.empty()) {
            auto result = try_recv_result<T>{try_op_status::success, std::move(queue.front())};
            queue.pop_front();
            // Try to resolve one pending send and move into queue
            while (auto *send = pending_sends.pop()) {
                if (auto value = send->try_resolve(op_status::success)) {
                    queue.emplace_back(std::move(*value));
                    break;
                }
            }
            return result;
        }
        while (auto *send = pending_sends.pop()) {
            if (auto value = send->try_resolve(op_status::success)) {
                return try_recv_result<T>{try_op_status::success, std::move(*value)};
            }
        }
        return {queue_.closed() ? try_op_status::closed : try_op_status::wouldblock, std::nullopt};
    }

    template <typename U>
    auto try_send_impl(U &&msg) -> try_op_status {
        if (queue_.closed()) {
            return try_op_status::closed;
        }
        while (auto *recv = pending_recvs.pop()) {
            if (recv->try_resolve(std::forward<U>(msg))) {
                return try_op_status::success;
            }
        }
        if (!queue_.capacity() || queue.size() < *queue_.capacity()) {
            queue.emplace_back(std::forward<U>(msg));
            return try_op_status::success;
        }
        return try_op_status::wouldblock;
    }

    void close() {
        queue_.closed_.store(true);
        while (auto send = pending_sends.pop()) {
            send->try_resolve(op_status::closed);
        }
        while (auto recv = pending_recvs.pop()) {
            recv->resolve_closed();
        }
    }
};

template <typename T>
class concurrent_queue : public concurrent_queue_base {
  private:
    synchronized_value<Chan<T>> chan_;
    using chan_guard_type = typename synchronized_value<Chan<T>>::guard_proxy;

    template <typename U, typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto blocking_send_impl(U &&msg, std::optional<std::chrono::time_point<Clock, Duration>> deadline = std::nullopt)
        -> try_op_status {
        auto chan = chan_.lock();
        if (auto status = chan->try_send_impl(std::forward<U>(msg)); status != try_op_status::wouldblock) {
            return status;
        }
        auto oneshot = sync_oneshot();
        auto send_op = pending_send<T>(std::forward<U>(msg), &oneshot);
        chan->pending_sends.push(&send_op);
        chan.unlock();
        auto result = send_op.try_send(chan_, deadline);
        assert(oneshot.done());
        return result;
    }

    template <typename Clock = std::chrono::steady_clock, typename Duration = typename Clock::duration>
    auto blocking_recv_impl(std::optional<std::chrono::time_point<Clock, Duration>> deadline = {})
        -> try_recv_result<T> {
        auto chan = chan_.lock();
        if (auto try_recv_result = chan->try_recv_impl(); try_recv_result.status != try_op_status::wouldblock) {
            return try_recv_result;
        }
        auto oneshot = sync_oneshot();
        auto recv_op = pending_recv<T>(&oneshot);
        chan->push(recv_op);
        chan.unlock();
        auto result = recv_op.try_recv(chan_, deadline);
        assert(oneshot.done());
        return result;
    }

  public:
    explicit concurrent_queue(std::optional<std::size_t> capacity) : concurrent_queue_base(capacity), chan_(*this) {}
    auto size() -> std::size_t {
        auto chan = chan_.lock();
        return chan->queue.size();
    }
    auto empty() -> bool { return size() == 0; }
    auto full() -> bool { return capacity_ ? size() >= *capacity_ : false; }

    void close() { chan_.lock()->close(); }

    // non-blocking operations
    auto try_send(T &&msg) -> try_op_status { return chan_.lock()->try_send_impl(std::move(msg)); }
    auto try_send(const T &msg) -> try_op_status { return chan_.lock()->try_send_impl(msg); }
    auto try_recv() -> try_recv_result<T> { return chan_.lock()->try_recv_impl(); }

    // blocking operations
    auto send(T &&msg) -> op_status { return to_op_status(blocking_send_impl(std::move(msg))); }
    auto send(const T &msg) -> op_status { return to_op_status(blocking_send_impl(msg)); }
    auto recv() -> recv_result<T> { return blocking_recv_impl(); }

    template <typename Clock, typename Duration>
    auto try_send_until(T &&msg, std::chrono::time_point<Clock, Duration> deadline) -> try_op_status {
        return blocking_send_impl(std::move(msg), deadline);
    }
    template <typename Clock, typename Duration>
    auto try_send_until(const T &msg, std::chrono::time_point<Clock, Duration> deadline) -> try_op_status {
        return blocking_send_impl(msg, deadline);
    }
    template <typename Clock, typename Duration>
    auto try_recv_until(std::chrono::time_point<Clock, Duration> deadline) -> try_recv_result<T> {
        return blocking_recv_impl(deadline);
    }

    template <typename Rep, typename Period>
    auto try_send_for(T &&msg, std::chrono::duration<Rep, Period> timeout) -> try_op_status {
        return blocking_send_impl(std::move(msg), {std::chrono::steady_clock::now() + timeout});
    }
    template <typename Rep, typename Period>
    auto try_send_for(const T &msg, std::chrono::duration<Rep, Period> timeout) -> try_op_status {
        return blocking_send_impl(msg, {std::chrono::steady_clock::now() + timeout});
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
    void inc_tx() { tx_count_.fetch_add(1); }
    void inc_rx() { rx_count_.fetch_add(1); }
    auto dec_tx() -> bool { return tx_count_.fetch_sub(1) == 1; }
    auto dec_rx() -> bool { return rx_count_.fetch_sub(1) == 1; }
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

    explicit operator bool() { return queue_ != nullptr; }

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