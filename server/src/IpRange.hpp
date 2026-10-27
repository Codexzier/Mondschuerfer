#pragma once
// ---------------------------------------------------------------------------
// IpRange.hpp – IPv4-Adressen und -Bereiche für Sperren (Verwaltungsseite,
// Bereich "Sperren").
//
// Ein Bereich wird wie üblich als "Adresse/Präfix" geschrieben:
//   203.0.113.45      genau diese eine Adresse (Präfix 32)
//   203.0.113.0/24    alle 256 Adressen 203.0.113.0 bis 203.0.113.255
// Das Präfix sagt, wie viele Bits von links fest sind.
//
// Adressen stehen hier in "Lesereihenfolge" als Zahl: 1.2.3.4 = 0x01020304.
// (ENet speichert sie andersherum, siehe ReadableIp in GameServer.cpp.)
//
// Nur Header (inline): Benutzt von Accounts (bans.txt), GameServer und
// WebServer. WebServer.cpp darf Protocol.hpp/raylib.h nicht einbinden, daher
// stehen die Funktionen nicht in Accounts.hpp.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstdint>
#include <string>

// Größter sperrbarer Bereich: /8 (16 Millionen Adressen). Kleinere Präfixe
// wären fast immer ein Tippfehler und würden halb das Internet aussperren.
constexpr int MIN_BAN_PREFIX = 8;

// Netzmaske mit prefix Einsen von links: 24 -> 0xFFFFFF00
inline uint32_t PrefixMask(int prefix) {
    if (prefix <= 0) return 0;
    if (prefix >= 32) return 0xFFFFFFFFu;
    return ~(0xFFFFFFFFu >> prefix);
}

// "203.0.113.45" oder "203.0.113.0/24" -> net (Hostteil auf 0 gesetzt) und
// prefix. false bei ungültigem Text (auch Leerzeichen) oder prefix < MIN_BAN_PREFIX.
inline bool ParseIpRange(const std::string& text, uint32_t& net, int& prefix) {
    uint32_t ip = 0;
    size_t i = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
        int v = 0, digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 3) {
            v = v * 10 + (text[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0 || v > 255) return false;
        ip = (ip << 8) | static_cast<uint32_t>(v);
    }
    int p = 32;
    if (i < text.size() && text[i] == '/') {
        ++i;
        p = 0;
        int digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 2) {
            p = p * 10 + (text[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0 || p > 32) return false;
    }
    if (i != text.size() || p < MIN_BAN_PREFIX) return false;
    net = ip & PrefixMask(p);   // "203.0.113.45/24" wird zu 203.0.113.0/24
    prefix = p;
    return true;
}

// "203.0.113.45" bzw. "203.0.113.0/24"
inline std::string IpRangeText(uint32_t net, int prefix) {
    std::string out = std::to_string(net >> 24) + "." + std::to_string((net >> 16) & 0xFF) + "." +
                      std::to_string((net >> 8) & 0xFF) + "." + std::to_string(net & 0xFF);
    if (prefix < 32) out += "/" + std::to_string(prefix);
    return out;
}

inline bool IpInRange(uint32_t ip, uint32_t net, int prefix) {
    const uint32_t mask = PrefixMask(prefix);
    return (ip & mask) == (net & mask);
}

// Bereiche, die nicht gesperrt werden dürfen: 127.0.0.0/8 (dieser Rechner)
// und 172.16.0.0/12 (Docker-Netz). Hinter einer Docker-Adresse können alle
// Spieler stecken, die über die Docker-Weiterleitung kommen (z. B. über IPv6).
// Zwei Bereiche überschneiden sich, wenn sie im kürzeren Präfix gleich sind.
inline bool IpRangeProtected(uint32_t net, int prefix) {
    auto overlaps = [&](uint32_t otherNet, int otherPrefix) {
        const uint32_t mask = PrefixMask(std::min(prefix, otherPrefix));
        return (net & mask) == (otherNet & mask);
    };
    return overlaps(0x7F000000u, 8) || overlaps(0xAC100000u, 12);
}
