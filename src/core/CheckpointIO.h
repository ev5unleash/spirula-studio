#pragma once

// CheckpointIO -- zero-dependency writers for the resumable-checkpoint format.
//
// A checkpoint's resume payload is a single POSIX ustar `state.tar` bundling:
//   * state.json         -- small runtime/validation manifest (written by the
//                           engine; NOT config -- config lives in config.json)
//   * <slot_name>.npy    -- one flat, typed NumPy array per saved pool buffer,
//                           named by its DevicePool slot ("world.means.npy",
//                           "eng.sh_quant.q.npy", ...).
//
// The device->host copy is chunked through a small reusable host buffer, so
// serializing a multi-GB buffer uses bounded host RAM and ZERO extra device
// memory (nothing is allocated on the GPU during a save).
//
// Phase 2 (load/resume) will add the matching tar + .npy readers here.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/Tensor.h"   // NpyScalar, npy_scalar_descr
#include "external/npy.hpp"


namespace ckpt {

// --- NPY v1.0 header for a flat array of `numel` elements of descr `descr`. ---
inline std::string npy_header(const char* descr, size_t numel) {
    std::string dict = "{'descr': '" + std::string(descr) +
        "', 'fortran_order': False, 'shape': (" + std::to_string(numel) + ",), }";
    size_t base = 10 + dict.size() + 1;               // +1 for trailing '\n'
    size_t pad  = (64 - (base % 64)) % 64;            // pad header to 64B multiple
    dict.append(pad, ' ');
    dict.push_back('\n');
    std::string h;
    h.reserve(10 + dict.size());
    h.append("\x93NUMPY", 6);
    h.push_back('\x01'); h.push_back('\x00');          // version 1.0
    uint16_t hlen = (uint16_t)dict.size();
    h.push_back((char)(hlen & 0xff));
    h.push_back((char)((hlen >> 8) & 0xff));
    h.append(dict);
    return h;
}

// --- POSIX ustar member header (512 bytes) for a file of `size` bytes. ---
inline void tar_header(std::ostream& out, const std::string& name, size_t size) {
    char h[512] = {0};
    std::snprintf(h + 0,   100, "%s", name.c_str());   // name
    std::snprintf(h + 100, 8,   "%07o", 0644);         // mode
    std::snprintf(h + 108, 8,   "%07o", 0);            // uid
    std::snprintf(h + 116, 8,   "%07o", 0);            // gid
    if (size <= 077777777777ULL) {                     // size: octal if it fits,
        std::snprintf(h + 124, 12, "%011llo", (unsigned long long)size);
    } else {                                            // else GNU base-256
        h[124] = (char)0x80;
        unsigned long long v = size;
        for (int i = 135; i >= 125; --i) { h[i] = (char)(v & 0xff); v >>= 8; }
    }
    std::snprintf(h + 136, 12,  "%011llo", 0ull);      // mtime
    std::memset (h + 148, ' ', 8);                     // checksum field = spaces
    h[156] = '0';                                      // typeflag: regular file
    std::memcpy (h + 257, "ustar", 5);                 // magic ("ustar\0")
    h[263] = '0'; h[264] = '0';                        // version "00"
    unsigned chk = 0;
    for (int i = 0; i < 512; i++) chk += (unsigned char)h[i];
    std::snprintf(h + 148, 7, "%06o", chk & 0777777u); // 6 octal digits + NUL@154
    h[155] = ' ';                                      // trailing space
    out.write(h, 512);
}

inline void tar_pad(std::ostream& out, size_t size) {
    size_t pad = (512 - (size % 512)) % 512;
    if (pad) { char z[512] = {0}; out.write(z, pad); }
}

// --- Append an in-memory member (e.g. state.json). ---
inline void tar_write_bytes(std::ostream& out, const std::string& name,
                            const char* data, size_t size) {
    tar_header(out, name, size);
    out.write(data, size);
    tar_pad(out, size);
}

// --- Two zero blocks terminate the archive. ---
inline void tar_finish(std::ostream& out) {
    char z[512] = {0};
    out.write(z, 512);
    out.write(z, 512);
}

// --- Stream a device buffer as a flat typed .npy member. Chunked D->H copy
//     through `host` (reused across calls); no device allocation. ---
inline void tar_write_npy_device(std::ostream& out, const std::string& name,
                                 const void* dptr, size_t nbytes, uint8_t dtype_tag,
                                 std::vector<char>& host) {
    auto descr_size = npy_scalar_descr((NpyScalar)dtype_tag);
    const char* descr = descr_size.first;
    size_t esize      = descr_size.second;
    if (esize == 0 || nbytes % esize != 0)
        throw std::runtime_error("checkpoint: slot byte count is not scalar-aligned");
    size_t numel      = nbytes / esize;
    std::string hdr = npy_header(descr, numel);
    size_t member   = hdr.size() + nbytes;
    tar_header(out, name, member);
    out.write(hdr.data(), (std::streamsize)hdr.size());

    constexpr size_t CHUNK = 32u << 20;   // 32 MiB host staging
    if (host.size() < std::min(nbytes, CHUNK)) host.resize(std::min(nbytes, CHUNK));
    const char* src = (const char*)dptr;
    for (size_t off = 0; off < nbytes; ) {
        size_t n = std::min(CHUNK, nbytes - off);
        backend::memcpy_sync(host.data(), src + off, n, backend::MemcpyKind::DeviceToHost);
        out.write(host.data(), (std::streamsize)n);
        off += n;
    }
    tar_pad(out, member);
}


// ============================================================================
// Readers (checkpoint load / resume).
// ============================================================================

// A member located in a tar stream: its name and the [offset, size) of its
// raw content within the archive.
struct TarMember {
    std::string name;
    uint64_t    data_offset;
    uint64_t    size;
};

// Index every regular-file member of a ustar stream (no extraction). Handles
// octal and GNU base-256 size fields. Leaves the stream position undefined.
namespace checkpoint_io_detail {

inline uint64_t stream_length(std::istream& in) {
    in.clear();
    in.seekg(0, std::ios::end);
    if (!in) throw std::runtime_error("checkpoint archive: cannot seek to end");
    const std::streampos end = in.tellg();
    if (end < std::streampos(0))
        throw std::runtime_error("checkpoint archive: invalid length");
    return (uint64_t)end;
}

inline void seek_abs(std::istream& in, uint64_t offset) {
    if (offset > (uint64_t)std::numeric_limits<std::streamoff>::max())
        throw std::runtime_error("checkpoint archive: offset is too large");
    in.clear();
    in.seekg((std::streamoff)offset, std::ios::beg);
    if (!in) throw std::runtime_error("checkpoint archive: seek failed");
}

inline bool zero_block(const char* h) {
    for (size_t i = 0; i < 512; ++i)
        if (h[i] != '\0') return false;
    return true;
}

inline uint64_t parse_octal(const char* field, size_t count,
                            const char* what) {
    uint64_t value = 0;
    bool any = false;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char c = (unsigned char)field[i];
        if (c == '\0') break;
        if (c == ' ' && !any) continue;
        if (c == ' ' && any) break;
        if (c < '0' || c > '7')
            throw std::runtime_error(std::string("checkpoint archive: invalid ") + what);
        any = true;
        if (value > (UINT64_MAX - (uint64_t)(c - '0')) / 8)
            throw std::runtime_error(std::string("checkpoint archive: overflowing ") + what);
        value = value * 8 + (uint64_t)(c - '0');
    }
    if (!any)
        throw std::runtime_error(std::string("checkpoint archive: empty ") + what);
    return value;
}

inline uint64_t parse_size(const char* field) {
    if ((unsigned char)field[0] & 0x80) {
        // GNU positive base-256: the marker occupies the high bit of the
        // first byte and the remaining eleven bytes hold the value.
        if ((unsigned char)field[0] != 0x80)
            throw std::runtime_error("checkpoint archive: negative/invalid base-256 size");
        uint64_t value = 0;
        for (int i = 1; i < 12; ++i) {
            if (value > (UINT64_MAX >> 8))
                throw std::runtime_error("checkpoint archive: overflowing size");
            value = (value << 8) | (uint64_t)(unsigned char)field[i];
        }
        return value;
    }
    return parse_octal(field, 12, "size");
}

inline unsigned parse_checksum(const char* field) {
    return (unsigned)parse_octal(field, 8, "checksum");
}

inline unsigned header_checksum(const char* h) {
    unsigned value = 0;
    for (size_t i = 0; i < 512; ++i)
        value += (unsigned char)((i >= 148 && i < 156) ? ' ' : h[i]);
    return value;
}

inline std::string header_name(const char* h) {
    size_t n = 0;
    while (n < 100 && h[n] != '\0') ++n;
    if (n == 0) throw std::runtime_error("checkpoint archive: empty member name");
    return std::string(h, n);
}

}  // namespace checkpoint_io_detail

