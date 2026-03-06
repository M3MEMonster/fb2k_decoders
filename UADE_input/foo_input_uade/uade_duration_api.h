#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct uade_flat_entry {
    uint64_t hash;
    int subsong;
    double duration;
    std::string name;
};

std::vector<uade_flat_entry> uade_get_all_duration_entries();
