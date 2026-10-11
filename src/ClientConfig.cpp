// ---------------------------------------------------------------------------
// ClientConfig.cpp: Laden und Speichern von Serveradresse und Spielername.
//
// Ablauf:
//   Load(): Datei zeilenweise lesen, jede Zeile an der ersten Leerstelle in
//           Schlüssel und Wert zerlegen, bekannte Schlüssel übernehmen.
//   Save(): Datei komplett neu schreiben (sie ist winzig).
//
// Die Datei kann von Hand bearbeitet werden. Deshalb werden die Werte beim
// Lesen "bereinigt": Leerzeichen am Rand und Steuerzeichen fallen weg, und
// überlange Werte werden gekürzt, damit ein kaputter Eintrag das Menü nicht
// durcheinanderbringt.
//
// Benutzt von: src/Multiplayer.cpp (Spiel-Thread).
// ---------------------------------------------------------------------------
#include "ClientConfig.hpp"
#include "raylib.h"
#include <fstream>
#include <string>

namespace {

// Höchstlängen beim Einlesen. Die Adresse darf einen langen DNS-Namen
// enthalten; Spielernamen sind laut Protokoll höchstens 16 Zeichen lang –
// hier etwas großzügiger, die eigentliche Prüfung macht das Anmeldefenster.
constexpr size_t kMaxServerLen = 255;
constexpr size_t kMaxNameLen = 32;

// Entfernt Steuerzeichen (z. B. das '\r' von Windows-Zeilenenden) und
// Leerraum am Anfang/Ende. Bytes >= 0x80 (UTF-8) bleiben erhalten.
std::string Clean(const std::string& in, size_t maxLen) {
    std::string out;
    for (char ch : in) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c < 32 || c == 127) continue;
        out.push_back(ch);
    }
    size_t a = out.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = out.find_last_not_of(' ');
    out = out.substr(a, b - a + 1);
    if (out.size() > maxLen) out.resize(maxLen);
    return out;
}

} // namespace

std::string ClientConfig::Path() {
    // GetApplicationDirectory() endet bereits mit einem Pfadtrenner ('/' bzw. '\').
    return std::string(GetApplicationDirectory()) + "mondschuerfer_client.txt";
}

bool ClientConfig::Load() {
    std::ifstream f(Path());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        // "Schlüssel Wert": alles nach der ersten Leerstelle ist der Wert
        // (der Wert selbst darf also Leerzeichen enthalten).
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = Clean(line.substr(0, sp), 32);
        const std::string value = line.substr(sp + 1);
        if (key == "server") {
            std::string v = Clean(value, kMaxServerLen);
            if (!v.empty()) server = v;      // leere Adresse: Standard behalten
        } else if (key == "name") {
            name = Clean(value, kMaxNameLen);
        }
        // andere Schlüssel: ignorieren (z. B. aus einer neueren Spielversion)
    }
    return true;
}

bool ClientConfig::Save() const {
    std::ofstream f(Path(), std::ios::trunc);
    if (!f) return false;
    // Beim Schreiben ebenfalls bereinigen: ein Zeilenumbruch im Wert würde
    // sonst eine zweite (falsche) Zeile erzeugen.
    f << "server " << Clean(server, kMaxServerLen) << '\n';
    f << "name " << Clean(name, kMaxNameLen) << '\n';
    // Absichtlich KEIN Passwort (siehe ClientConfig.hpp).
    return static_cast<bool>(f);
}
