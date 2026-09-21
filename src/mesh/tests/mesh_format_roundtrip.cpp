// mesh_format_roundtrip -- every format the mesher writes reads back, and a
// run asking for several colors writes each of them exactly once.
//
//   ./build_vulkan/mesh_format_roundtrip
//
// The writers and the reader are two hand-written implementations of the same
// five container formats; nothing but a round trip keeps them honest.

#include "core/SourcePath.h"
#include "mesh/MeshExport.h"
#include "mesh/MeshImport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fail = 0;

void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::printf("FAIL %s:%d  %s\n", SS_FILE, line, what);
    g_fail++;
}
#define CHECK(cond) check((cond), #cond, __LINE__)

const fs::path& scratch() {
    static const fs::path dir =
        fs::temp_directory_path() / "ss_mesh_format_test";
    return dir;
}

void reset_scratch() {
    std::error_code ec;
    fs::remove_all(scratch(), ec);
    fs::create_directories(scratch(), ec);
    CHECK(!ec);
}

void cleanup_scratch() {
    std::error_code ec;
    fs::remove_all(scratch(), ec);
}

int find_vertex(const meshing::MeshData& src,
                const std::array<float, 3>& v, float tolerance) {
    int found = -1;
    const float limit = tolerance * tolerance;
    for (size_t i = 0; i < src.V.size(); ++i) {
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = v[k] - src.V[i][k];
            d2 += d * d;
        }
        if (d2 <= limit) {
            if (found >= 0) return -2;
            found = (int)i;
        }
    }
    return found;
}

std::vector<int> map_vertices(const meshing::MeshData& src,
                              const meshing::MeshData& back,
                              float tolerance) {
    std::vector<int> ids;
    ids.reserve(back.V.size());
    std::vector<bool> present(src.V.size(), false);
    for (const auto& v : back.V) {
        const int id = find_vertex(src, v, tolerance);
        CHECK(id >= 0);
        ids.push_back(id);
        if (id >= 0) present[(size_t)id] = true;
    }
    for (bool seen : present) CHECK(seen);
    return ids;
}

std::array<int, 3> canonical_face(const std::array<int, 3>& f) {
    std::array<int, 3> best = f;
    for (int r = 1; r < 3; ++r) {
        const std::array<int, 3> rotated = {f[r], f[(r + 1) % 3],
                                             f[(r + 2) % 3]};
        if (rotated < best) best = rotated;
    }
    return best;
}

void check_surface(const meshing::MeshData& src,
                   const meshing::MeshData& back,
                   const std::vector<int>& ids) {
    CHECK(back.F.size() == src.F.size());
    std::vector<std::array<int, 3>> expected, actual;
    expected.reserve(src.F.size());
    actual.reserve(back.F.size());
    for (const auto& f : src.F) expected.push_back(canonical_face(f));

    bool valid = true;
    for (const auto& f : back.F) {
        std::array<int, 3> mapped = {};
        for (int k = 0; k < 3; ++k) {
            if (f[k] < 0 || (size_t)f[k] >= ids.size() ||
                ids[(size_t)f[k]] < 0) {
                valid = false;
                continue;
            }
            mapped[k] = ids[(size_t)f[k]];
        }
        if (valid) actual.push_back(canonical_face(mapped));
    }
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    CHECK(valid && actual == expected);
}

std::array<float, 3> face_normal(const meshing::MeshData& mesh,
                                 const std::array<int, 3>& f) {
    const auto& a = mesh.V[(size_t)f[0]];
    const auto& b = mesh.V[(size_t)f[1]];
    const auto& c = mesh.V[(size_t)f[2]];
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    std::array<float, 3> n = {
        e1[1] * e2[2] - e1[2] * e2[1],
        e1[2] * e2[0] - e1[0] * e2[2],
        e1[0] * e2[1] - e1[1] * e2[0]};
    const float len =
        std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-20f)
        for (float& v : n) v /= len;
    return n;
}

bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

