// Resume.cpp -- see Resume.h.

#include "checkpoint/Resume.h"

#include "config/TrainConfigJson.h"
#include "core/CheckpointIO.h"
#include "i18n/catalog/Log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <climits>

namespace fs = std::filesystem;
namespace lmsg = spirula::i18n::msg::log;

namespace ckpt {

namespace {

// Read state.tar once and hand back both its member index and the stream.
struct TarView {
    std::ifstream in;
    std::vector<TarMember> members;
};

TarView open_state_tar(const fs::path& ckpt_dir) {
    const fs::path tarpath = ckpt_dir / "state.tar";
    TarView v;
    v.in.open(tarpath.string(), std::ios::binary);
    if (!v.in)
        throw std::runtime_error("cannot open " + tarpath.string() +
                                 " (not a checkpoint directory)");
    v.members = tar_index(v.in);
    return v;
}

std::string read_member(TarView& v, const std::string& name) {
    for (const auto& m : v.members)
        if (m.name == name) return tar_read_member(v.in, m);
    return {};
}

bool is_staging(const fs::path& path) {
    const std::string name = path.filename().string();
    return name.rfind(".checkpoint-", 0) == 0;
}

fs::path paired_config_path(const fs::path& run_dir, const fs::path& ckpt_dir) {
    const fs::path local = ckpt_dir / "config.json";
    std::error_code ec;
    fs::file_status st = fs::symlink_status(local, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        ec.clear();
        st = fs::file_status(fs::file_type::not_found);
    }
    if (ec)
        throw std::runtime_error("cannot inspect " + local.string() +
                                 ": " + ec.message());
    if (st.type() != fs::file_type::not_found)
        return local;
    return run_dir / "config.json";
}

[[noreturn]] void no_checkpoint(const fs::path& run_dir) {
    throw std::runtime_error(spirula::i18n::format(
        lmsg::checkpoint_resume_unavailable, {run_dir.string()}));
}

int state_step(const JsonValue& state, const fs::path& ckpt_dir) {
    const JsonValue* v = state.find("step");
    if (!v || v->type != JsonValue::Type::Number ||
        !std::isfinite(v->num) || std::floor(v->num) != v->num ||
        v->num < 0.0 || v->num > (double)INT_MAX)
        throw std::runtime_error("checkpoint " + ckpt_dir.string() +
                                 " has an invalid state step");
    return (int)v->num;
}


struct Candidate {
    int step;
    fs::path path;
};

}  // namespace


int checkpoint_step(const fs::path& path) {
    const std::string name = path.filename().string();
    constexpr const char* prefix = "step-";
    constexpr const char* suffix = ".ckpt";
    if (name.size() <= std::strlen(prefix) + std::strlen(suffix) ||
        name.compare(0, std::strlen(prefix), prefix) != 0 ||
        name.compare(name.size() - std::strlen(suffix),
                     std::strlen(suffix), suffix) != 0)
        return -1;
    const size_t begin = std::strlen(prefix);
    const size_t end = name.size() - std::strlen(suffix);
    int value = 0;
    const auto parsed = std::from_chars(name.data() + begin,
                                        name.data() + end, value);
    if (parsed.ec != std::errc() || parsed.ptr != name.data() + end ||
        value < 0)
        return -1;
    char canonical[64];
    std::snprintf(canonical, sizeof canonical, "step-%09d.ckpt", value);
    return name == canonical ? value : -1;
}


ResolvedCheckpoint resolve_checkpoint(const fs::path& input) {
    fs::path path = fs::absolute(input).lexically_normal();
    if (path != path.root_path() && path.filename().empty())
        path = path.parent_path();
    if (is_staging(path))
        throw std::runtime_error("staging checkpoint paths cannot be resumed: " +
                                 path.string());
    std::error_code ec;

    fs::file_status tar_status = fs::symlink_status(path / "state.tar", ec);
    if (ec == std::errc::no_such_file_or_directory) {
        ec.clear();
        tar_status = fs::file_status(fs::file_type::not_found);
    }
    if (ec)
        throw std::runtime_error("cannot inspect " +
                                 (path / "state.tar").string() + ": " +
                                 ec.message());
    if (tar_status.type() == fs::file_type::regular)
        return {path.parent_path(), path,
                paired_config_path(path.parent_path(), path)};

    // A canonical checkpoint name pins direct-path callers even when its
    // archive is incomplete; validate_checkpoint reports the actual problem.
    if (checkpoint_step(path) >= 0)
        return {path.parent_path(), path,
                paired_config_path(path.parent_path(), path)};

    const fs::file_status run_status = fs::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        throw std::runtime_error("cannot inspect " + path.string() + ": " +
                                 ec.message());
    if (ec || run_status.type() != fs::file_type::directory)
        no_checkpoint(path);

    std::vector<Candidate> candidates;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path candidate = it->path();
        const int step = checkpoint_step(candidate);
        if (step < 0) continue;
        const fs::file_status st = fs::symlink_status(candidate, ec);
        if (ec) break;
        if (st.type() == fs::file_type::directory)
            candidates.push_back({step, candidate});
    }
    if (ec) throw std::runtime_error("cannot enumerate " + path.string() +
                                     ": " + ec.message());
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.step > b.step;
              });

    for (const Candidate& candidate : candidates) {
        try {
            validate_checkpoint(candidate.path, true);
            const fs::path config =
                paired_config_path(path, candidate.path);
            const TrainConfig saved = config_from_json(config);
            if (saved.data.empty())
                throw std::runtime_error("checkpoint config has no data");
            return {path, candidate.path, config};
        } catch (const std::exception&) {
            // A damaged/newer candidate must not hide an older usable one.
        }
    }
    no_checkpoint(path);
}


