// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "mcpp/concurrent-queue/concurrent_queue.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <numeric>
#include <optional>
#include <thread>

using namespace mcpp::concurrent_queue;

TEST_CASE("queue.basic.send_recv") {
    static constexpr int num_items = 1000;
    auto [tx, rx] = make_queue<int>(std::nullopt);
    for (int i = 0; i < num_items; ++i) {
        CHECK(tx.send(i) == op_status::success);
    }
    for (int i = 0; i < num_items; ++i) {
        CHECK(rx.recv() == recv_result<int>{op_status::success, i});
    }
    CHECK(rx.try_recv() == try_recv_result<int>{try_op_status::wouldblock, std::nullopt});
}

class counter {
  private:
    int count_ = 0;

  public:
    explicit counter(int count = 0) : count_(count) {}
    auto operator()() -> int { return count_++; }
    auto operator*() const -> int { return count_; }
    auto operator++() -> counter & {
        ++count_;
        return *this;
    }
    auto operator++(int) -> counter {
        auto c = *this;
        ++count_;
        return *this;
    }
    auto operator==(const counter &other) const -> bool { return count_ == other.count_; }
    auto operator!=(const counter &other) const -> bool { return count_ != other.count_; }
};

TEST_CASE("queue.basic.iter") {
    static constexpr int n_elements = 1000;
    auto [tx, rx] = make_queue<int>(std::nullopt);
    std::generate_n(tx.begin(), n_elements, counter());
    tx = {};
    int sum = std::accumulate(rx.begin(), rx.end(), 0);
    int expected_sum = std::accumulate(counter(0), counter(n_elements), 0);
    CHECK(sum == expected_sum);
}

TEST_CASE("queue.basic.iter_threaded") {
    static constexpr int n_elements = 1000;
    auto [tx, rx] = make_queue<int>(std::nullopt);
    for (int i = 0; i < n_elements; ++i) {
        std::thread([i, tx = tx]() mutable { CHECK(tx.send(i) == op_status::success); }).detach();
    }
    tx = {};
    int sum = std::accumulate(rx.begin(), rx.end(), 0);
    int expected_sum = std::accumulate(counter(0), counter(n_elements), 0);
    CHECK(sum == expected_sum);
}

TEST_CASE("queue.basic.try_send_for") {
    static constexpr auto timeout = std::chrono::milliseconds(350);
    static constexpr auto delta = std::chrono::milliseconds(10);

    auto [tx, rx] = make_queue<int>(1);
    CHECK(tx.try_send_for(42, timeout) == try_op_status::success);
    auto then = std::chrono::steady_clock::now();
    CHECK(tx.try_send_for(42, timeout) == try_op_status::wouldblock);
    auto now = std::chrono::steady_clock::now();
    CHECK(now - then > timeout);
    CHECK(now - then < timeout + delta);
    rx = {};
    CHECK(tx.try_send_for(42, timeout) == try_op_status::closed);
}

TEST_CASE("queue.basic.try_recv_for") {
    static constexpr auto timeout = std::chrono::milliseconds(350);
    static constexpr auto delta = std::chrono::milliseconds(10);

    auto [tx, rx] = make_queue<int>(std::nullopt);
    auto then = std::chrono::steady_clock::now();
    CHECK(rx.try_recv_for(timeout).status == try_op_status::wouldblock);
    auto now = std::chrono::steady_clock::now();
    CHECK(now - then > timeout);
    CHECK(now - then < timeout + delta);
    CHECK(tx.send(42) == op_status::success);
    then = std::chrono::steady_clock::now();
    CHECK(rx.try_recv_for(timeout) == try_recv_result<int>{try_op_status::success, 42});
    now = std::chrono::steady_clock::now();
    CHECK(now - then < delta);
}

TEST_CASE("queue.basic.recv_timeout_missed_send") {
    auto [tx, rx] = make_queue<int>(10);
    CHECK(rx.try_recv_for(std::chrono::milliseconds(100)) ==
          try_recv_result<int>{try_op_status::wouldblock, std::nullopt});
    tx.send(42);
    CHECK(rx.recv() == recv_result<int>{op_status::success, 42});
}

TEST_CASE("queue.basic.close_tx") {
    auto [tx, rx] = make_queue<int>(std::nullopt);
    tx = {};
    CHECK(rx.recv().status == op_status::closed);
}

TEST_CASE("queue.basic.close_rx") {
    auto [tx, rx] = make_queue<int>(std::nullopt);
    rx = {};
    CHECK(tx.send(0) == op_status::closed);
}

