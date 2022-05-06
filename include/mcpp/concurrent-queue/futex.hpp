// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <atomic>
#include <cstdint>

using futex_type = std::uint32_t;
auto futex_wait(std::atomic<futex_type> &futex, futex_type expected, std::uint64_t timeout = 0) noexcept -> bool;
auto futex_wake(std::atomic<futex_type> &futex) noexcept -> bool;

#if defined(__APPLE__)
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_THREAD 0x00000200
#define ULF_NO_ERRNO 0x01000000
extern "C" {
extern auto __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us) -> int;
extern auto __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value) -> int;
}

auto futex_wait(std::atomic<futex_type> &futex, futex_type expected, uint64_t timeout = 0) noexcept -> bool {
    do {
        uint32_t timeout_us = 0;
        int ret = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &futex, static_cast<uint64_t>(expected), timeout_us);
        if (ret >= 0) {
            if (try_resume()) {
                return true;
            }
            continue;
        }
        switch (-res) {
            case EINTR:
            case EFAULT:
                continue;
            case ETIMEDOUT:
                return false;
            default:
                assert(false);
                std::terminate();
        }
    } while (state_.load() == state::suspended);
    int res =
        __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &state_, static_cast<uint64_t>(state::suspended), timeout_us);
}

auto futex_wake(std::atomic<futex_type> &futex) noexcept -> bool {
    while (true) {
        auto ret = __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &futex, 0u);
        if (ret >= 0) {
            return ret > 0;
        }
        switch (-ret) {
            case EINTR:
            case EFAULT:
                continue;
            default:
                // TODO: std::terminate();
                return false;
        }
    }
}

#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

// TODO: untested
auto futex_wait(std::atomic<uint32_t> &futex, uint32_t expected, uint64_t timeout = 0) -> bool {
    // TODO: create timespec
    while (true) {
        if (futex.load(std::memory_order_relaxed) != expected) {
            return true;
        }
        auto ret = syscall(SYS_futex, futex, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, expected, nullptr, 0,
                           FUTEX_BITSET_MATCH_ANY);
        if (ret < 0) {
            switch (errno) {
                case ETIMEDOUT:
                    return false;
                default:
                    break;
            }
        }
    }
}

auto futex_wake(std::atomic<uint32_t> &futex) noexcept -> bool {
    return syscall(SYS_futex, futex, FUTEX_WAKE_PRIVATE, 1) > 0
}

#endif