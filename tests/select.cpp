// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#include <chrono>
#include <iostream>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

// TODO: Hack hack hack
#define private public
#define protected public
#include "mcpp/concurrent-queue/concurrent_queue.hpp"
#include "mcpp/concurrent-queue/oneshot.hpp"
#undef protected
#undef private
#include <catch2/catch_test_macros.hpp>

namespace mcpp::concurrent_queue {

template <typename F, size_t... Is>
constexpr auto index_apply(std::index_sequence<Is...> /* is */, F f) {
    return f(std::integral_constant<size_t, Is>{}...);
}

template <typename... Ts>
auto select_recv(queue_receiver<Ts> &...rxs) -> std::variant<recv_result<Ts>...> {
    static constexpr auto Is = std::index_sequence_for<Ts...>();
    auto oneshot = sync_oneshot();
    auto chans = std::tuple<synchronized_value<Chan<Ts>> &...>(rxs.queue_->chan_...);
    auto ops = std::tuple<pending_recv<Ts>...>(((void)(Ts *)(nullptr), &oneshot)...);

    index_apply(Is, [&](auto... I) { //
        (std::get<I>(chans).lock()->push(std::get<I>(ops)) && ...);
    });
    oneshot.wait_done();
    index_apply(Is, [&](auto... I) { //
        (std::get<I>(chans).lock()->pending_recvs.try_remove(&std::get<I>(ops)), ...);
    });
    assert(oneshot.done());

    auto result = std::optional<std::variant<recv_result<Ts>...>>();
    index_apply(Is, [&](auto... I) { //
        (void)(((!result && std::get<I>(ops).result_)
                    ? (result.emplace(std::in_place_index<I>, std::move(*std::get<I>(ops).result_)), true)
                    : false) //
               || ...);
    });
    assert(result.has_value());
    return std::move(*result);
}

} // namespace mcpp::concurrent_queue

using namespace mcpp::concurrent_queue;
using namespace std::literals;

class jthread : public std::thread {
  public:
    using std::thread::thread;
    ~jthread() {
        if (joinable()) {
            join();
        }
    }
};

TEST_CASE("select.basic") {
    static constexpr auto n_batches = 100;
    static constexpr auto n_messages = 1000;
    for (int b = 0; b < n_batches; ++b) {
        auto [red_tx, red_rx] = make_queue<const char *>(0);
        auto [blue_tx, blue_rx] = make_queue<const char *>(0);
        auto t1 = jthread([red_tx = std::move(red_tx)]() mutable {
            for (int i = 0; i < n_messages; ++i) {
                CHECK(red_tx.send("Red") == op_status::success);
            }
        });
        auto t2 = jthread([blue_tx = std::move(blue_tx)]() mutable {
            for (int i = 0; i < n_messages; ++i) {
                CHECK(blue_tx.send("Blue") == op_status::success);
            }
        });

        auto n_selects = 0;
        auto n_red = 0;
        auto n_blue = 0;
        while (red_rx && blue_rx) {
            // printf("n_red=%d, n_blue=%d\n", n_red, n_blue);
            auto result = select_recv(red_rx, blue_rx);
            n_selects += 1;
            switch (result.index()) {
                case 0: {
                    auto &r = std::get<0>(result);
                    if (r.status == op_status::success) {
                        CHECK(r.value.has_value());
                        CHECK(r.value.value() == "Red"s);
                        ++n_red;
                    } else {
                        CHECK(n_red == n_messages);
                        CHECK(r.status == op_status::closed);
                        // printf("Red closed\n");
                        red_rx = {};
                        CHECK(!red_rx);
                    }
                    break;
                }
                case 1: {
                    auto &r = std::get<1>(result);
                    if (r.status == op_status::success) {
                        CHECK(r.value.has_value());
                        CHECK(r.value.value() == "Blue"s);
                        ++n_blue;
                    } else {
                        CHECK(n_blue == n_messages);
                        CHECK(r.status == op_status::closed);
                        // printf("Blue closed\n");
                        blue_rx = {};
                        CHECK(!blue_rx);
                    }
                    break;
                }
                default:
                    REQUIRE(result.index() < 2);
                    break;
            }
            // fflush(stdout);
        }
        // printf("After select loop: n_red=%d, n_blue=%d\n", n_red, n_blue);
        CHECK(n_selects == n_red + n_blue + 1);

        auto drain = [](auto &rx, auto &count, std::string expected) {
            for (auto val : rx) {
                CHECK(val == expected);
                ++count;
            }
            CHECK(count == n_messages);
            rx = {};
        };
        if (!red_rx) {
            // printf("Draining blue\n");
            drain(blue_rx, n_blue, "Blue"s);
        } else {
            // printf("Draining red\n");
            drain(red_rx, n_red, "Red"s);
        }

        CHECK(n_red == n_messages);
        CHECK(n_blue == n_messages);
    }
    // printf("");
}
