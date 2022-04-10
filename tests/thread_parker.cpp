// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "mcpp/concurrent-queue/thread_parker.hpp"
#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <thread>

TEST_CASE("thread_parker.basic") {
    auto parker = mcpp::concurrent_queue::thread_parker<>();
    auto thread = std::thread([&] {
        std::cout << "parking thread" << std::endl;
        parker.suspend();
        std::cout << "thread woke up" << std::endl;
    });
    std::this_thread::sleep_for(std::chrono::seconds(1));
    parker.resume();
    thread.join();
}