void check_normals(const meshing::MeshData& src,
                   const meshing::MeshData& back,
                   const std::vector<int>& ids, const std::string& fmt) {
    CHECK(back.N.size() == back.V.size());
    if (back.N.size() != back.V.size()) return;
    if (fmt == "stl") {
        for (const auto& f : back.F) {
            std::array<int, 3> mapped = {};
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                if (f[k] < 0 || (size_t)f[k] >= ids.size() ||
                    ids[(size_t)f[k]] < 0) {
                    valid = false;
                    continue;
                }
                mapped[k] = ids[(size_t)f[k]];
            }
            if (!valid) continue;
            const auto expected = face_normal(src, mapped);
            for (int k = 0; k < 3; ++k)
                for (int d = 0; d < 3; ++d)
                    CHECK(near(back.N[(size_t)f[k]][d], expected[d], 2e-5f));
        }
        return;
    }

    const float tolerance = fmt == "obj" ? 3e-4f : 2e-5f;
    for (size_t i = 0; i < back.V.size(); ++i) {
        if (ids[i] < 0) continue;
        for (int d = 0; d < 3; ++d)
            CHECK(near(back.N[i][d], src.N[(size_t)ids[i]][d], tolerance));
    }
}

void check_fields(const meshing::MeshData& src,
                  const meshing::MeshData& back,
                  const std::vector<int>& ids,
                  const meshing::MeshFormatSpec& spec,
                  meshing::MeshColorMode mode) {
    if (mode == meshing::MeshColorMode::Vertex) {
        CHECK(back.C.size() == back.V.size());
        if (back.C.size() == back.V.size())
            for (size_t i = 0; i < back.C.size(); ++i) {
                if (ids[i] < 0) continue;
                for (int d = 0; d < 3; ++d)
                    CHECK(back.C[i][d] == src.C[(size_t)ids[i]][d]);
            }
    } else {
        CHECK(back.C.empty());
    }

    if (mode != meshing::MeshColorMode::Texture) {
        CHECK(back.UV.empty());
        CHECK(back.texture.empty());
        CHECK(back.tex_width == 0 && back.tex_height == 0);
        return;
    }

    CHECK(back.UV.size() == back.V.size());
    if (back.UV.size() == back.V.size())
        for (size_t i = 0; i < back.UV.size(); ++i) {
            if (ids[i] < 0) continue;
            for (int d = 0; d < 2; ++d)
                CHECK(near(back.UV[i][d], src.UV[(size_t)ids[i]][d],
                           spec.fmt == "obj" ? 2e-5f : 2e-6f));
        }

    CHECK(back.tex_width == src.tex_width &&
          back.tex_height == src.tex_height);
    CHECK(back.texture.size() == src.texture.size());
    if (back.texture.size() != src.texture.size()) return;
    if (!spec.jpeg) {
        CHECK(back.texture == src.texture);
        return;
    }

    const int tolerance = spec.quality >= 90 ? 64 : 96;
    for (size_t i = 0; i < back.texture.size(); ++i)
        CHECK(std::abs((int)back.texture[i] - (int)src.texture[i]) <=
              tolerance);
}

void check_outputs(const fs::path& base,
                   const meshing::MeshFormatSpec& spec,
                   meshing::MeshColorMode mode) {
    for (const std::string& path :
         meshing::mesh_output_paths(spec, mode, base.string()))
        CHECK(fs::is_regular_file(path));
    if (mode != meshing::MeshColorMode::Texture) return;

    const fs::path png = base.string() + ".png";
    const fs::path jpg = base.string() + ".jpg";
    if (spec.fmt == "glb") {
        CHECK(!fs::exists(png));
        CHECK(!fs::exists(jpg));
    } else {
        CHECK(fs::is_regular_file(spec.jpeg ? jpg : png));
        CHECK(!fs::exists(spec.jpeg ? png : jpg));
    }
}

// Distinct geometry and attribute values make index swaps observable.
meshing::MeshData tetra(meshing::MeshColorMode mode) {
    meshing::MeshData m;
    m.V = {{0.13f, 0.27f, 0.41f},
           {1.37f, 0.39f, 0.22f},
           {0.31f, 1.43f, 0.58f},
           {0.52f, 0.34f, 1.67f}};
    m.F = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    meshing::compute_vertex_normals(m);
    if (mode == meshing::MeshColorMode::Vertex)
        m.C = {{17, 83, 149}, {211, 43, 97}, {64, 201, 37}, {239, 157, 11}};
    if (mode == meshing::MeshColorMode::Texture) {
        m.UV = {{0.13f, 0.23f}, {0.87f, 0.31f},
                {0.22f, 0.79f}, {0.74f, 0.91f}};
        m.tex_width = 4;
        m.tex_height = 3;
        m.texture = {
            12, 34, 56, 228, 186, 142, 46, 208, 98, 202, 52, 226,
            16, 120, 236, 240, 164, 24, 12, 34, 56, 228, 186, 142,
            46, 208, 98, 202, 52, 226, 16, 120, 236, 240, 164, 24};
    }
    return m;
}

