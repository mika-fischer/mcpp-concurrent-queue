// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

// TODO: Hack hack hack
#define private public

#include "mcpp/concurrent-queue/concurrent_queue.hpp"
#include "mcpp/concurrent-queue/oneshot.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <tuple>
#include <utility>

namespace mcpp::concurrent_queue {

struct select_timeout {};

template <typename T>
struct queue_and_op {
    queue_receiver<T> &rx;
    blocking_recv<T> op;

    queue_and_op(queue_receiver<T> &rx) : rx(rx) {}
};

template <typename T>
auto schedule_recv(queue_and_op<T> &item, sync_oneshot &oneshot) -> bool {
    if (oneshot.done()) {
        return false;
    }
    auto chan = item.rx.queue_->chan_.lock();
    if (auto try_recv_result = item.rx.queue_->try_recv_impl(chan);
        try_recv_result.status != try_op_status::wouldblock) {
        item.op.result_.emplace(std::move(try_recv_result));
        return false;
    }
    item.op.emplace(item.rx.queue_->chan_, oneshot);
    chan->pending_recvs.push(&item.op);
    return true;
}

template <typename Variant, size_t I, typename T>
auto extract_result_impl(queue_and_op<T> &item, Variant &result) -> bool {
    if (result || !item.op.result_) {
        return false;
    }
    result.emplace(std::in_place_index<I>, std::move(*item.op.result_));
    return true;
}

template <typename Variant, typename Tuple, size_t... Is>
auto extract_result(Tuple &items, Variant &result, std::index_sequence<Is...>) -> bool {
    return (extract_result_impl<Variant, Is>(std::get<Is>(items), result) || ...);
}

template <typename... Ts>
auto select_recv(queue_receiver<Ts> &...rxs) -> std::variant<recv_result<Ts>...> {
    using result_type = std::variant<recv_result<Ts>...>;
    auto oneshot = sync_oneshot{thread_parker<>::this_thread_parker()};
    auto items = std::tuple<queue_and_op<Ts>...>(rxs...);
    if (std::apply([&](auto &...item) { return (schedule_recv(item, oneshot) && ...); }, items)) {
        oneshot.wait_done();
    }
    std::apply([](auto &...item) { (item.op.try_cancel(), ...); }, items);
    auto result = std::optional<result_type>();
    extract_result(items, result, std::index_sequence_for<Ts...>());
    return std::move(*result);
}

} // namespace mcpp::concurrent_queue

using namespace mcpp::concurrent_queue;

TEST_CASE("select.basic") {
    for (int i = 0; i < 1000; ++i) {
        auto [red_tx, red_rx] = make_queue<const char *>(0);
        auto [blue_tx, blue_rx] = make_queue<const char *>(0);

        auto t = std::thread([red_tx = std::move(red_tx), blue_tx = std::move(blue_tx)]() mutable {
            auto t1 = std::thread([&] { CHECK(red_tx.send("Red") == op_status::success); });
            auto t2 = std::thread([&] { CHECK(blue_tx.send("Blue") == op_status::success); });
            t1.join();
            t2.join();
        });

        auto result = select_recv(red_rx, blue_rx);
        CHECK(result.index() >= 0);
        CHECK(result.index() <= 1);
        switch (result.index()) {
            case 0:
                CHECK(std::string(std::get<0>(result).value.value()) == std::string("Red"));
                CHECK(std::string(blue_rx.recv().value.value()) == std::string("Blue"));
                CHECK(blue_rx.recv().status == op_status::closed);
                CHECK(red_rx.recv().status == op_status::closed);
                break;
            case 1:
                CHECK(std::string(std::get<1>(result).value.value()) == std::string("Blue"));
                CHECK(std::string(red_rx.recv().value.value()) == std::string("Red"));
                CHECK(red_rx.recv().status == op_status::closed);
                CHECK(blue_rx.recv().status == op_status::closed);
                break;
            default:
                std::terminate();
        }
        t.join();
    }
}
