// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "mcpp/concurrent-queue/concurrent_queue.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <iostream>
#include <thread>

TEST_CASE("queue.basic") {
    auto queue = mcpp::concurrent_queue::concurrent_queue<int>(3);

    std::thread t1([&] {
        auto r_res = queue.recv();
        CHECK(r_res.status == mcpp::concurrent_queue::op_status::success);
        CHECK(r_res.value.has_value());
        CHECK(r_res.value.value() == 1);
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));
    queue.try_send(1);
    t1.join();

    queue.try_send_for(2, std::chrono::seconds(1));
    queue.send(3);
    queue.send(4);

    auto ts_res = queue.try_send_for(5, std::chrono::seconds(1));
    CHECK(ts_res == mcpp::concurrent_queue::try_op_status::wouldblock);

    auto tr_res = queue.try_recv();
    CHECK(tr_res.status == mcpp::concurrent_queue::try_op_status::success);
    CHECK(tr_res.value.has_value());
    CHECK(tr_res.value.value() == 2);

    auto r_res = queue.recv();
    CHECK(r_res.status == mcpp::concurrent_queue::op_status::success);
    CHECK(r_res.value.has_value());
    CHECK(r_res.value.value() == 3);

    tr_res = queue.try_recv_for(std::chrono::seconds(1));
    CHECK(tr_res.status == mcpp::concurrent_queue::try_op_status::success);
    CHECK(tr_res.value.has_value());
    CHECK(tr_res.value.value() == 4);

    tr_res = queue.try_recv_for(std::chrono::seconds(1));
    CHECK(tr_res.status == mcpp::concurrent_queue::try_op_status::wouldblock);
    CHECK(!tr_res.value.has_value());

    queue.close();
}