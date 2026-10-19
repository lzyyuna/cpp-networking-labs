#pragma once

#include <cstddef>
#include <vector>

struct Statistics
{
    std::size_t count;
    double min;
    double max;
    double mean;

    double p50;
    double p95;
    double p99;
};

Statistics calculate_statistics(
    const std::vector<double>& values
);