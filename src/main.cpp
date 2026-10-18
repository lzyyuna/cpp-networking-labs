#include "telemetry/statistics.hpp"

#include <iomanip>
#include <iostream>
#include <vector>


int main()
{
    std::vector<double> values = {
        8.2, 5.1, 12.4, 6.8, 9.3
    };

    try
    {
        Statistics stats = calculate_statistics(values);

        std::cout << std::fixed << std::setprecision(2);

        std::cout << "count: " << stats.count << std::endl;
        std::cout << "min_ms: " << stats.min << std::endl;
        std::cout << "max_ms: " << stats.max << std::endl;
        std::cout << "mean_ms: " << stats.mean << std::endl;
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr << "error: " << error.what() << std::endl;
        return 1;
    }

    return 0;
}