#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct PipelineResult
{
    std::vector<double> values;
    std::size_t valid = 0;
    std::size_t invalid = 0;
};

PipelineResult run_pipeline(
    const std::string& file_path,
    std::size_t capacity
);
