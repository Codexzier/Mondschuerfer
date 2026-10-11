// Umsetzung von SHA-256, HMAC-SHA256 und PBKDF2-HMAC-SHA256 (siehe Crypto.hpp).
//
// SHA-256 in Kürze: Die Nachricht wird auf ein Vielfaches von 64 Byte
// aufgefüllt (eine 1, Nullen, dann die Länge in Bit). Jeder 64-Byte-Block
// wird in 64 Runden mit festen Konstanten (K) in den 8 Zustandswörtern (h)
// "verrührt". Das Ergebnis sind die 8 Wörter = 32 Byte. Schon ein geändertes
// Bit der Eingabe ändert das Ergebnis völlig, und man kann aus dem Ergebnis
// nicht auf die Eingabe zurückrechnen.
#include "Crypto.hpp"
#include <cstring>
#include <random>
#include <vector>

namespace crypto {

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// Rechenzustand, der Daten in Stücken aufnehmen kann (für HMAC/PBKDF2 nötig)
struct Sha256Ctx {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64] = {};
    size_t blockLen = 0;
    uint64_t totalBits = 0;

    void Compress(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) | (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(p[4 * i + 2]) << 8) | static_cast<uint32_t>(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void Update(const uint8_t* data, size_t len) {
        totalBits += static_cast<uint64_t>(len) * 8;
        while (len > 0) {
            size_t take = 64 - blockLen;
            if (take > len) take = len;
            std::memcpy(block + blockLen, data, take);
            blockLen += take;
            data += take;
            len -= take;
            if (blockLen == 64) {
                Compress(block);
                blockLen = 0;
            }
        }
    }

    Digest Final() {
        // Auffüllen: 0x80, Nullen bis 56 Byte, dann die Länge in Bit (8 Byte, big endian)
        const uint64_t bits = totalBits;
        const uint8_t one = 0x80;
        Update(&one, 1);
        const uint8_t zero = 0;
        while (blockLen != 56) Update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        Update(lenBytes, 8);
        Digest out{};
        for (int i = 0; i < 8; ++i) {
            out[static_cast<size_t>(4 * i)] = static_cast<uint8_t>(h[i] >> 24);
            out[static_cast<size_t>(4 * i + 1)] = static_cast<uint8_t>(h[i] >> 16);
            out[static_cast<size_t>(4 * i + 2)] = static_cast<uint8_t>(h[i] >> 8);
            out[static_cast<size_t>(4 * i + 3)] = static_cast<uint8_t>(h[i]);
        }
        return out;
    }
};

// HMAC vorbereitet: innerer und äußerer Zustand nach dem Schlüssel-Block.
// PBKDF2 ruft HMAC zehntausende Male mit demselben Schlüssel auf; so muss der
// Schlüssel nur einmal verarbeitet werden.
struct HmacKey {
    Sha256Ctx inner, outer;
    HmacKey(const uint8_t* key, size_t keyLen) {
        uint8_t k[64] = {};
        if (keyLen > 64) {
            Digest d = Sha256(key, keyLen);   // zu lange Schlüssel werden erst gehasht
            std::memcpy(k, d.data(), d.size());
        } else if (keyLen > 0) {
            std::memcpy(k, key, keyLen);
        }
        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; ++i) {
            ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
            opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
        }
        inner.Update(ipad, 64);
        outer.Update(opad, 64);
    }
    Digest Mac(const uint8_t* msg, size_t len) const {
        Sha256Ctx in = inner;
        in.Update(msg, len);
        Digest innerHash = in.Final();
        Sha256Ctx out = outer;
        out.Update(innerHash.data(), innerHash.size());
        return out.Final();
    }
};

} // namespace

Digest Sha256(const uint8_t* data, size_t len) {
    Sha256Ctx c;
    c.Update(data, len);
    return c.Final();
}

Digest Sha256(const std::string& s) { return Sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

Digest HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen) {
    return HmacKey(key, keyLen).Mac(msg, msgLen);
}

void Pbkdf2HmacSha256(const uint8_t* password, size_t passwordLen, const uint8_t* salt, size_t saltLen,
                      uint32_t iterations, uint8_t* out, size_t outLen) {
    // RFC 8018: Für jeden 32-Byte-Block i: U1 = HMAC(pw, salt || i), Uj = HMAC(pw, Uj-1),
    // Block = U1 xor U2 xor ... xor Uc. Die vielen Runden machen das Durchprobieren
    // von Passwörtern teuer.
    const HmacKey prf(password, passwordLen);
    if (iterations == 0) iterations = 1;
    std::vector<uint8_t> msg(salt, salt + saltLen);
    msg.resize(saltLen + 4);
    uint32_t blockIndex = 1;
    size_t written = 0;
    while (written < outLen) {
        msg[saltLen] = static_cast<uint8_t>(blockIndex >> 24);
        msg[saltLen + 1] = static_cast<uint8_t>(blockIndex >> 16);
        msg[saltLen + 2] = static_cast<uint8_t>(blockIndex >> 8);
        msg[saltLen + 3] = static_cast<uint8_t>(blockIndex);
        Digest u = prf.Mac(msg.data(), msg.size());
        Digest t = u;
        for (uint32_t i = 1; i < iterations; ++i) {
            u = prf.Mac(u.data(), u.size());
            for (size_t k = 0; k < t.size(); ++k) t[k] ^= u[k];
        }
        size_t take = outLen - written;
        if (take > t.size()) take = t.size();
        std::memcpy(out + written, t.data(), take);
        written += take;
        ++blockIndex;
    }
}

bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

std::string ToHex(const uint8_t* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 15]);
    }
    return s;
}

bool FromHex(const std::string& hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        int hi = val(hex[2 * i]), lo = val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

Digest DeriveLoginKey(const std::string& password, const std::string& nameLower, const std::array<uint8_t, 16>& realmSalt,
                      uint32_t iterations) {
    // Salz = fester Text + Salz des Servers + Name: Gleiche Passwörter ergeben auf
    // verschiedenen Servern und für verschiedene Namen verschiedene Schlüssel.
    const std::string salt = "Mondschuerfer|" + ToHex(realmSalt.data(), realmSalt.size()) + "|" + nameLower;
    Digest key{};
    Pbkdf2HmacSha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
                     reinterpret_cast<const uint8_t*>(salt.data()), salt.size(), iterations, key.data(), key.size());
    return key;
}

Digest LoginProof(const Digest& key, const std::array<uint8_t, 16>& nonce, const std::string& nameLower) {
    std::vector<uint8_t> msg(nonce.begin(), nonce.end());
    msg.insert(msg.end(), nameLower.begin(), nameLower.end());
    return HmacSha256(key.data(), key.size(), msg.data(), msg.size());
}

void RandomBytes(uint8_t* out, size_t n) {
    // std::random_device liefert unter Linux Zufall aus /dev/urandom (kryptografisch geeignet).
    static std::random_device rd;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = rd();
        for (size_t k = 0; k < 4 && i + k < n; ++k) out[i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
}

} // namespace crypto
