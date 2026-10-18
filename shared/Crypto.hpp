#pragma once
// ---------------------------------------------------------------------------
// Kryptografie für die Anmeldung der Spieler (ohne externe Bibliothek).
//
// Ziel: Das Passwort eines Spielers verlässt nie seinen Rechner, und wer den
// Netzwerkverkehr mitliest, kann sich bei einer späteren Anmeldung nicht als
// dieser Spieler ausgeben (Challenge-Response).
//
//   Konto anlegen (einmalig):
//     Client: key = PBKDF2-HMAC-SHA256(passwort, salz, KDF_ITERATIONS, 32 Byte)
//             salz = "Mondschuerfer|" + Server-Salz (hex) + "|" + name (klein)
//     Client -> Server: name, key          (Server speichert key)
//
//   Anmelden:
//     Client -> Server: name
//     Server -> Client: nonce (16 zufällige Bytes, nur einmal gültig)
//     Client -> Server: proof = HMAC-SHA256(key, nonce + name (klein))
//     Server: rechnet dasselbe mit dem gespeicherten key und vergleicht in
//             konstanter Zeit (verrät keine Teilergebnisse über die Laufzeit).
//
// Grenzen (bewusst einfach gehalten, siehe server/README.md):
//   - Beim Anlegen des Kontos wird der key unverschlüsselt übertragen. Wer
//     genau diesen Moment mitliest, kann sich später als dieser Spieler
//     anmelden (aber er erfährt nie das Passwort selbst).
//   - Wer die Kontodatei des Servers stiehlt, kann sich ebenfalls anmelden.
//   Für echten Schutz beim Betrieb im Internet: Verbindung über ein VPN oder
//   einen verschlüsselten Tunnel (siehe Rückfragen in der Dokumentation).
//
// Die Funktionen folgen den Standards FIPS 180-4 (SHA-256), RFC 2104 (HMAC)
// und RFC 8018 (PBKDF2).
// ---------------------------------------------------------------------------
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace crypto {

using Digest = std::array<uint8_t, 32>;

Digest Sha256(const uint8_t* data, size_t len);
Digest Sha256(const std::string& s);

Digest HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen);

// Schreibt outLen Bytes Schlüsselmaterial nach out
void Pbkdf2HmacSha256(const uint8_t* password, size_t passwordLen, const uint8_t* salt, size_t saltLen,
                      uint32_t iterations, uint8_t* out, size_t outLen);

// Vergleich ohne frühen Abbruch (Laufzeit hängt nicht davon ab, wo sich a und b unterscheiden)
bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);

std::string ToHex(const uint8_t* data, size_t n);
bool FromHex(const std::string& hex, uint8_t* out, size_t n);   // false bei falscher Länge/Zeichen

// Anmeldeschlüssel eines Spielers (siehe oben). nameLower = Name in Kleinbuchstaben.
Digest DeriveLoginKey(const std::string& password, const std::string& nameLower, const std::array<uint8_t, 16>& realmSalt,
                      uint32_t iterations);

// Antwort auf die Anmelde-Herausforderung
Digest LoginProof(const Digest& key, const std::array<uint8_t, 16>& nonce, const std::string& nameLower);

// Zufallsbytes vom Betriebssystem (std::random_device). Nur auf dem Server verwendet.
void RandomBytes(uint8_t* out, size_t n);

} // namespace crypto
