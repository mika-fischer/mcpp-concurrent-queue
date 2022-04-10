# concurrent-queue

TODO

## Inspiration
- Go
  - [Channel](https://go.dev/ref/spec#Channel_types)
- Rust
  - [mpsc](https://github.com/rust-lang/rust/tree/master/library/std/src/sync/mpsc)
  - [crossbeam](https://github.com/crossbeam-rs/crossbeam)
  - [flume](https://github.com/zesterer/flume)
- C++
  - [Concurrent Queues (p0260)](https://wg21.link/p0260)
  - [Concurrent Buffer Queue (p1958)](https://wg21.link/p1958)
  - [Boost thread synchronized queues](https://www.boost.org/doc/libs/develop/doc/html/thread/sds.html#thread.sds.synchronized_queues)
  - [Google concurrency library](https://github.com/alasdairmackintosh/google-concurrency-library)
  - [oneTBB](https://oneapi-src.github.io/oneTBB/main/tbb_userguide/Concurrent_Queue_Classes.html)

## TODO

- Tests
  - move-only types
  - copy-only types
  - Order
  - Count copies & moves
- Drain functionality (remove all elements currently in queue)
- Select functionality
  - atomic cancelation
- Different storage options
  - unbounded / bounded / rendezvous (size = 0)
  - unbounded -> `std::deque`
  - bounded -> ring buffer on std::vector
  - rendezvous -> nothing
- spinlocks
- Split out some things into own libs
  - spinlock, thread_parker



## License
This software is licensed under the [Boost Software License - Version 1.0](https://www.boost.org/LICENSE_1_0.txt).