inline std::vector<TarMember> tar_index(std::istream& in) {
    using namespace checkpoint_io_detail;
    const uint64_t length = stream_length(in);
    std::vector<TarMember> members;
    std::set<std::string> names;
    char h[512];
    uint64_t offset = 0;
    for (;;) {
        if (offset > length || length - offset < sizeof(h))
            throw std::runtime_error("checkpoint archive: truncated member header");
        seek_abs(in, offset);
        in.read(h, sizeof(h));
        if (!in) throw std::runtime_error("checkpoint archive: short member header");
        if (zero_block(h)) {
            if (length - offset < 1024)
                throw std::runtime_error("checkpoint archive: truncated tar terminator");
            char z[512];
            seek_abs(in, offset + 512);
            in.read(z, sizeof(z));
            if (!in || !zero_block(z))
                throw std::runtime_error("checkpoint archive: missing second tar terminator");
            if (length != offset + 1024)
                throw std::runtime_error("checkpoint archive: trailing data after tar terminator");
            return members;
        }
        if (header_checksum(h) != parse_checksum(h + 148))
            throw std::runtime_error("checkpoint archive: header checksum mismatch");
        const char type = h[156];
        if (type != '\0' && type != '0')
            throw std::runtime_error("checkpoint archive: unsupported member type");
        const std::string name = header_name(h);
        if (!names.insert(name).second)
            throw std::runtime_error("checkpoint archive: duplicate member '" + name + "'");
        const uint64_t size = parse_size(h + 124);
        const uint64_t data_offset = offset + 512;
        if (data_offset < offset || data_offset > length ||
            size > length - data_offset)
            throw std::runtime_error("checkpoint archive: member extends past end");
        if (size > UINT64_MAX - 511)
            throw std::runtime_error("checkpoint archive: padded member size overflows");
        const uint64_t padded = (size + 511) & ~uint64_t(511);
        if (padded > length - data_offset)
            throw std::runtime_error("checkpoint archive: padded member extends past end");
        members.push_back({name, data_offset, size});
        offset = data_offset + padded;
    }
}

