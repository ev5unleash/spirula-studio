#pragma once

// CheckpointIO -- zero-dependency writers and readers for `state.tar`.
// The archive contains `state.json` plus one flat, typed NumPy array per
// saved device-pool slot. Device transfers use a reusable 32 MiB host buffer,
// keeping checkpoint serialization bounded in host and device memory.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/Tensor.h"   // NpyScalar, npy_scalar_descr


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
    if (!out) throw std::runtime_error("checkpoint archive write failed");
}

// --- Two zero blocks terminate the archive. ---
inline void tar_finish(std::ostream& out) {
    char z[512] = {0};
    out.write(z, 512);
    out.write(z, 512);
    out.flush();
    if (!out) throw std::runtime_error("checkpoint archive write failed");
}

// --- Stream a device buffer as a flat typed .npy member. Chunked D->H copy
//     through `host` (reused across calls); no device allocation. ---
inline void tar_write_npy_device(std::ostream& out, const std::string& name,
                                 const void* dptr, size_t nbytes, uint8_t dtype_tag,
                                 std::vector<char>& host) {
    auto descr_size = npy_scalar_descr((NpyScalar)dtype_tag);
    const char* descr = descr_size.first;
    size_t esize      = descr_size.second;
    size_t numel      = nbytes / esize;

    std::string hdr = npy_header(descr, numel);
    size_t member   = hdr.size() + nbytes;
    tar_header(out, name, member);
    out.write(hdr.data(), (std::streamsize)hdr.size());
    if (!out) throw std::runtime_error("checkpoint archive write failed");

    constexpr size_t CHUNK = 32u << 20;
    if (host.size() < std::min(nbytes, CHUNK)) host.resize(std::min(nbytes, CHUNK));
    const char* src = (const char*)dptr;
    for (size_t off = 0; off < nbytes; ) {
        size_t n = std::min(CHUNK, nbytes - off);
        backend::memcpy_sync(host.data(), src + off, n, backend::MemcpyKind::DeviceToHost);
        out.write(host.data(), (std::streamsize)n);
        if (!out) throw std::runtime_error("checkpoint archive write failed");
        off += n;
    }
    tar_pad(out, member);
    if (!out) throw std::runtime_error("checkpoint archive write failed");
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
inline std::vector<TarMember> tar_index(std::istream& in) {
    auto all_zero = [](const char* p) {
        return std::all_of(p, p + 512, [](char c) { return c == '\0'; });
    };
    auto octal = [](const char* p, size_t n) {
        uint64_t value = 0;
        bool have_digit = false;
        bool ended = false;
        for (size_t i = 0; i < n; ++i) {
            const unsigned char c = (unsigned char)p[i];
            if (c == '\0' || c == ' ') {
                if (have_digit) ended = true;
                continue;
            }
            if (ended || c < '0' || c > '7' ||
                value > (std::numeric_limits<uint64_t>::max() >> 3))
                throw std::runtime_error("invalid checkpoint archive header");
            value = (value << 3) + (c - '0');
            have_digit = true;
        }
        if (!have_digit) throw std::runtime_error("invalid checkpoint archive header");
        return value;
    };

    in.clear();
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0) throw std::runtime_error("cannot size checkpoint archive");
    const uint64_t archive_size = (uint64_t)end;
    in.seekg(0, std::ios::beg);
    if (!in) throw std::runtime_error("cannot read checkpoint archive");

    std::vector<TarMember> members;
    for (;;) {
        const std::streamoff pos = in.tellg();
        if (pos < 0 || (uint64_t)pos > archive_size ||
            archive_size - (uint64_t)pos < 512)
            throw std::runtime_error("truncated checkpoint archive header");

        char h[512];
        in.read(h, 512);
        if (in.gcount() != 512)
            throw std::runtime_error("truncated checkpoint archive header");
        if (all_zero(h)) {
            char end_block[512];
            in.read(end_block, 512);
            if (in.gcount() != 512 || !all_zero(end_block))
                throw std::runtime_error("truncated checkpoint archive terminator");
            break;
        }
        if (h[0] == '\0' || std::memcmp(h + 257, "ustar", 5) != 0)
            throw std::runtime_error("invalid checkpoint archive header");

        const uint64_t expected_checksum = octal(h + 148, 8);
        uint64_t actual_checksum = 0;
        for (int i = 0; i < 512; ++i)
            actual_checksum += (i >= 148 && i < 156)
                ? (unsigned char)' ' : (unsigned char)h[i];
        if (actual_checksum != expected_checksum)
            throw std::runtime_error("checkpoint archive header checksum mismatch");

        uint64_t size = 0;
        if ((unsigned char)h[124] & 0x80) {
            for (int i = 125; i < 136; ++i) {
                if (size > (std::numeric_limits<uint64_t>::max() >> 8))
                    throw std::runtime_error("checkpoint archive member is too large");
                size = (size << 8) | (unsigned char)h[i];
            }
        } else {
            size = octal(h + 124, 12);
        }

        const uint64_t data_off = (uint64_t)pos + 512;
        if (size > std::numeric_limits<uint64_t>::max() - 511)
            throw std::runtime_error("checkpoint archive member is too large");
        const uint64_t padded = (size + 511) & ~uint64_t{511};
        if (data_off > archive_size || padded > archive_size - data_off)
            throw std::runtime_error("truncated checkpoint archive member");

        if (h[156] == '\0' || h[156] == '0')
            members.push_back({std::string(h, ::strnlen(h, 100)), data_off, size});
        in.seekg((std::streamoff)(data_off + padded), std::ios::beg);
        if (!in) throw std::runtime_error("cannot read checkpoint archive");
    }
    return members;
}

