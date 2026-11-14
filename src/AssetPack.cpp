// ---------------------------------------------------------------------------
// AssetPack.cpp – Asset-Paket lesen, ChaCha20 (Schnittstelle und Aufbau der
// Datei: AssetPack.hpp).
// ---------------------------------------------------------------------------
#include "AssetPack.hpp"
#include "Crypto.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace pak {

namespace {

uint32_t Load32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

uint64_t Load64(const uint8_t* p) { return Load32(p) | static_cast<uint64_t>(Load32(p + 4)) << 32; }

void Store32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

void Append(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t Rotl(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

// Viertelrunde von ChaCha20 (RFC 8439, Abschnitt 2.1)
void QuarterRound(uint32_t* x, int a, int b, int c, int d) {
    x[a] += x[b]; x[d] ^= x[a]; x[d] = Rotl(x[d], 16);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = Rotl(x[b], 12);
    x[a] += x[b]; x[d] ^= x[a]; x[d] = Rotl(x[d], 8);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = Rotl(x[b], 7);
}

Key Hmac(const Key& key, const char* label, const uint8_t* salt) {
    std::vector<uint8_t> msg(label, label + strlen(label));
    msg.insert(msg.end(), salt, salt + 16);
    const crypto::Digest d = crypto::HmacSha256(key.data(), key.size(), msg.data(), msg.size());
    Key k{};
    std::copy(d.begin(), d.end(), k.begin());
    return k;
}

} // namespace

Key KeyFromFileContents(const std::string& contents) {
    const crypto::Digest d = crypto::Sha256(contents);
    Key k{};
    std::copy(d.begin(), d.end(), k.begin());
    return k;
}

Keys DeriveKeys(const Key& key, const uint8_t salt[16]) {
    Keys k;
    k.enc = Hmac(key, "Mondschuerfer-Pak enc", salt);
    k.mac = Hmac(key, "Mondschuerfer-Pak mac", salt);
    return k;
}

// Zustand: 4 Konstanten, 8 Wörter Schlüssel, Blockzähler, 96-Bit-Nonce (hier
// ein Wort 0 und die 64-Bit-Nonce). Jeder 64-Byte-Block des Schlüsselstroms
// entsteht aus 20 Runden über den Zustand; danach zählt der Blockzähler hoch
// (32 Bit: reicht für 256 GB je Datei).
void ChaCha20(const Key& key, uint64_t nonce, uint8_t* data, size_t len) {
    uint32_t state[16] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    for (int i = 0; i < 8; ++i) state[4 + i] = Load32(key.data() + 4 * i);
    state[12] = 0;
    state[13] = 0;
    state[14] = static_cast<uint32_t>(nonce);
    state[15] = static_cast<uint32_t>(nonce >> 32);
    uint8_t block[64];
    for (size_t pos = 0; pos < len; pos += 64) {
        uint32_t x[16];
        memcpy(x, state, sizeof(x));
        for (int r = 0; r < 10; ++r) {
            QuarterRound(x, 0, 4, 8, 12);
            QuarterRound(x, 1, 5, 9, 13);
            QuarterRound(x, 2, 6, 10, 14);
            QuarterRound(x, 3, 7, 11, 15);
            QuarterRound(x, 0, 5, 10, 15);
            QuarterRound(x, 1, 6, 11, 12);
            QuarterRound(x, 2, 7, 8, 13);
            QuarterRound(x, 3, 4, 9, 14);
        }
        for (int i = 0; i < 16; ++i) Store32(block + 4 * i, x[i] + state[i]);
        const size_t n = std::min<size_t>(64, len - pos);
        for (size_t i = 0; i < n; ++i) data[pos + i] ^= block[i];
        ++state[12];
    }
}

std::vector<uint8_t> WriteIndex(const std::vector<Entry>& entries) {
    std::vector<uint8_t> out;
    for (const Entry& e : entries) {
        Append(out, e.path.size(), 2);
        out.insert(out.end(), e.path.begin(), e.path.end());
        Append(out, e.offset, 8);
        Append(out, e.size, 8);
        out.insert(out.end(), e.mac.begin(), e.mac.end());
    }
    return out;
}

bool ReadIndex(const std::vector<uint8_t>& data, uint32_t count, std::vector<Entry>& out) {
    out.clear();
    size_t pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 2 > data.size()) return false;
        const size_t len = data[pos] | static_cast<size_t>(data[pos + 1]) << 8;
        pos += 2;
        if (pos + len + 8 + 8 + 32 > data.size()) return false;
        Entry e;
        e.path.assign(reinterpret_cast<const char*>(data.data() + pos), len);
        pos += len;
        e.offset = Load64(data.data() + pos);
        e.size = Load64(data.data() + pos + 8);
        pos += 16;
        std::copy(data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + 32),
                  e.mac.begin());
        pos += 32;
        e.nonce = static_cast<uint64_t>(i) + 1;
        out.push_back(std::move(e));
    }
    return pos == data.size();
}

