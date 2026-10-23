#include "telemetry/statistics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{

double calculate_percentile(
    const std::vector<double>& sorted_values,
    double percentile
)
{
    if (sorted_values.empty())
    {
        throw std::invalid_argument(
            "cannot calculate percentile of empty data"
        );
    }

    const std::size_t index =
        static_cast<std::size_t>(
            std::ceil(percentile * sorted_values.size())
        ) - 1;

    return sorted_values[index];
}

} // namespace


Statistics calculate_statistics(
    const std::vector<double>& values
)
{
    if (values.empty())
    {
        throw std::invalid_argument(
            "cannot calculate statistics of empty data"
        );
    }

    Statistics stats;

    stats.count = values.size();
    stats.min = values[0];
    stats.max = values[0];

    double sum = 0.0;

    for (double value : values)
    {
        if (value < stats.min)
        {
            stats.min = value;
        }

        if (value > stats.max)
        {
            stats.max = value;
        }

        sum += value;
    }

    stats.mean = sum / static_cast<double>(stats.count);

    std::vector<double> sorted_values = values;

    std::sort(
        sorted_values.begin(),
        sorted_values.end()
    );

    stats.p50 = calculate_percentile(
        sorted_values,
        0.50
    );

    stats.p95 = calculate_percentile(
        sorted_values,
        0.95
    );

    stats.p99 = calculate_percentile(
        sorted_values,
        0.99
    );

    return stats;
}