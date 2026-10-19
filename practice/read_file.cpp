#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <cmath>

int main()
{
    std::ifstream file("data/sample.txt");

    if (!file.is_open())
    {
        std::cerr << "Cannot open file\n";
        return 1;
    }

    std::string line;

    int valid = 0;
    int invalid = 0;

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

           
            double value = std::stod(line, &pos);

          
            if (line.find_first_not_of(" \t\r\n", pos)
                != std::string::npos)
            {
                throw std::invalid_argument(
                    "unexpected characters after number"
                );
            }

            
            if (!std::isfinite(value) || value < 0)
            {
                throw std::invalid_argument(
                    "invalid latency"
                );
            }

           
            ++valid;
            std::cout << value << '\n';
        }
        catch (const std::invalid_argument& e)
        {
            ++invalid;
            std::cerr << "Invalid number: "
                      << line << '\n';
        }
        catch (const std::out_of_range& e)
        {
            ++invalid;
            std::cerr << "Number out of range: "
                      << line << '\n';
        }
    }

    std::cout << "valid: " << valid << '\n';
    std::cout << "invalid: " << invalid << '\n';

    return 0;
}