void test_roundtrip(const meshing::MeshFormatSpec& spec,
                    meshing::MeshColorMode mode) {
    reset_scratch();
    const meshing::MeshData src = tetra(mode);
    const fs::path base =
        scratch() / ("tetra_" + spec.token() + "_" +
                     meshing::mesh_color_token(mode));
    meshing::write_mesh(src, mode, spec, base.string(), /*verbose=*/false);
    check_outputs(base, spec, mode);

    meshing::MeshData back;
    std::string error;
    const bool ok =
        meshing::read_mesh(base.string() + "." + spec.fmt, back, error);
    if (!ok) std::printf("  read_mesh(%s): %s\n", spec.token().c_str(),
                         error.c_str());
    CHECK(ok);
    if (!ok) return;

    const std::vector<int> ids = map_vertices(src, back, 3e-5f);
    check_surface(src, back, ids);
    check_normals(src, back, ids, spec.fmt);
    check_fields(src, back, ids, spec, mode);

    if (spec.fmt == "stl") {
        CHECK(back.V.size() == src.F.size() * 3);
        CHECK(mode == meshing::MeshColorMode::None);
    }
    if (spec.fmt == "obj" && mode == meshing::MeshColorMode::Vertex)
        CHECK(back.C.empty());
}


// The plan is what decides where each file lands, so it is what has to be
// right before a three-minute run writes anything.
void test_plan() {
    const std::vector<meshing::MeshFormatSpec> ply_glb = {
        meshing::parse_one_mesh_format("ply"),
        meshing::parse_one_mesh_format("glb")};

    // One colour: the names a run has always had, no suffix.
    auto one = meshing::plan_mesh_outputs(
        ply_glb, {meshing::MeshColorMode::Vertex}, "out/mesh");
    CHECK(one.size() == 2);
    for (const auto& r : one) CHECK(r.base == "out/mesh");

    // Two: each writes at its own base, and the pair PLY cannot carry is
    // dropped rather than failing the run.
    std::vector<std::string> dropped;
    auto two = meshing::plan_mesh_outputs(
        ply_glb, {meshing::MeshColorMode::Vertex, meshing::MeshColorMode::Texture},
        "out/mesh", &dropped);
    CHECK(two.size() == 3);
    CHECK(dropped.size() == 1);
    for (const auto& r : two) {
        CHECK(r.base != "out/mesh");
        CHECK(!(r.spec.fmt == "ply" && r.mode == meshing::MeshColorMode::Texture));
    }

    // Nothing at all is the mistake, and it comes back empty.
    auto none = meshing::plan_mesh_outputs(
        {meshing::parse_one_mesh_format("stl")},
        {meshing::MeshColorMode::Texture}, "out/mesh");
    CHECK(none.empty());

    // A colour list parses like a format list, and rejects what it should.
    const auto modes = meshing::parse_mesh_colors("texture, none");
    CHECK(modes.size() == 2);
    CHECK(modes[0] == meshing::MeshColorMode::Texture);
    bool threw = false;
    try { meshing::parse_mesh_colors("vertex,vertex"); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // An --output that already carries an extension means the same thing.
    CHECK(meshing::mesh_output_strip_ext("out/mesh.glb") == "out/mesh");
    CHECK(meshing::mesh_output_strip_ext("out/mesh") == "out/mesh");
}

}  // namespace


int main() {
    for (const char* fmt : {"ply", "gltf", "glb"}) {
        meshing::MeshFormatSpec spec;
        spec.fmt = fmt;
        test_roundtrip(spec, meshing::MeshColorMode::Vertex);
    }
    for (const char* fmt : {"ply", "obj", "gltf", "glb", "stl"}) {
        meshing::MeshFormatSpec spec;
        spec.fmt = fmt;
        test_roundtrip(spec, meshing::MeshColorMode::None);
    }
    for (const char* fmt : {"obj", "gltf", "glb"}) {
        meshing::MeshFormatSpec spec;
        spec.fmt = fmt;
        test_roundtrip(spec, meshing::MeshColorMode::Texture);
    }
    for (const char* fmt : {"obj+jpeg95", "gltf+jpeg95", "glb+jpeg95"})
        test_roundtrip(meshing::parse_one_mesh_format(fmt),
                       meshing::MeshColorMode::Texture);
    test_plan();
    cleanup_scratch();

    if (g_fail) {
        std::printf("mesh_format_roundtrip: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("mesh_format_roundtrip: OK\n");
    return 0;
}