JsonValue read_state_json(const fs::path& ckpt_dir) {
    TarView v = open_state_tar(ckpt_dir);
    const std::string sj = read_member(v, "state.json");
    if (sj.empty())
        throw std::runtime_error("state.json missing in " +
                                 (ckpt_dir / "state.tar").string());
    return json_parse(sj);
}


JsonValue validate_checkpoint(const fs::path& ckpt_dir, bool require_full) {
    TarView v = open_state_tar(ckpt_dir);
    const std::string sj = read_member(v, "state.json");
    if (sj.empty())
        throw std::runtime_error("state.json missing in " +
                                 (ckpt_dir / "state.tar").string());
    const JsonValue state = json_parse(sj);
    if (!state.is_object())
        throw std::runtime_error("state.json is not an object in " +
                                 ckpt_dir.string());
    const int step = state_step(state, ckpt_dir);
    const int named_step = checkpoint_step(ckpt_dir);
    if (named_step >= 0 && named_step != step)
        throw std::runtime_error("checkpoint filename/state step mismatch in " +
                                 ckpt_dir.string());

    bool has_means = false;
    bool has_opacities = false;
    for (const auto& m : v.members) {
        if (m.name == "world.means.npy") has_means = true;
        if (m.name == "world.opacities.npy") has_opacities = true;
        if (m.name.size() >= 4 &&
            m.name.compare(m.name.size() - 4, 4, ".npy") == 0)
            (void)npy_locate(v.in, m.data_offset, m.size);
    }

    const JsonValue* full = state.find("full_resume");
    if (full) {
        if (full->type != JsonValue::Type::Number ||
            !std::isfinite(full->num) || (full->num != 0.0 && full->num != 1.0))
            throw std::runtime_error("checkpoint full_resume must be numeric 0 or 1");
    }
    if (require_full &&
        ((full && full->num == 0.0) || !has_means || !has_opacities))
        throw std::runtime_error(spirula::i18n::format(
            lmsg::checkpoint_not_resumable, {ckpt_dir.string()}));
    return state;
}


TrainConfig config_from_json(const fs::path& config_json) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(config_json, ec);
    if (ec || status.type() != fs::file_type::regular)
        throw std::runtime_error(config_json.string() +
                                 " is not a regular file");
    const JsonValue root = json_parse_file(config_json.string());
    if (!root.is_object() || !train_config_json_has_fields(root))
        throw std::runtime_error(config_json.string() +
                                 " holds no training options");
    TrainConfig c;
    train_config_from_json(root, c);
    return c;
}


TrainConfig build_resume_config(const TrainConfig& cli,
                                 const std::string& preset,
                                 const std::set<std::string>& explicit_flags) {
    const ResolvedCheckpoint r = resolve_checkpoint(cli.resume);
    const JsonValue state = validate_checkpoint(r.ckpt_dir, true);
    (void)state;
    TrainConfig base = config_from_json(r.config_path);
    if (base.data.empty())
        throw std::runtime_error(r.config_path.string() +
                                 " has no dataset path");
    const fs::path run_dir = fs::absolute(r.run_dir).lexically_normal();
    if (const fs::path data = fs::path(base.data); data.is_relative()) {
        std::error_code ec;
        const fs::file_status status = fs::status(data, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            throw std::runtime_error("cannot inspect " + data.string() +
                                     ": " + ec.message());
        if (!ec && fs::exists(status)) {
            const fs::path absolute_data = fs::absolute(data, ec);
            if (ec)
                throw std::runtime_error("cannot resolve " + data.string() +
                                         ": " + ec.message());
            base.data = absolute_data.lexically_normal().string();
        } else {
            base.data = (run_dir / data).lexically_normal().string();
        }
    }

    // Continue writing into the checkpoint's own run folder, so new
    // checkpoints, eval images and logs land beside the old ones. An explicit
    // --output-dir-* below overrides this.
    base.output_dir_prefix = run_dir.parent_path().string();
    base.output_dir_name   = run_dir.filename().string();

    // A preset named on the resume command line re-imposes its deviations on
    // top of the checkpoint.
    if (!preset.empty() && !train_apply_preset(base, preset))
        throw std::runtime_error("unknown preset: " + preset);

    // Explicit flags win over both.
#define SS_APPLY_EXPLICIT(type, member, default_, section, tier, choices)     \
    if (explicit_flags.count(#member)) base.member = cli.member;
    SS_CONFIG_FIELDS(SS_APPLY_EXPLICIT)
#undef SS_APPLY_EXPLICIT

    // Always pin the selected checkpoint after applying flags; a run
    // directory may have resolved to a different step than the input spelling.
    base.resume = fs::absolute(r.ckpt_dir).string();
    return base;
}

}  // namespace ckpt