// Read a whole small member (e.g. state.json) into a string.
inline std::string tar_read_member(std::istream& in, const TarMember& m) {
    if (m.size > (uint64_t)std::numeric_limits<std::streamsize>::max())
        throw std::runtime_error("checkpoint archive member is too large");
    std::string s((size_t)m.size, '\0');
    in.clear();
    in.seekg((std::streamoff)m.data_offset, std::ios::beg);
    if (!in) throw std::runtime_error("cannot read checkpoint archive member");
    if (!s.empty()) in.read(s.data(), (std::streamsize)s.size());
    if (!in) throw std::runtime_error("truncated checkpoint archive member");
    return s;
}

// Location of the raw array data inside a .npy blob that starts at `npy_start`
// and spans `member_size` bytes. Also returns the numpy descr string.
struct NpyInfo {
    uint64_t    data_offset;   // absolute offset in the stream
    uint64_t    data_bytes;    // member_size - header
    std::string descr;         // e.g. "<f4"
};
inline NpyInfo npy_locate(std::istream& in, uint64_t npy_start, uint64_t member_size) {
    if (member_size < 10)
        throw std::runtime_error("truncated checkpoint NumPy header");
    in.clear();
    in.seekg((std::streamoff)npy_start, std::ios::beg);
    if (!in) throw std::runtime_error("cannot read checkpoint NumPy header");

    char pre[10];
    in.read(pre, 10);
    if (!in || std::memcmp(pre, "\x93NUMPY", 6) != 0)
        throw std::runtime_error("invalid checkpoint NumPy header");
    const uint8_t major = (uint8_t)pre[6];
    uint32_t hlen;
    uint32_t prefix;
    if (major == 2 || major == 3) {
        if (member_size < 12)
            throw std::runtime_error("truncated checkpoint NumPy header");
        char more[2];
        in.read(more, 2);
        if (!in) throw std::runtime_error("truncated checkpoint NumPy header");
        hlen = (uint8_t)pre[8] | ((uint8_t)pre[9] << 8)
             | ((uint8_t)more[0] << 16) | ((uint8_t)more[1] << 24);
        prefix = 12;
    } else if (major == 1) {
        hlen = (uint8_t)pre[8] | ((uint8_t)pre[9] << 8);
        prefix = 10;
    } else {
        throw std::runtime_error("unsupported checkpoint NumPy version");
    }
    if ((uint64_t)prefix + hlen > member_size)
        throw std::runtime_error("truncated checkpoint NumPy header");

    std::string dict(hlen, '\0');
    if (!dict.empty()) in.read(dict.data(), hlen);
    if (!in) throw std::runtime_error("truncated checkpoint NumPy header");
    std::string descr;
    const size_t d = dict.find("'descr':");
    if (d != std::string::npos) {
        const size_t q1 = dict.find('\'', d + 8);
        const size_t q2 = q1 == std::string::npos
            ? q1 : dict.find('\'', q1 + 1);
        if (q2 != std::string::npos) descr = dict.substr(q1 + 1, q2 - q1 - 1);
    }
    size_t element_size = 0;
    for (int tag = 0; tag < (int)NpyScalar::b1 + 1; ++tag) {
        const auto [candidate, size] = npy_scalar_descr((NpyScalar)tag);
        if (descr == candidate) {
            element_size = size;
            break;
        }
    }
    if (element_size == 0)
        throw std::runtime_error("unsupported checkpoint NumPy dtype");

    const std::string shape_key = "'shape': (";
    size_t p = dict.find(shape_key);
    if (p == std::string::npos)
        throw std::runtime_error("invalid checkpoint NumPy shape");
    p += shape_key.size();
    uint64_t numel = 0;
    bool have_digit = false;
    while (p < dict.size() && dict[p] >= '0' && dict[p] <= '9') {
        const uint64_t digit = (uint64_t)(dict[p++] - '0');
        if (numel > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            throw std::runtime_error("checkpoint NumPy shape is too large");
        numel = numel * 10 + digit;
        have_digit = true;
    }
    if (!have_digit || p >= dict.size() || dict[p] != ',')
        throw std::runtime_error("invalid checkpoint NumPy shape");

    const uint64_t data_bytes = member_size - (prefix + hlen);
    if (numel > std::numeric_limits<uint64_t>::max() / element_size ||
        numel * element_size != data_bytes)
        throw std::runtime_error("checkpoint NumPy shape does not match its data");
    return {npy_start + prefix + hlen, data_bytes, descr};
}

// Stream `nbytes` from `in` (positioned via seek to `offset`) into a device
// buffer, chunked through `host` -- bounded host RAM, no device allocation.
inline void read_into_device(std::istream& in, uint64_t offset,
                             void* dptr, size_t nbytes, std::vector<char>& host) {
    in.clear();
    in.seekg((std::streamoff)offset, std::ios::beg);
    if (!in) throw std::runtime_error("cannot read checkpoint array");
    constexpr size_t CHUNK = 32u << 20;
    if (host.size() < std::min(nbytes, CHUNK)) host.resize(std::min(nbytes, CHUNK));
    char* dst = (char*)dptr;
    for (size_t off = 0; off < nbytes; ) {
        size_t n = std::min(CHUNK, nbytes - off);
        in.read(host.data(), (std::streamsize)n);
        if (!in) throw std::runtime_error("truncated checkpoint array");
        backend::memcpy_sync(dst + off, host.data(), n, backend::MemcpyKind::HostToDevice);
        off += n;
    }
}

} // namespace ckpt
