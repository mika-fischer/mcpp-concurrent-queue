include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.0.0-preview4
)
FetchContent_MakeAvailable(Catch2)

add_executable(tests
    basic.cpp
    concurrent_queue.cpp
    select.cpp
    thread_parker.cpp
)
target_link_libraries(tests
    PRIVATE
        mcpp::concurrent-queue
        Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tests)