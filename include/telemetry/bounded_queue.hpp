#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>

class BoundedQueue
{
public:
    explicit BoundedQueue(std::size_t capacity);

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    void push(double value);

    std::optional<double> pop();

    void close();

private:
    std::size_t capacity_;
    std::queue<double> queue_;

    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;

    bool closed_ = false;
};