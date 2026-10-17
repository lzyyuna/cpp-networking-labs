#include <iostream>
#include <vector>
#include <stdexcept>
#include <iomanip>

struct Statistics
{
    std::size_t count;
    double min;
    double max;
    double mean;
};

Statistics calculate_statistics(const std::vector<double>& values)
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