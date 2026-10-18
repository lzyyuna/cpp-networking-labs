#include "telemetry/statistics.hpp"

#include <stdexcept>

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

    stats.mean = sum / stats.count;

    return stats;
}