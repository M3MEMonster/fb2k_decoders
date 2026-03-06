#pragma once

#include <cstdint>

namespace core {

struct StereoSample {
    float left;
    float right;

    StereoSample operator*(float v) const { return { left * v, right * v }; }
    StereoSample& operator+=(const StereoSample& o) { left += o.left; right += o.right; return *this; }
};

}

using core::StereoSample;
