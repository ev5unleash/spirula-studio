#pragma once


// Linear working colour space: Adam runs on x = splat_dc_encode(dc) and every
// colour step is clipped to a trust region. FPBO does this inline; the
// non-FPBO DC/SH kernels take the same inputs here so both paths agree.
struct ColorTrustState {
    bool  enabled = false;
    float eps_tr  = 1e-6f;
    // [N,3] DC and [N] pre-sigmoid opacity; only the SH kernels read them.
    const float* features_dc = nullptr;
    const float* opacities   = nullptr;
};
