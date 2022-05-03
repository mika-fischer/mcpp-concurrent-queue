// Copyright Mika Fischer 2022.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#ifdef __APPLE__
#include "thread_parker_ulock.hpp"
namespace mcpp::concurrent_queue {
using thread_parker = thread_parker_ulock;
}
#else
#include "thread_parker_generic.hpp"
namespace mcpp::concurrent_queue {
using thread_parker = thread_parker_generic<>;
}
#endif