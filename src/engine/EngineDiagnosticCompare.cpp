#include "engine/EngineDiagnostic.h"
#include "core/Tensor.h"
#include "core/Sha256.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using Field = EngineDiagnosticField;
using Snapshot = EngineDiagnosticSnapshot;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error("diagnostic comparator: " + message);
}

void quoted(std::ostream& out, const std::string& text) {
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 32) {
                const char* hex = "0123456789abcdef";
                out << "\\u00" << hex[c >> 4] << hex[c & 15];
            } else out << char(c);
        }
    }
    out << '"';
}

template<class T> T load(const Field& f, size_t index) {
    T value;
    std::memcpy(&value, f.bytes.data() + index * sizeof(T), sizeof(T));
    return value;
}

struct Metric {
    uint64_t cells = 0, finite = 0, nonfinite_equal = 0, nonfinite_bad = 0, outside = 0;
    double maxabs = 0, sumsq = 0, refsq = 0, worst = 0;
    int64_t worst_row = -1, first_bad_row = -1, first_nonfinite_row = -1;
    int worst_component = -1;

    void add(float x, float y, int64_t row, int component) {
        ++cells;
        bool bad;
        if (!q_finite(x) || !q_finite(y)) {
            if (first_nonfinite_row < 0) first_nonfinite_row = row;
            bad = std::memcmp(&x, &y, sizeof(float)) != 0;
            nonfinite_bad += bad;
            nonfinite_equal += !bad;
        } else {
            ++finite;
            double delta = std::abs(double(x) - double(y));
            double screen = std::min(delta, delta / (1e-6 + std::abs(double(x))));
            maxabs = std::max(maxabs, delta);
            sumsq += delta * delta;
            refsq += double(x) * double(x);
            if (worst_row < 0 || screen > worst) {
                worst = screen; worst_row = row; worst_component = component;
            }
            bad = screen > 1e-4;
            outside += bad;
        }
        if (bad && first_bad_row < 0) first_bad_row = row;
    }

    uint64_t errors() const { return outside + nonfinite_bad; }
    void emit(std::ostream& out, const std::string& name, const char* scope,
              bool acceptance, bool descriptive = false) const {
        out << "{\"type\":\"diagnostic_field\",\"field\":"; quoted(out, name);
        out << ",\"scope\":"; quoted(out, scope);
        out << ",\"acceptance\":" << (acceptance && !descriptive ? "true" : "false")
            << ",\"cells\":" << cells << ",\"finite\":" << finite
            << ",\"equal_nonfinite_bits\":" << nonfinite_equal
            << ",\"nonfinite_mismatch\":" << nonfinite_bad
            << ",\"maxabs\":" << maxabs
            << ",\"relRMS\":" << std::sqrt(sumsq / std::max(refsq, 1e-30));
        if (!descriptive) out << ",\"outside\":" << outside << ",\"worst_screen\":" << worst;
        out << ",\"worst_row\":" << worst_row
            << ",\"worst_component\":" << worst_component << "}\n";
    }
};

using Fields = std::map<std::string, const Field*>;
Fields index(const Snapshot& s) {
    Fields fields;
    for (const auto& f : s.fields)
        require(fields.emplace(f.name, &f).second, "duplicate field " + f.name);
    return fields;
}
const Field& get(const Fields& fields, const std::string& name, NpyScalar dtype, size_t bytes) {
    auto it = fields.find(name);
    require(it != fields.end(), "missing field " + name);
    const auto& f = *it->second;
    require(f.dtype == uint8_t(dtype) && f.bytes.size() == bytes, "incompatible dtype/size " + name);
    return f;
}
uint64_t different(const Field& x, const Field& y, size_t offset, size_t size) {
    uint64_t count = 0;
    for (size_t i = offset; i < offset + size; ++i) count += x.bytes[i] != y.bytes[i];
    return count;
}
float2 adam16(const Field& q, size_t cell, float4 bounds) {
    uint16_t code[2];
    std::memcpy(code, q.bytes.data() + cell * sizeof(code), sizeof(code));
    return QuantizedAdamState<16>::decode_g1g2(reinterpret_cast<const uint8_t*>(code), 0, bounds);
}
float shvalue16(const Field& q, size_t cell, float2 bounds) {
    uint16_t code;
    std::memcpy(&code, q.bytes.data() + cell * sizeof(code), sizeof(code));
    return QuantizedTensor<16>::decode_v(reinterpret_cast<const uint8_t*>(&code), 0, bounds);
}

