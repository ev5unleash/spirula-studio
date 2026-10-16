#include "engine/EngineDiagnostic.h"
#include "core/Tensor.h"
#include "data/Json.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
EngineDiagnosticField& field(EngineDiagnosticSnapshot& s, const std::string& name) {
    for (auto& f : s.fields) if (f.name == name) return f;
    throw std::runtime_error("missing fixture field " + name);
}
template<class T> void put(EngineDiagnosticSnapshot& s, const std::string& name, size_t cell, T value) {
    auto& bytes = field(s, name).bytes;
    std::memcpy(bytes.data()+cell*sizeof(T), &value, sizeof(T));
}
EngineDiagnosticSnapshot snapshot() {
    EngineDiagnosticSnapshot s;
    s.rows = s.max_rows = 320;
    s.num_sh = 1; s.owner = 1; s.source_id = 7;
    s.owned_blocks = {0, 1}; s.host_signature = "same-input-schedule";
    auto add = [&](const std::string& name, NpyScalar dtype, size_t bytes) {
        s.fields.push_back({name, uint8_t(dtype), true, std::vector<uint8_t>(bytes, 0)});
    };
    const char* names[] = {"means", "quats", "scales", "opacities", "features_dc"};
    const int dims[] = {3, 4, 3, 1, 3};
    for (int i = 0; i < 5; ++i) {
        add(std::string("world.")+names[i], NpyScalar::f4, 320*dims[i]*4);
        add(std::string("eng.")+names[i]+"_qfpbo.q", NpyScalar::u1, 320*dims[i]*4);
        add(std::string("eng.")+names[i]+"_qfpbo.qb", NpyScalar::f4, 2*16);
    }
    const size_t shbytes = size_t(sh_fpbo_cells(320, 1))*2;
    add("eng.sh_quant_fpbo.q", NpyScalar::u1, shbytes);
    add("eng.sh_quant_fpbo.qb", NpyScalar::f4, 2*16);
    add("eng.world.sh_vq16_fpbo.q", NpyScalar::u1, shbytes);
    add("eng.world.sh_vq16_fpbo.qb", NpyScalar::f4, 2*8);
    add("eng.bias_correction_steps", NpyScalar::i4, 320*4);
    add("eng.accum_buffer", NpyScalar::f4, 320*8);
    add("eng.radii", NpyScalar::f4, 320*4);
    return s;
}
struct Result { bool accepted; JsonValue summary; };
Result compare(const EngineDiagnosticSnapshot& s, const EngineDiagnosticSnapshot& a,
               const EngineDiagnosticSnapshot& b, int updates) {
    std::ostringstream output;
    const bool accepted = engine_diagnostic_compare(s, a, b, output, "regression", updates, {});
    const auto text = output.str();
    const auto start = text.rfind("{\"type\":\"diagnostic_summary\"");
    if (start == std::string::npos) throw std::runtime_error("missing comparison summary");
    return {accepted, json_parse(text.substr(start))};
}
int64_t count(const Result& result, const std::string& name) {
    const auto* value = result.summary.find(name);
    if (!value || value->type != JsonValue::Type::Number) throw std::runtime_error("missing summary count " + name);
    return value->as_int();
}
} // namespace

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL %s\n", name); ++failures; }
    };
    try {
        const auto baseline = snapshot();
        auto reference = baseline;
        auto actual = reference;
        put(actual, "world.means", 319*3, 5e-5f);
        auto immediate = compare(baseline, reference, actual, 0);
        check(!immediate.accepted && count(immediate, "decoded_errors") == 0 &&
              count(immediate, "immediate_serialized_logical_byte_errors") > 0,
              "subthreshold owned tail value is not an exact native roundtrip");
        actual = reference;
        field(actual, "eng.means_qfpbo.q").bytes[319*3*4] = 1;
        immediate = compare(baseline, reference, actual, 0);
        check(!immediate.accepted && count(immediate, "decoded_errors") == 0 &&
              count(immediate, "immediate_serialized_logical_byte_errors") > 0,
              "equal decoded moments do not waive immediate serialized code change");
        auto padded = reference;
        const auto sh_address = sh_quant_addr(1, 0);
        field(padded, "eng.sh_quant_fpbo.q").bytes[size_t(sh_address.cell(sh_address.base(319), 3))*2] = 1;
        field(padded, "eng.world.sh_vq16_fpbo.q").bytes[size_t(sh_address.cell(sh_address.base(320), 0))*2] = 1;
        const auto padding_only = compare(baseline, reference, padded, 0);
        check(padding_only.accepted && count(padding_only, "immediate_logical_byte_errors") > 0 &&
              count(padding_only, "immediate_serialized_logical_byte_errors") == 0,
              "odd SH lane and invalid tail padding remain raw diagnostics only");
        for (size_t row = 256; row < 320; ++row) {
            put(reference, "eng.bias_correction_steps", row, int32_t(1));
            put(actual, "eng.bias_correction_steps", row, int32_t(1));
        }
        put(actual, "world.means", 319*3, 5e-5f);
        auto continued = compare(baseline, reference, actual, 1);
        check(continued.accepted && count(continued, "decoded_errors") == 0,
              "finite postupdate owned values and raw codes retain unchanged Qscreen");
        put(actual, "eng.means_qfpbo.qb", 4, std::numeric_limits<float>::quiet_NaN());
        auto invalid_bounds = compare(baseline, reference, actual, 1);
        check(!invalid_bounds.accepted && count(invalid_bounds, "decoded_errors") == 0 &&
              count(invalid_bounds, "nonfinite_bound_errors") == 1,
              "native sanitized zero moments do not waive nonfinite owned bounds");
        check(count(invalid_bounds, "metadata_errors") == 0 && count(invalid_bounds, "clock_errors") == 0,
              "nonfinite rejection comes from numerical contract not malformed fixture");
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what()); ++failures;
    }
    if (!failures) std::printf("PASS native snapshot comparator regression\n");
    return failures ? 1 : 0;
}
