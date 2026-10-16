#include "checkpoint/B1Contract.h"
#include "checkpoint/Resume.h"
#include "config/TrainConfigJson.h"
#include "core/CheckpointIO.h"
#include "core/Sha256.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ckpt {
namespace {
[[noreturn]] void bad(const std::string& s) { throw std::runtime_error("B1 contract: " + s); }
void unique(const JsonValue& j) {
    if (!j.is_object()) bad("expected object");
    std::set<std::string> keys;
    for (const auto& p : j.obj) if (!keys.insert(p.first).second) bad("duplicate field " + p.first);
}
int64_t number(const JsonValue& j, const char* key) {
    const auto* v = j.find(key);
    if (!v || v->type != JsonValue::Type::Number || !std::isfinite(v->num) ||
        v->num < 0 || v->num > INT32_MAX || std::trunc(v->num) != v->num) bad(key);
    return (int64_t)v->num;
}
std::string text(const JsonValue& j, const char* key, bool digest = true) {
    const auto* v = j.find(key);
    if (!v || v->type != JsonValue::Type::String || v->str.empty()) bad(key);
    if (digest && (v->str.size() != 64 || v->str.find_first_not_of("0123456789abcdef") != std::string::npos)) bad(key);
    return v->str;
}
uint64_t le(std::istream& in, int bytes) {
    uint64_t n = 0;
    for (int i = 0; i < bytes; ++i) { int c = in.get(); if (c == EOF) bad("truncated sidecar"); n |= (uint64_t)(uint8_t)c << (i * 8); }
    return n;
}
std::vector<uint32_t> mask(const std::filesystem::path& root, const B1Contract& c) {
    std::ifstream in(root / "b1-owned-blocks.bin", std::ios::binary);
    if (!in || le(in,8) != (uint64_t)c.rows) bad("sidecar rows");
    if (!le(in,8) || le(in,8) != (uint64_t)c.completed_step) bad("sidecar source/step");
    uint64_t n = le(in,4);
    if (n != (uint64_t)(c.rows + 255) / 256 || le(in,4) != (uint64_t)c.owner) bad("sidecar owner/blocks");
    std::vector<uint32_t> out((size_t)n);
    for (auto& x : out) { x = (uint32_t)le(in,4); if (x > 1 || (c.owner == 2 && x != 1)) bad("mask value"); }
    if (in.get() != EOF) bad("extra mask bytes");
    return out;
}
std::string mask_digest(const std::vector<uint32_t>& m) {
    spirula::Sha256 h;
    for (uint32_t x : m) { uint8_t raw[4] = {(uint8_t)x,0,0,0}; h.update(raw,4); }
    return h.hex();
}
}
std::string b1_file_hash(const std::filesystem::path& p) {
    if (!std::filesystem::is_regular_file(p)) bad("missing file " + p.string());
    std::ifstream in(p, std::ios::binary); spirula::Sha256 h; std::vector<uint8_t> buf(1 << 20);
    while (in) { in.read((char*)buf.data(), (std::streamsize)buf.size()); if (in.gcount()) h.update(buf.data(),(size_t)in.gcount()); }
    if (!in.eof()) bad("read failed " + p.string());
    return h.hex();
}
std::string b1_config_hash(const TrainConfig& c) {
    static const std::set<std::string> logistics = {"resume","output_dir_prefix","output_dir_name","steps_per_save","save_only_latest_checkpoint","save_full_checkpoint","save_eval_images","viewer_port","disable_viewer","keep_viewer_alive"};
    spirula::Sha256 h;
    for (const auto& p : train_config_json_pairs(c)) if (!logistics.count(p.first)) {
        std::string s = std::string(p.first) + ":" + p.second + "\n";
        h.update((const uint8_t*)s.data(),s.size());
    }
    return h.hex();
}
std::string b1_indices_hash(const std::vector<int32_t>& ids) {
    spirula::Sha256 h;
    for (int32_t i : ids) { uint8_t b[4]; for (int k=0;k<4;++k) b[k]=(uint8_t)((uint32_t)i>>(k*8)); h.update(b,4); }
    return h.hex();
}
std::string b1_shared_hash(const std::filesystem::path& root) {
    std::ifstream in(root / "state.tar", std::ios::binary);
    if (!in) bad("missing state.tar");
    auto members = tar_index(in); spirula::Sha256 h;
    for (const auto& m : members) if (m.name.rfind("eng.bg.",0)==0 || m.name.rfind("eng.ppisp.",0)==0 || m.name.rfind("eng.bg_sky.",0)==0) {
        h.update((const uint8_t*)m.name.data(),m.name.size());
        in.clear(); in.seekg((std::streamoff)m.data_offset);
        std::vector<uint8_t> buf(1<<20); uint64_t left=m.size;
        while(left) { size_t n=(size_t)std::min<uint64_t>(left,buf.size()); in.read((char*)buf.data(),n); if((size_t)in.gcount()!=n) bad("truncated shared state"); h.update(buf.data(),n); left-=n; }
    }
    return h.hex();
}
std::string serialize_b1_contract(const B1Contract& c) {
    std::ostringstream out;
    out << "{\"policy\":" << json_field::emit(std::string(c.version == 3
        ? "native_fpbo_live_context_v1" : "native_fpbo_fixed_context_v1")) << ",";
#define N(x) out << "\"" #x "\":" << c.x << ",";
    N(version) N(owner) N(sampler_version) N(source_step) N(completed_step) N(horizon) N(rows)
#undef N
#define S(x) out << "\"" #x "\":" << json_field::emit(c.x) << ",";
    S(source_state_sha256) S(config_sha256) S(views_sha256) S(train_indices_sha256) S(mask_sha256) S(frozen_context) S(frozen_shared_sha256) S(blocks_sha256) S(owned_ids_sha256) S(ply_sha256)
    if (c.version != 1) { S(view_policy) S(full_train_indices_sha256) }
    if (c.version == 3) {
        S(refresh_group_sha256) S(refresh_payload0_sha256) S(refresh_payload1_sha256) S(refresh_commit_sha256)
        out << "\"refresh_cadence\":" << c.refresh_cadence << ","
            << "\"refresh_generation\":" << c.refresh_generation << ","
            << "\"refresh_parent\":" << c.refresh_parent << ",";
    }
#undef S
    out << "\"excluded_ids_sha256\":" << json_field::emit(c.excluded_ids_sha256) << "}"; return out.str();
}
JsonValue b1_contract_json(const B1Contract& c) { return json_parse(serialize_b1_contract(c)); }
B1Contract parse_b1_contract(const JsonValue& j) {
    unique(j); B1Contract c;
    c.version = (int)number(j,"version");
    if ((c.version != 1 && c.version != 2 && c.version != 3) ||
        j.obj.size() != (c.version == 1 ? 19u : c.version == 2 ? 21u : 28u) ||
        text(j,"policy",false) != (c.version == 3
            ? "native_fpbo_live_context_v1" : "native_fpbo_fixed_context_v1"))
        bad("unknown policy or missing fields");
#define N(x) c.x = (decltype(c.x))number(j,#x);
    N(version) N(owner) N(sampler_version) N(source_step) N(completed_step) N(horizon) N(rows)
#undef N
#define S(x) c.x = text(j,#x);
    S(source_state_sha256) S(config_sha256) S(views_sha256) S(train_indices_sha256) S(mask_sha256) S(frozen_shared_sha256) S(blocks_sha256) S(owned_ids_sha256) S(excluded_ids_sha256) S(ply_sha256)
#undef S
    c.frozen_context=text(j,"frozen_context");
    if(c.sampler_version!=1 || c.owner>2 || c.rows<=0 || c.completed_step<c.source_step || c.completed_step>c.horizon) bad("version/owner/schedule");
    if (c.version == 1) {
        c.view_policy = c.owner == 2 ? "full" : "partitioned";
        if (c.owner == 2) c.full_train_indices_sha256 = c.train_indices_sha256;
    } else {
        c.view_policy = text(j,"view_policy",false);
        c.full_train_indices_sha256 = text(j,"full_train_indices_sha256");
        if ((c.view_policy != "partitioned" && c.view_policy != "full") ||
            (c.owner == 2 && c.view_policy != "full")) bad("view policy");
        if (c.view_policy == "full" && c.train_indices_sha256 != c.full_train_indices_sha256)
            bad("full training coverage");
    }
    if (c.version == 3) {
        c.refresh_cadence = number(j,"refresh_cadence");
        c.refresh_generation = number(j,"refresh_generation");
        c.refresh_parent = number(j,"refresh_parent");
        c.refresh_group_sha256 = text(j,"refresh_group_sha256");
        c.refresh_payload0_sha256 = text(j,"refresh_payload0_sha256");
        c.refresh_payload1_sha256 = text(j,"refresh_payload1_sha256");
        c.refresh_commit_sha256 = text(j,"refresh_commit_sha256");
        const std::string zero(64, '0');
        if (c.owner > 1 || c.view_policy != "full" || !c.refresh_cadence ||
            c.refresh_generation < c.source_step || c.refresh_generation > c.completed_step ||
            (c.refresh_generation - c.source_step) % c.refresh_cadence ||
            c.completed_step - c.refresh_generation > c.refresh_cadence ||
            c.refresh_group_sha256 == zero)
            bad("refresh policy/generation");
        if (c.refresh_generation == c.source_step) {
            if (c.refresh_parent != c.source_step || c.refresh_payload0_sha256 != zero ||
                c.refresh_payload1_sha256 != zero || c.refresh_commit_sha256 != c.refresh_group_sha256)
                bad("initial refresh binding");
        } else if (c.refresh_parent != c.refresh_generation - c.refresh_cadence ||
                   c.refresh_payload0_sha256 == zero || c.refresh_payload1_sha256 == zero ||
                   c.refresh_commit_sha256 == zero || c.refresh_commit_sha256 == c.refresh_group_sha256) {
            bad("partial refresh binding");
        }
    }
    return c;
}
void bind_b1_sidecars(B1Contract& c, const std::filesystem::path& root) {
    c.blocks_sha256=b1_file_hash(root/"b1-owned-blocks.bin"); c.owned_ids_sha256=b1_file_hash(root/"b1-owned-ids.bin"); c.excluded_ids_sha256=b1_file_hash(root/"b1-excluded-ids.bin");
    c.mask_sha256=mask_digest(mask(root,c));
}
std::optional<B1Contract> read_b1_contract(const std::filesystem::path& root) {
    auto j=read_state_json(root); unique(j); const auto* v=j.find("b1");
    if(!v) { for(const char* n:{"b1-owned-blocks.bin","b1-owned-ids.bin","b1-excluded-ids.bin"}) if(std::filesystem::exists(root/n)) bad("unmarked experimental owner checkpoint"); return std::nullopt; }
    auto c=parse_b1_contract(*v);
    if(number(j,"format_version")!=2 || number(j,"full_resume")!=1 || number(j,"step")!=c.completed_step || number(j,"cur_num_splats")!=c.rows) bad("native state mismatch");
    auto bound=c; bind_b1_sidecars(bound,root);
    if(bound.blocks_sha256!=c.blocks_sha256 || bound.owned_ids_sha256!=c.owned_ids_sha256 || bound.excluded_ids_sha256!=c.excluded_ids_sha256 || bound.mask_sha256!=c.mask_sha256) bad("sidecar digest mismatch");
    auto m=mask(root,c); std::vector<uint8_t> seen((size_t)c.rows);
    for(const char* name:{"b1-owned-ids.bin","b1-excluded-ids.bin"}) {
        std::ifstream in(root/name,std::ios::binary); uint64_t n=le(in,8); if(n>(uint64_t)c.rows) bad("ID count"); uint32_t prev=0;
        for(uint64_t k=0;k<n;++k) { uint64_t id=le(in,4); if(id>=(uint64_t)c.rows || !m[(size_t)id/256] || (k && id<=prev) || seen[(size_t)id]++) bad("ID ownership/order"); prev=(uint32_t)id; }
        if(in.get()!=EOF) bad("extra ID bytes");
    }
    for(size_t i=0;i<seen.size();++i) if((seen[i]!=0)!=(m[i/256]!=0)) bad("incomplete owned rows");
    if(b1_shared_hash(root)!=c.frozen_shared_sha256) bad("shared appearance changed");
    const auto config=root/"config.json";
    if(!std::filesystem::is_regular_file(config) || b1_config_hash(config_from_json(config))!=c.config_sha256) bad("missing or mismatched saved config");
    if(b1_file_hash(root/"splat.ply")!=c.ply_sha256) bad("stale or changed owner PLY");
    return c;
}
std::string b1_ply_body_hash(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) bad("missing PLY");
    std::string line; bool end = false;
    while (std::getline(in, line)) {
        if (line.size() > 4096) bad("oversized PLY header");
        if (line == "end_header") { end = true; break; }
    }
    if (!end) bad("missing PLY end_header");
    spirula::Sha256 h; std::vector<uint8_t> buffer(1 << 20);
    while (in) {
        in.read((char*)buffer.data(), buffer.size());
        if (in.gcount()) h.update(buffer.data(), (size_t)in.gcount());
    }
    if (!in.eof()) bad("PLY body read failed");
    return h.hex();
}
void validate_b1_pair(const B1Contract& a, const B1Contract& b) {
    const auto left = parse_b1_contract(b1_contract_json(a));
    const auto right = parse_b1_contract(b1_contract_json(b));
    if(left.owner>1 || right.owner>1 || left.owner==right.owner || left.version!=right.version ||
        left.sampler_version!=right.sampler_version || left.view_policy!=right.view_policy ||
        (left.version>=2 && left.full_train_indices_sha256!=right.full_train_indices_sha256) ||
        left.source_step!=right.source_step || left.completed_step!=right.completed_step || left.horizon!=right.horizon ||
        left.rows!=right.rows || left.source_state_sha256!=right.source_state_sha256 ||
        left.config_sha256!=right.config_sha256 || left.views_sha256!=right.views_sha256 ||
        left.frozen_shared_sha256!=right.frozen_shared_sha256) bad("owners are not a matched complementary pair");
    if (left.version == 3 &&
        (left.refresh_cadence != right.refresh_cadence ||
         left.refresh_generation != right.refresh_generation ||
         left.refresh_parent != right.refresh_parent ||
         left.refresh_group_sha256 != right.refresh_group_sha256 ||
         left.refresh_payload0_sha256 != right.refresh_payload0_sha256 ||
         left.refresh_payload1_sha256 != right.refresh_payload1_sha256 ||
         left.refresh_commit_sha256 != right.refresh_commit_sha256))
        bad("owners have mixed refresh contexts");
}
std::string b1_appearance_comment(const B1Contract& c,const std::string& hash) {
    return "comment ss_b1_appearance {\"version\":1,\"source_state_sha256\":"+json_field::emit(c.source_state_sha256)+",\"frozen_shared_sha256\":"+json_field::emit(c.frozen_shared_sha256)+",\"body_sha256\":"+json_field::emit(hash)+"}\n";
}
void validate_b1_appearance(const std::filesystem::path& ply,const std::filesystem::path& source) {
    const auto root = ply.parent_path();
    if (ply.filename() == "splat.ply" && std::filesystem::exists(root / "state.tar")) {
        const auto contract = read_b1_contract(root);
        if (!contract || contract->owner != 2)
            bad("evaluation requires an allowned native checkpoint or bound merged PLY");
        if (contract->source_state_sha256 != b1_file_hash(source / "state.tar") ||
            contract->frozen_shared_sha256 != b1_shared_hash(source))
            bad("wrong source appearance");
        return;
    }
    std::ifstream in(ply,std::ios::binary); if(!in) bad("missing merged PLY"); std::string line; std::optional<JsonValue> binding; bool end=false;
    const std::string prefix = "comment ss_b1_appearance ";
    while(std::getline(in,line)) { if(line.size()>4096) bad("oversized PLY header"); if(line.rfind(prefix,0)==0) { if(binding) bad("duplicate appearance binding"); binding=json_parse(line.substr(prefix.size())); } if(line=="end_header") {end=true;break;} }
    if(!end || !binding) bad("missing merged appearance binding"); unique(*binding);
    if(binding->obj.size()!=4 || number(*binding,"version")!=1 || text(*binding,"source_state_sha256")!=b1_file_hash(source/"state.tar") || text(*binding,"frozen_shared_sha256")!=b1_shared_hash(source)) bad("wrong source appearance");
    spirula::Sha256 h; std::vector<uint8_t> buf(1<<20); while(in) {in.read((char*)buf.data(),buf.size()); if(in.gcount())h.update(buf.data(),(size_t)in.gcount());} if(!in.eof() || h.hex()!=text(*binding,"body_sha256")) bad("merged payload digest mismatch");
}
}
