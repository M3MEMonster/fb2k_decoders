#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace core {

template<typename T>
T Min(T a, T b) { return (a < b) ? a : b; }

template<typename T>
T Max(T a, T b) { return (a > b) ? a : b; }

}
