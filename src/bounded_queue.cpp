#include "telemetry/bounded_queue.hpp"

#include <stdexcept>

BoundedQueue::BoundedQueue(std::size_t capacity)
    : capacity_(capacity)
{
    if (capacity == 0)
    {
        throw std::invalid_argument(
            "queue capacity must be greater than zero"
        );
    }
}

void BoundedQueue::push(double value)
{
    std::unique_lock<std::mutex> lock(mutex_);

    not_full_.wait(
        lock,
        [this]
        {
            return closed_ || queue_.size() < capacity_;
        }
    );

    if (closed_)
    {
        throw std::logic_error(
            "cannot push to a closed queue"
        );
    }

    queue_.push(value);

    lock.unlock();
    not_empty_.notify_one();
}

std::optional<double> BoundedQueue::pop()
{
    std::unique_lock<std::mutex> lock(mutex_);

    not_empty_.wait(
        lock,
        [this]
        {
            return closed_ || !queue_.empty();
        }
    );

    if (queue_.empty())
    {
        return std::nullopt;
    }

    const double value = queue_.front();
    queue_.pop();

    lock.unlock();
    not_full_.notify_one();

    return value;
}

void BoundedQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    not_empty_.notify_all();
    not_full_.notify_all();
}