void hex_bytes(std::ostream& out, const Field& f, size_t offset, size_t bytes) {
    const char* hex = "0123456789abcdef";
    out << '"';
    for (size_t i = offset; i < offset + bytes; ++i)
        out << hex[f.bytes[i] >> 4] << hex[f.bytes[i] & 15];
    out << '"';
}

// One original-row block plus small shared inputs, not three full snapshots.
void retain_first(const Snapshot& s, const Snapshot& a, const Snapshot& b,
                  const Fields& sf, const Fields& af, const Fields& bf,
                  const std::string& arm, int updates, int64_t row,
                  const std::string& cause, const std::filesystem::path& root) {
    if (root.empty()) return;
    require(arm.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos,
            "unsafe detail arm");
    std::filesystem::create_directories(root);
    auto path = root / ("first-mismatch-" + arm + "-" + std::to_string(updates) + ".jsonl");
    require(!std::filesystem::exists(path), "detail already exists");
    std::ofstream out(path, std::ios::binary);
    require(bool(out), "cannot create mismatch detail");
    const int64_t block = std::max<int64_t>(0, row) / 256;
    out << "{\"type\":\"diagnostic_first_mismatch\",\"row\":" << row << ",\"block\":" << block
        << ",\"cause\":"; quoted(out, cause);
    out << ",\"updates\":" << updates << ",\"rows\":" << s.rows << ",\"num_sh\":" << s.num_sh
        << ",\"owner\":" << s.owner << ",\"source_id\":" << s.source_id << ",\"host_baseline\":";
    quoted(out, s.host_signature); out << ",\"host_reference\":"; quoted(out, a.host_signature);
    out << ",\"host_actual\":"; quoted(out, b.host_signature);
    out << ",\"host_state_baseline\":"; quoted(out, s.host_state);
    out << ",\"host_state_reference\":"; quoted(out, a.host_state);
    out << ",\"host_state_actual\":"; quoted(out, b.host_state); out << "}\n";
    size_t retained = 0;
    constexpr size_t budget = 4 * 1024 * 1024;
    for (const auto& [name, fp] : sf) {
        const auto& f = *fp;
        size_t offset = 0, bytes = f.bytes.size();
        if (bytes > 65536) {
            const bool codec = name.rfind("eng.means_qfpbo.", 0) == 0 || name.rfind("eng.quats_qfpbo.", 0) == 0 ||
                name.rfind("eng.scales_qfpbo.", 0) == 0 || name.rfind("eng.opacities_qfpbo.", 0) == 0 ||
                name.rfind("eng.features_dc_qfpbo.", 0) == 0 || name.rfind("eng.sh_quant_fpbo.", 0) == 0 ||
                name.rfind("eng.world.sh_vq16_fpbo.", 0) == 0;
            const bool bounds = codec && name.size() >= 3 && name.substr(name.size()-3) == ".qb";
            const bool sh = name == "eng.sh_quant_fpbo.q" || name == "eng.world.sh_vq16_fpbo.q";
            const bool row_field = name == "world.means" || name == "world.quats" || name == "world.scales" ||
                name == "world.opacities" || name == "world.features_dc" || name == "eng.accum_buffer" ||
                name == "eng.radii" || name == "eng.bias_correction_steps" || name == "eng.densify.oversize" ||
                name == "eng.densify.sample_score";
            if (bounds || sh || codec || row_field) {
                const size_t stride = bounds || sh ? bytes / s.owned_blocks.size() : bytes / size_t(s.rows) * 256;
                require(stride > 0 && size_t(block) < (bytes+stride-1)/stride, "invalid detail field " + name);
                offset = size_t(block) * stride;
                bytes = std::min(stride, f.bytes.size() - offset);
            } else {
                const auto& x = *af.at(name);
                const auto& y = *bf.at(name);
                auto digest = [](const Field& field) {
                    spirula::Sha256 sha;
                    sha.update(field.bytes.data(), field.bytes.size());
                    return sha.hex();
                };
                const auto baseline_digest = digest(f);
                const bool equal_a = f.bytes == x.bytes, equal_b = f.bytes == y.bytes;
                out << "{\"type\":\"diagnostic_large_shared_identity\",\"field\":"; quoted(out, name);
                out << ",\"total_bytes\":" << bytes << ",\"baseline_sha256\":"; quoted(out, baseline_digest);
                out << ",\"reference_sha256\":"; quoted(out, equal_a ? baseline_digest : digest(x));
                out << ",\"actual_sha256\":"; quoted(out, equal_b ? baseline_digest : digest(y)); out << "}\n";
                while (offset < bytes && f.bytes[offset] == x.bytes[offset] && f.bytes[offset] == y.bytes[offset]) ++offset;
                if (offset == bytes) continue;
                offset = offset > 256 ? offset-256 : 0;
                bytes = std::min(size_t(4096), f.bytes.size()-offset);
            }
        }
        require(bytes <= (budget - retained) / 3, "first mismatch detail exceeds 4 MiB budget");
        retained += bytes * 3;
        out << "{\"type\":\"diagnostic_state_bytes\",\"field\":"; quoted(out, name);
        out << ",\"dtype\":" << unsigned(f.dtype) << ",\"serialized\":" << (f.serialized ? "true" : "false")
            << ",\"offset\":" << offset << ",\"bytes\":" << bytes << ",\"baseline\":";
        hex_bytes(out, f, offset, bytes); out << ",\"reference\":";
        hex_bytes(out, *af.at(name), offset, bytes); out << ",\"actual\":";
        hex_bytes(out, *bf.at(name), offset, bytes); out << "}\n";
    }
    out.flush(); require(bool(out), "failed writing mismatch detail");
}
} // namespace

