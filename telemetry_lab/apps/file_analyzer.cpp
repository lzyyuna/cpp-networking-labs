#include "telemetry/file_pipeline.hpp"
#include "telemetry/statistics.hpp"

#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{

std::size_t parse_capacity(const std::string& text)
{
    if (text.empty())
    {
        throw std::invalid_argument(
            "queue capacity must be a positive integer"
        );
    }

    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            throw std::invalid_argument(
                "queue capacity must be a positive integer"
            );
        }
    }

    const unsigned long long value = std::stoull(text);

    if (value == 0
        || value > std::numeric_limits<std::size_t>::max())
    {
        throw std::invalid_argument(
            "queue capacity must be greater than zero"
        );
    }

    return static_cast<std::size_t>(value);
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr
            << "Usage: telemetry_analyze <file_path> <queue_capacity>\n";
        return 1;
    }

    try
    {
        const std::size_t capacity = parse_capacity(argv[2]);
        const PipelineResult result = run_pipeline(argv[1], capacity);

        std::cout << "valid: " << result.valid << '\n';
        std::cout << "invalid: " << result.invalid << '\n';

        if (result.values.empty())
        {
            std::cout << "No valid data\n";
            return 0;
        }

        const Statistics stats =
            calculate_statistics(result.values);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "count: " << stats.count << '\n';
        std::cout << "min_ms: " << stats.min << '\n';
        std::cout << "max_ms: " << stats.max << '\n';
        std::cout << "mean_ms: " << stats.mean << '\n';
        std::cout << "p50_ms: " << stats.p50 << '\n';
        std::cout << "p95_ms: " << stats.p95 << '\n';
        std::cout << "p99_ms: " << stats.p99 << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
