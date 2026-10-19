#include "telemetry/pipeline.hpp"

#include "telemetry/bounded_queue.hpp"

#include <cmath>
#include <exception>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

PipelineResult run_pipeline(
    const std::string& file_path,
    std::size_t capacity
)
{
    BoundedQueue queue(capacity);
    PipelineResult result;

    std::exception_ptr producer_error;
    std::exception_ptr consumer_error;

    std::thread producer(
        [&]
        {
            try
            {
                std::ifstream file(file_path);

                if (!file.is_open())
                {
                    throw std::runtime_error(
                        "cannot open file: " + file_path
                    );
                }

                std::string line;

                while (std::getline(file, line))
                {
                    if (line.find_first_not_of(" \t\r\n")
                        == std::string::npos)
                    {
                        continue;
                    }

                    try
                    {
                        std::size_t pos = 0;
                        const double value = std::stod(line, &pos);

                        if (line.find_first_not_of(" \t\r\n", pos)
                                != std::string::npos
                            || !std::isfinite(value)
                            || value < 0)
                        {
                            ++result.invalid;
                            continue;
                        }

                        queue.push(value);
                        ++result.valid;
                    }
                    catch (const std::invalid_argument&)
                    {
                        ++result.invalid;
                    }
                    catch (const std::out_of_range&)
                    {
                        ++result.invalid;
                    }
                }

                if (file.bad())
                {
                    throw std::runtime_error(
                        "file read failed: " + file_path
                    );
                }
            }
            catch (...)
            {
                producer_error = std::current_exception();
            }

            queue.close();
        }
    );

    std::thread consumer;

    try
    {
        consumer = std::thread(
            [&]
            {
                try
                {
                    while (true)
                    {
                        const std::optional<double> value = queue.pop();

                        if (!value.has_value())
                        {
                            break;
                        }

                        result.values.push_back(*value);
                    }
                }
                catch (...)
                {
                    consumer_error = std::current_exception();
                    queue.close();
                }
            }
        );
    }
    catch (...)
    {
        queue.close();
        producer.join();
        throw;
    }

    producer.join();
    consumer.join();

    if (consumer_error)
    {
        std::rethrow_exception(consumer_error);
    }

    if (producer_error)
    {
        std::rethrow_exception(producer_error);
    }

    return result;
}
