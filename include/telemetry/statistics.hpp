#pragma once

#include <cstddef>
#include <vector>

struct Statistics
{
    std::size_t count;
    double min;
    double max;
    double mean;
};

Statistics calculate_statistics(
    const std::vector<double>& values
);