TEST_CASE("queue.basic.try_send") {
    static constexpr int n_items = 5;
    auto [tx, rx] = make_queue<int>(n_items);
    for (int i = 0; i < n_items; ++i) {
        CHECK(tx.try_send(i) == try_op_status::success);
    }
    CHECK(tx.try_send(42) == try_op_status::wouldblock);
    CHECK(rx.recv() == recv_result<int>{op_status::success, 0});
    CHECK(tx.try_send(42) == try_op_status::success);
    CHECK(rx.recv() == recv_result<int>{op_status::success, 1});
    rx = {};
    CHECK(tx.try_send(42) == try_op_status::closed);
}

// TODO: Check why this is so slow
TEST_CASE("queue.basic.send_bounded") {
    static constexpr size_t n_elements = 10000;
    static constexpr size_t n_threads = 100;

    auto [tx, rx] = make_queue<int>(5);
    for (int i = 0; i < 5; ++i) {
        CHECK(tx.send(42) == op_status::success);
    }
    CHECK(rx.recv() == recv_result<int>{op_status::success, 42});
    CHECK(tx.send(42) == op_status::success);
    CHECK(tx.try_send(42) == try_op_status::wouldblock);

    while (rx.try_recv().status == try_op_status::success) {
    }

    auto threads = std::vector<std::thread>(n_threads);
    for (auto &t : threads) {
        t = std::thread([tx = tx]() mutable {
            for (int i = 0; i < n_elements; ++i) {
                CHECK(tx.send(i) == op_status::success);
            }
        });
    }
    tx = {};

    auto sum = std::accumulate(rx.begin(), rx.end(), 0LL);
    auto expected_sum = std::accumulate(counter(0), counter(n_elements), 0LL);
    CHECK(sum == expected_sum * n_threads);

    for (auto &t : threads) {
        t.join();
    }

    CHECK(rx.recv().status == op_status::closed);
}

TEST_CASE("queue.basic.rendezvous") {
    auto [tx, rx] = make_queue<int>(0);
    for (int i = 0; i < 5; i++) {
        auto t = std::thread([tx = tx]() mutable {
            CHECK(tx.try_send(0) == try_op_status::wouldblock);
            auto then = std::chrono::steady_clock::now();
            CHECK(tx.send(0) == op_status::success);
            auto now = std::chrono::steady_clock::now();
            CHECK(now - then > std::chrono::milliseconds(50));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(rx.recv() == recv_result<int>{op_status::success, 0});
        t.join();
    }
}

TEST_CASE("queue.basic.hydra") {
    static constexpr std::size_t n_threads = 100;
    static constexpr std::size_t n_batches = 10;
    static constexpr std::size_t n_messages = 10000;

    auto [main_tx, main_rx] = make_queue<int>(std::nullopt);

    auto txs = std::vector<queue_sender<int>>();
    for (std::size_t i = 0; i < n_threads; ++i) {
        auto [tx, rx] = make_queue<int>(std::nullopt);
        txs.push_back(tx);
        std::thread([rx = rx, main_tx = main_tx]() mutable {
            std::copy(rx.begin(), rx.end(), main_tx.begin());
        }).detach();
    }

    main_tx = {};

    for (std::size_t b = 0; b < n_batches; ++b) {
        for (auto &tx : txs) {
            for (std::size_t j = 0; j < n_messages; ++j) {
                CHECK(tx.send(0) == op_status::success);
            }
        }
        for (std::size_t t = 0; t < n_threads; ++t) {
            for (std::size_t m = 0; m < n_messages; ++m) {
                CHECK(main_rx.recv().status == op_status::success);
            }
        }
    }

    txs.clear();
    CHECK(main_rx.recv().status == op_status::closed);
}

TEST_CASE("queue.basic.daisy_chain") {
    static constexpr std::size_t n_threads = 32;
    static constexpr std::size_t n_batches = 10;
    static constexpr std::size_t n_messages = 1000;

    auto [main_tx, main_rx] = make_queue<int>(0);

    for (std::size_t i = 0; i < n_threads; ++i) {
        auto [tx, rx] = make_queue<int>(0);
        std::swap(tx, main_tx);
        std::thread([rx = std::move(rx), tx = std::move(tx)]() mutable {
            std::copy(rx.begin(), rx.end(), tx.begin());
        }).detach();
    }

    for (std::size_t b = 0; b < n_batches; ++b) {
        std::thread([main_tx = main_tx]() mutable {
            for (std::size_t m = 0; m < n_messages; ++m) {
                CHECK(main_tx.send(0) == op_status::success);
            }
        }).detach();
        for (std::size_t m = 0; m < n_messages; ++m) {
            CHECK(main_rx.recv().status == op_status::success);
        }
    }

    main_tx = {};
    CHECK(main_rx.recv().status == op_status::closed);
}

// TODO: flume: drain?
