struct ImageStripCheckParams {
    float* values;
    uint* invalid_flag;
    uint count, wgs_per_row;
};

[shader("compute")]
[numthreads(256, 1, 1)]
void check_finite(uint3 wg: SV_GroupID, uint tid: SV_GroupThreadID,
                  uniform ImageStripCheckParams p) {
    uint group = wg.y * p.wgs_per_row + wg.x;
    uint groups = p.count / 256 + uint(p.count % 256 != 0);
    if (group >= groups) return;
    uint index = group * 256 + tid;
    if (index < p.count &&
        (asuint(p.values[index]) & 0x7f800000u) == 0x7f800000u) {
        uint previous;
        InterlockedOr(p.invalid_flag[0], 1u, previous);
    }
}