bool engine_diagnostic_compare(const Snapshot& s, const Snapshot& a, const Snapshot& b,
    std::ostream& out, const std::string& arm, int updates, const std::filesystem::path& detail_root,
    bool descriptive, bool reference_all_owned) {
    uint64_t decoded_errors = 0, clock_errors = 0, invariant_errors = 0, metadata_errors = 0, logical_errors = 0;
    uint64_t serialized_logical_errors = 0, nonfinite_bound_errors = 0;
    int64_t first_row = -1;
    std::string first_field;
    auto first = [&](const std::string& field, int64_t row) {
        if (first_field.empty()) { first_field = field; first_row = row; }
    };
    out << std::setprecision(17);
    try {
        require(updates >= 0 && s.rows > 0 && s.rows <= INT32_MAX && s.max_rows == s.rows &&
                s.num_sh > 0 && s.num_sh <= 255 && s.owner >= 0 && s.owner <= 2 && s.source_id != 0,
                "unsupported source dimensions/owner");
        const size_t n = size_t(s.rows), blocks = (n + 255) / 256;
        require(s.owned_blocks.size() == blocks, "baseline mask size");
        for (auto owned : s.owned_blocks) require(owned <= 1, "baseline mask value");
        for (const auto* candidate : {&a, &b}) {
            require(candidate->rows == s.rows && candidate->max_rows == s.max_rows && candidate->num_sh == s.num_sh &&
                    candidate->source_id == s.source_id &&
                    ((reference_all_owned && candidate == &a && candidate->owner == 2) ||
                     (candidate->owner == s.owner && candidate->owned_blocks == s.owned_blocks)),
                    "snapshot metadata/mask mismatch");
        }
        auto sf = index(s), af = index(a), bf = index(b);
        require(sf.size() == af.size() && sf.size() == bf.size(), "field-set mismatch");
        for (const auto& [name, f] : sf) {
            for (const auto* fields : {&af, &bf}) {
                auto it = fields->find(name);
                require(it != fields->end() && it->second->dtype == f->dtype &&
                        it->second->serialized == f->serialized && it->second->bytes.size() == f->bytes.size(),
                        "field schema mismatch " + name);
            }
            if (updates == 0) {
                const auto& x = *af.at(name);
                const auto& y = *bf.at(name);
                const uint64_t errors = different(x, y, 0, f->bytes.size());
                logical_errors += errors;
                uint64_t required_errors = 0;
                const bool required = f->serialized && name != "eng.radii";
                if (required) {
                    if (name == "eng.sh_quant_fpbo.q" || name == "eng.world.sh_vq16_fpbo.q") {
                        const size_t cells = size_t(sh_fpbo_cells(s.rows, uint32_t(s.num_sh)));
                        require(x.bytes.size() == cells*2, "immediate SH packed schema " + name);
                        const auto address = sh_quant_addr(uint32_t(s.num_sh), 0);
                        for (size_t row = 0; row < n; ++row)
                            for (int c = 0; c < 3*s.num_sh; ++c)
                                required_errors += different(x, y, size_t(address.cell(address.base(int64_t(row)), c))*2, 2);
                    } else required_errors = errors;
                    serialized_logical_errors += required_errors;
                }
                out << "{\"type\":\"diagnostic_immediate_logical\",\"field\":"; quoted(out, name);
                out << ",\"different_bytes\":" << errors << ",\"serialized_logical_different_bytes\":" << required_errors
                    << ",\"acceptance\":" << (required ? "true" : "false") << "}\n";
                if (errors && first_field.empty()) {
                    size_t offset = 0;
                    while (offset < x.bytes.size() && x.bytes[offset] == y.bytes[offset]) ++offset;
                    int64_t row = -1;
                    const bool bounds = name.size() >= 3 && name.substr(name.size()-3) == ".qb";
                    const bool sh = name == "eng.sh_quant_fpbo.q" || name == "eng.world.sh_vq16_fpbo.q";
                    if ((bounds || sh) && x.bytes.size()/blocks > 0)
                        row = int64_t(offset / (x.bytes.size()/blocks) * 256);
                    else if (x.bytes.size() >= n && x.bytes.size()%n == 0)
                        row = int64_t(offset / (x.bytes.size()/n));
                    first(name, row);
                }
            }
        }
        const bool host_equal = reference_all_owned
            ? a.common_host_signature == b.common_host_signature &&
                b.role_host_signature == s.role_host_signature
            : a.host_signature == b.host_signature;
        if (!host_equal || (updates == 0 && a.host_signature != s.host_signature)) {
            ++metadata_errors; first("host_signature", -1);
            out << "{\"type\":\"diagnostic_metadata\",\"field\":\"host_signature\",\"equal\":false,\"reference\":";
            quoted(out, a.host_signature); out << ",\"actual\":"; quoted(out, b.host_signature); out << "}\n";
        }
        std::set<std::string> handled;
        auto exact = [&](const std::string& name, size_t offset, size_t bytes, int64_t row) {
            const uint64_t errors = (reference_all_owned && row >= 0 ? 0 :
                different(*sf.at(name), *af.at(name), offset, bytes)) +
                different(*sf.at(name), *bf.at(name), offset, bytes);
            invariant_errors += errors;
            if (errors) first(name, row);
            return errors;
        };
        auto numeric = [&](const std::string& name, int dims) {
            const auto& x = get(af, name, NpyScalar::f4, n * dims * 4);
            const auto& y = get(bf, name, NpyScalar::f4, n * dims * 4);
            Metric metric;
            const auto before = invariant_errors;
            for (size_t r = 0; r < n; ++r) {
                if (!s.owned_blocks[r / 256]) exact(name, r * dims * 4, dims * 4, int64_t(r));
                else for (int c = 0; c < dims; ++c) metric.add(load<float>(x, r*dims+c), load<float>(y, r*dims+c), int64_t(r), c);
            }
            metric.emit(out, name, "owned_original_rows_including_nonfinite", true, descriptive);
            out << "{\"type\":\"diagnostic_exact\",\"field\":"; quoted(out, name);
            out << ",\"errors\":" << invariant_errors-before << ",\"scope\":\"baseline_unowned\"}\n";
            if (!descriptive) decoded_errors += metric.errors();
            if (!descriptive && metric.errors()) first(name, metric.first_bad_row);
            handled.insert(name);
        };
        const char* names[] = {"means", "quats", "scales", "opacities", "features_dc"};
        const int dims[] = {3, 4, 3, 1, 3};
        for (int f = 0; f < 5; ++f) numeric(std::string("world.") + names[f], dims[f]);
        const auto addr = sh_quant_addr(uint32_t(s.num_sh), 0);
        const size_t shcells = size_t(sh_fpbo_cells(s.rows, uint32_t(s.num_sh)));
        for (int f = 0; f < 7; ++f) {
            const bool sh = f >= 5, value = f == 6;
            const int dim = sh ? 3*s.num_sh : dims[f];
            const size_t cell_bytes = sh ? 2 : 4, bound_bytes = value ? 8 : 16;
            const std::string name = sh ? (value ? "eng.world.sh_vq16_fpbo" : "eng.sh_quant_fpbo") : std::string("eng.")+names[f]+"_qfpbo";
            const std::string qname = name+".q", bname = name+".qb";
            const size_t cells = sh ? shcells : n*dim;
            const auto& x = get(af, qname, NpyScalar::u1, cells*cell_bytes);
            const auto& y = get(bf, qname, NpyScalar::u1, cells*cell_bytes);
            const auto& xb = get(af, bname, NpyScalar::f4, blocks*bound_bytes);
            const auto& yb = get(bf, bname, NpyScalar::f4, blocks*bound_bytes);
            Metric bounds, g1, g2, pad1, pad2;
            uint64_t qerrors = 0, bounderrors = 0;
            out << "{\"type\":\"diagnostic_raw_storage\",\"field\":"; quoted(out, qname);
            out << ",\"acceptance\":false,\"different_bytes\":" << different(x, y, 0, x.bytes.size()) << "}\n";
            for (size_t block = 0; block < blocks; ++block) {
                if (!s.owned_blocks[block]) {
                    const size_t offset = sh ? block*size_t(addr.bounds_stride)*cell_bytes : block*256*dim*cell_bytes;
                    const size_t bytes = sh ? size_t(addr.bounds_stride)*cell_bytes : std::min(size_t(256), n-block*256)*dim*cell_bytes;
                    qerrors += exact(qname, offset, bytes, int64_t(block*256));
                    bounderrors += exact(bname, block*bound_bytes, bound_bytes, int64_t(block*256));
                    continue;
                }
                for (size_t c = 0; c < bound_bytes/4; ++c)
                    bounds.add(load<float>(xb, block*(bound_bytes/4)+c), load<float>(yb, block*(bound_bytes/4)+c), int64_t(block*256), int(c));
                const float4 bx = value ? float4{} : load<float4>(xb, block);
                const float4 by = value ? float4{} : load<float4>(yb, block);
                const float2 vx = value ? load<float2>(xb, block) : float2{};
                const float2 vy = value ? load<float2>(yb, block) : float2{};
                const size_t end = sh ? (block+1)*256 : std::min((block+1)*256, n);
                for (size_t row = block*256; row < end; ++row) {
                    for (int c = 0; c < (sh ? ((dim+1)/2)*2 : dim); ++c) {
                        const size_t cell = sh ? size_t(addr.cell(addr.base(int64_t(row)), c)) : row*dim+c;
                        const bool padding = row >= n || c >= dim;
                        Metric& m1 = padding ? pad1 : g1;
                        Metric& m2 = padding ? pad2 : g2;
                        if (value) m1.add(shvalue16(x, cell, vx), shvalue16(y, cell, vy), int64_t(row), c);
                        else {
                            const float2 dx = sh ? QuantizedAdamState<8>::decode_g1g2(x.bytes.data(), int64_t(cell), bx) : adam16(x, cell, bx);
                            const float2 dy = sh ? QuantizedAdamState<8>::decode_g1g2(y.bytes.data(), int64_t(cell), by) : adam16(y, cell, by);
                            m1.add(dx.x, dy.x, int64_t(row), c); m2.add(dx.y, dy.y, int64_t(row), c);
                        }
                    }
                }
            }
            for (const auto& entry : {std::pair{qname, qerrors}, std::pair{bname, bounderrors}}) {
                out << "{\"type\":\"diagnostic_exact\",\"field\":"; quoted(out, entry.first);
                out << ",\"errors\":" << entry.second << ",\"scope\":\"baseline_unowned\"}\n";
            }
            bounds.emit(out, bname, "owned_raw_bounds_diagnostic", false, descriptive);
            const uint64_t invalid_bounds = bounds.cells - bounds.finite;
            nonfinite_bound_errors += invalid_bounds;
            out << "{\"type\":\"diagnostic_exact\",\"field\":"; quoted(out, bname);
            out << ",\"errors\":" << invalid_bounds << ",\"scope\":\"owned_bounds_finite_required\"}\n";
            if (invalid_bounds) first(bname, bounds.first_nonfinite_row);
            g1.emit(out, name+(value ? ".decoded_value" : ".decoded_g1"), "owned_original_rows", true, descriptive);
            if (!value) g2.emit(out, name+".decoded_g2", "owned_original_rows", true, descriptive);
            pad1.emit(out, name+".padding_first", "tail_and_odd_component_padding", false, descriptive);
            if (!value) pad2.emit(out, name+".padding_g2", "tail_and_odd_component_padding", false, descriptive);
            if (!descriptive) decoded_errors += g1.errors() + (value ? 0 : g2.errors());
            if (!descriptive && g1.errors()) first(qname, g1.first_bad_row);
            if (!descriptive && !value && g2.errors()) first(qname, g2.first_bad_row);
            handled.insert(qname); handled.insert(bname);
        }
        const std::string clock = "eng.bias_correction_steps";
        const auto& sc = get(sf, clock, NpyScalar::i4, n*4);
        const auto& ac = get(af, clock, NpyScalar::i4, n*4);
        const auto& bc = get(bf, clock, NpyScalar::i4, n*4);
        for (size_t row = 0; row < n; ++row) {
            const int64_t expected = int64_t(load<int32_t>(sc, row)) + (s.owned_blocks[row/256] ? updates : 0);
            const int64_t reference_expected = int64_t(load<int32_t>(sc, row)) +
                (reference_all_owned ? updates : (s.owned_blocks[row/256] ? updates : 0));
            const uint64_t errors = uint64_t(load<int32_t>(ac, row) != reference_expected) +
                uint64_t(load<int32_t>(bc, row) != expected);
            clock_errors += errors;
            if (errors) first(clock, int64_t(row));
        }
        out << "{\"type\":\"diagnostic_exact\",\"field\":\"eng.bias_correction_steps\",\"errors\":" << clock_errors << "}\n";
        handled.insert(clock);
        numeric("eng.accum_buffer", 2);
        if (sf.count("eng.densify.oversize")) numeric("eng.densify.oversize", 1);
        const auto& radii_a = get(af, "eng.radii", NpyScalar::f4, n*4);
        const auto& radii_b = get(bf, "eng.radii", NpyScalar::f4, n*4);
        Metric radii;
        for (size_t row = 0; row < n; ++row)
            radii.add(load<float>(radii_a, row), load<float>(radii_b, row), int64_t(row), 0);
        radii.emit(out, "eng.radii", "derived_projection_all_rows", false, descriptive);
        handled.insert("eng.radii");
        for (const auto& [name, field] : sf) {
            if (handled.count(name)) continue;
            if (name == "eng.color_shift_reg.batch_sum" || name == "eng.densify.sample_score") {
                const auto& x = *af.at(name);
                const auto& y = *bf.at(name);
                out << "{\"type\":\"diagnostic_raw_storage\",\"field\":"; quoted(out, name);
                out << ",\"acceptance\":false,\"different_bytes\":" << different(x, y, 0, x.bytes.size()) << "}\n";
                if (field->dtype == uint8_t(NpyScalar::f4)) {
                    require(field->bytes.size()%4 == 0, "derived float field size " + name);
                    Metric metric;
                    for (size_t cell = 0; cell < field->bytes.size()/4; ++cell)
                        metric.add(load<float>(x, cell), load<float>(y, cell), int64_t(cell), 0);
                    metric.emit(out, name, "derived_evolving_scratch", false, descriptive);
                }
                continue;
            }
            const auto before = invariant_errors;
            exact(name, 0, field->bytes.size(), -1);
            out << "{\"type\":\"diagnostic_exact\",\"field\":"; quoted(out, name);
            out << ",\"errors\":" << invariant_errors-before << ",\"scope\":\"baseline_frozen_shared\"}\n";
        }
        if (!first_field.empty()) retain_first(s, a, b, sf, af, bf, arm, updates, first_row, first_field, detail_root);
    } catch (const std::exception& error) {
        ++metadata_errors;
        out << "{\"type\":\"diagnostic_metadata\",\"compatible\":false,\"error\":";
        quoted(out, error.what()); out << "}\n";
    }
    const bool accepted = !(decoded_errors || clock_errors || invariant_errors || metadata_errors ||
                            serialized_logical_errors || nonfinite_bound_errors);
    out << "{\"type\":\"diagnostic_summary\",\"arm\":"; quoted(out, arm);
    out << ",\"descriptive\":" << (descriptive ? "true" : "false")
        << ",\"reference_all_owned\":" << (reference_all_owned ? "true" : "false");
    out << ",\"updates\":" << updates << ",\"rows\":" << s.rows << ",\"tail_valid\":" << (s.rows%256 ? s.rows%256 : 256)
        << ",\"decoded_errors\":" << decoded_errors << ",\"clock_errors\":" << clock_errors
        << ",\"invariant_byte_errors\":" << invariant_errors << ",\"metadata_errors\":" << metadata_errors
        << ",\"nonfinite_bound_errors\":" << nonfinite_bound_errors
        << ",\"immediate_logical_byte_errors\":" << logical_errors
        << ",\"immediate_serialized_logical_byte_errors\":" << serialized_logical_errors
        << ",\"immediate_roundtrip_exact\":";
    if (updates == 0) out << (serialized_logical_errors || metadata_errors ? "false" : "true");
    else out << "null";
    out << ",\"accepted\":" << (accepted ? "true" : "false") << "}\n";
    return accepted;
}