bool Reader::Open(const std::string& file, const Key& key, std::string& err) {
    open_ = false;
    entries_.clear();
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        err = "Datei nicht gefunden";
        return false;
    }
    f.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    uint8_t head[kHeaderSize];
    if (fileSize < kHeaderSize || !f.read(reinterpret_cast<char*>(head), kHeaderSize) || memcmp(head, "MSPK", 4) != 0) {
        err = "kein Asset-Paket";
        return false;
    }
    if (Load32(head + 4) != kVersion) {
        err = "unbekannte Version";
        return false;
    }
    const uint32_t count = Load32(head + 24);
    const uint32_t indexSize = Load32(head + 28);
    if (kHeaderSize + static_cast<uint64_t>(indexSize) > fileSize) {
        err = "Inhaltsverzeichnis abgeschnitten";
        return false;
    }
    std::vector<uint8_t> index(indexSize);
    if (indexSize > 0 && !f.read(reinterpret_cast<char*>(index.data()), indexSize)) {
        err = "Inhaltsverzeichnis nicht lesbar";
        return false;
    }

    keys_ = DeriveKeys(key, head + 8);
    std::vector<uint8_t> signedPart(head, head + 32);
    signedPart.insert(signedPart.end(), index.begin(), index.end());
    const crypto::Digest mac = crypto::HmacSha256(keys_.mac.data(), keys_.mac.size(), signedPart.data(), signedPart.size());
    if (!crypto::ConstantTimeEqual(mac.data(), head + 32, mac.size())) {
        err = "falscher Schluessel oder Datei veraendert";
        return false;
    }
    ChaCha20(keys_.enc, 0, index.data(), index.size());
    if (!ReadIndex(index, count, entries_)) {
        err = "Inhaltsverzeichnis beschaedigt";
        return false;
    }
    for (const Entry& e : entries_)
        if (e.offset > fileSize || e.size > fileSize - e.offset) {
            err = "Datei abgeschnitten: " + e.path;
            entries_.clear();
            return false;
        }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    file_ = file;
    open_ = true;
    return true;
}

const Entry* Reader::Find(const std::string& path) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), path,
                               [](const Entry& e, const std::string& p) { return e.path < p; });
    return it != entries_.end() && it->path == path ? &*it : nullptr;
}

bool Reader::Read(const Entry& e, uint8_t* out) const {
    if (!open_) return false;
    std::ifstream f(file_, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(e.offset));
    if (e.size > 0 && !f.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(e.size))) return false;
    const crypto::Digest mac = crypto::HmacSha256(keys_.mac.data(), keys_.mac.size(), out, static_cast<size_t>(e.size));
    if (!crypto::ConstantTimeEqual(mac.data(), e.mac.data(), mac.size())) return false;
    ChaCha20(keys_.enc, e.nonce, out, static_cast<size_t>(e.size));
    return true;
}

bool Reader::Read(const std::string& path, std::vector<uint8_t>& out) const {
    const Entry* e = Find(path);
    if (!e) return false;
    out.resize(static_cast<size_t>(e->size));
    return Read(*e, out.data());
}

} // namespace pak