// Read a whole small member (e.g. state.json) into a string.
inline std::string tar_read_member(std::istream& in, const TarMember& m) {
    using namespace checkpoint_io_detail;
    const uint64_t length = stream_length(in);
    if (m.data_offset > length || m.size > length - m.data_offset)
        throw std::runtime_error("checkpoint archive: member extends past end");
    if (m.size > (uint64_t)std::numeric_limits<size_t>::max() ||
        m.size > (uint64_t)std::numeric_limits<std::streamsize>::max())
        throw std::runtime_error("checkpoint archive: member is too large to read");
    std::string s((size_t)m.size, '\0');
    seek_abs(in, m.data_offset);
    if (m.size != 0) {
        in.read(s.data(), (std::streamsize)m.size);
        if (!in) throw std::runtime_error("checkpoint archive: short member read");
    }
    return s;
}

// Location of the raw array data inside a .npy blob that starts at `npy_start`
// and spans `member_size` bytes. Also returns the numpy descr string.
struct NpyInfo {
    uint64_t    data_offset;   // absolute offset in the stream
    uint64_t    data_bytes;    // member_size - header
    uint64_t    numel;         // flat element count
    std::string descr;         // e.g. "<f4"
};

inline NpyInfo npy_locate(std::istream& in, uint64_t npy_start, uint64_t member_size) {
    using namespace checkpoint_io_detail;
    if (npy_start > UINT64_MAX - member_size)
        throw std::runtime_error("checkpoint NPY: member range overflows");
    seek_abs(in, npy_start);
    char pre[10];
    in.read(pre, sizeof(pre));
    if (!in) throw std::runtime_error("checkpoint NPY: truncated prefix");
    if (std::memcmp(pre, "\x93NUMPY", 6) != 0)
        throw std::runtime_error("checkpoint NPY: invalid magic");
    const uint8_t major = (uint8_t)pre[6];
    const uint8_t minor = (uint8_t)pre[7];
    if (major < 1 || major > 3 || minor != 0)
        throw std::runtime_error("checkpoint NPY: unsupported version");
    uint32_t hlen = 0;
    uint32_t prefix = 10;
    if (major >= 2) {
        char more[2];
        in.read(more, sizeof(more));
        if (!in) throw std::runtime_error("checkpoint NPY: truncated length");
        hlen = (uint32_t)(uint8_t)pre[8] |
               ((uint32_t)(uint8_t)pre[9] << 8) |
               ((uint32_t)(uint8_t)more[0] << 16) |
               ((uint32_t)(uint8_t)more[1] << 24);
        prefix = 12;
    } else {
        hlen = (uint32_t)(uint8_t)pre[8] |
               ((uint32_t)(uint8_t)pre[9] << 8);
    }
    if ((uint64_t)prefix > member_size ||
        (uint64_t)hlen > member_size - prefix)
        throw std::runtime_error("checkpoint NPY: header extends past member");
    if (hlen > (uint64_t)std::numeric_limits<size_t>::max())
        throw std::runtime_error("checkpoint NPY: header is too large");
    std::string dict((size_t)hlen, '\0');
    if (hlen != 0) {
        in.read(dict.data(), (std::streamsize)hlen);
        if (!in) throw std::runtime_error("checkpoint NPY: truncated header");
    }
    std::string trimmed = npy::pyparse::trim(dict);
    if (trimmed.empty() || trimmed.back() != '\n')
        throw std::runtime_error("checkpoint NPY: invalid empty/header dictionary");
    trimmed.pop_back();
    if (npy::pyparse::trim(trimmed).empty())
        throw std::runtime_error("checkpoint NPY: empty dictionary");
    const auto fields = npy::pyparse::parse_dict(
        trimmed, {"descr", "fortran_order", "shape"});
    const auto descr_raw = npy::pyparse::trim(fields.at("descr"));
    const auto fortran_raw = npy::pyparse::trim(fields.at("fortran_order"));
    const auto shape_raw = npy::pyparse::trim(fields.at("shape"));
    if (descr_raw.empty() || fortran_raw.empty() || shape_raw.empty())
        throw std::runtime_error("checkpoint NPY: empty header field");
    const std::string descr_s = npy::pyparse::parse_str(descr_raw);
    const npy::dtype_t dtype = npy::parse_descr(descr_s);
    if (dtype.itemsize == 0)
        throw std::runtime_error("checkpoint NPY: zero item size");
    if (npy::pyparse::parse_bool(fortran_raw))
        throw std::runtime_error("checkpoint NPY: Fortran layout is unsupported");
    const std::string shape_s = npy::pyparse::trim(shape_raw);
    if (shape_s.empty() || shape_s.front() != '(' || shape_s.back() != ')')
        throw std::runtime_error("checkpoint NPY: expected a flat one-dimensional shape tuple");
    const std::string tuple_body =
        shape_s.substr(1, shape_s.size() - 2);
    const size_t comma = tuple_body.find(',');
    if (comma == std::string::npos ||
        tuple_body.find(',', comma + 1) != std::string::npos ||
        !npy::pyparse::trim(tuple_body.substr(comma + 1)).empty())
        throw std::runtime_error("checkpoint NPY: expected a singleton shape tuple");
    const auto shape = npy::pyparse::parse_tuple(
        "(" + npy::pyparse::trim(tuple_body) + ")");
    if (shape.size() != 1)
        throw std::runtime_error("checkpoint NPY: expected a flat one-dimensional shape");
    const std::string dim_s = npy::pyparse::trim(shape.front());
    if (dim_s.empty())
        throw std::runtime_error("checkpoint NPY: empty shape dimension");
    uint64_t numel = 0;
    const char* first = dim_s.data();
    const char* last = first + dim_s.size();
    const auto parsed = std::from_chars(first, last, numel);
    if (parsed.ec != std::errc() || parsed.ptr != last)
        throw std::runtime_error("checkpoint NPY: invalid shape dimension");
    const uint64_t data_bytes = member_size - prefix - hlen;
    if (numel > UINT64_MAX / (uint64_t)dtype.itemsize ||
        numel * (uint64_t)dtype.itemsize != data_bytes)
        throw std::runtime_error("checkpoint NPY: shape does not match payload size");
    const uint64_t data_offset = npy_start + prefix + hlen;
    if (data_offset < npy_start)
        throw std::runtime_error("checkpoint NPY: data offset overflows");
    return {data_offset, data_bytes, numel, dtype.str()};
}

// Stream `nbytes` from `in` (positioned via seek to `offset`) into a device
// buffer, chunked through `host` -- bounded host RAM, no device allocation.
inline void read_into_device(std::istream& in, uint64_t offset,
                             void* dptr, size_t nbytes, std::vector<char>& host) {
    using namespace checkpoint_io_detail;
    const uint64_t length = stream_length(in);
    if (offset > length || (uint64_t)nbytes > length - offset)
        throw std::runtime_error("checkpoint archive: short device payload");
    seek_abs(in, offset);
    constexpr size_t CHUNK = 32u << 20;
    if (host.size() < std::min(nbytes, CHUNK))
        host.resize(std::min(nbytes, CHUNK));
    char* dst = (char*)dptr;
    for (size_t off = 0; off < nbytes; ) {
        size_t n = std::min(CHUNK, nbytes - off);
        in.read(host.data(), (std::streamsize)n);
        if (!in) throw std::runtime_error("checkpoint archive: short device payload");
        backend::memcpy_sync(dst + off, host.data(), n,
                             backend::MemcpyKind::HostToDevice);
        off += n;
    }
}

} // namespace ckpt
