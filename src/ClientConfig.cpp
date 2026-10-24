// ---------------------------------------------------------------------------
// ClientConfig.cpp: Laden und Speichern der Einstellungsdatei (Serveradresse,
// Spielername, Musik an/aus und Musiklautstärke).
//
// Ablauf:
//   Load(): Datei zeilenweise lesen, jede Zeile an der ersten Leerstelle in
//           Schlüssel und Wert zerlegen, bekannte Schlüssel übernehmen.
//   Save(): Datei komplett neu schreiben (sie ist winzig).
//
// Die Datei kann von Hand bearbeitet werden. Deshalb werden die Werte beim
// Lesen "bereinigt": Leerzeichen am Rand und Steuerzeichen fallen weg,
// überlange Werte werden gekürzt und Zahlen auf ihren erlaubten Bereich
// begrenzt, damit ein kaputter Eintrag das Spiel nicht durcheinanderbringt.
//
// Kommazahlen (music_volume) werden hier von Hand gelesen und geschrieben,
// nicht mit atof/strtof/printf. Grund: Diese Funktionen richten sich nach der
// Spracheinstellung des Systems ("locale"). Auf einem deutsch eingestellten
// System kann dann aus "0.50" beim Lesen 0 werden (dort ist das Komma das
// Dezimalzeichen) – und eine auf einem solchen System geschriebene Datei
// enthielte "0,50". Die Handarbeit hier schreibt immer einen Punkt und liest
// Punkt UND Komma.
//
// Benutzt von: src/main.cpp, src/Multiplayer.cpp, src/Options.cpp (Spiel-Thread).
// ---------------------------------------------------------------------------
#include "ClientConfig.hpp"
#include "raylib.h"
#include <cmath>
#include <fstream>
#include <string>

ClientConfig g_clientConfig;

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

// Liest eine Kommazahl wie "0.5", "0,75", "1" oder ".25" (siehe Dateikopf:
// unabhängig von der Spracheinstellung). Erlaubt sind nur Ziffern und
// höchstens ein Punkt oder Komma. Liefert false bei allem anderen (dann
// behält der Aufrufer seinen Standardwert).
// Rechnung: Jede Ziffer vor dem Trennzeichen schiebt den bisherigen Wert eine
// Stelle nach links (v = v * 10 + Ziffer). Nach dem Trennzeichen zählt die
// erste Ziffer Zehntel, die nächste Hundertstel usw. (scale wird jedes Mal
// durch 10 geteilt).
bool ParseNumber(const std::string& text, float& out) {
    if (text.empty() || text.size() > 16) return false;
    double v = 0.0;
    double scale = 1.0;      // 1 = noch vor dem Trennzeichen
    bool afterSep = false;
    bool anyDigit = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            anyDigit = true;
            if (!afterSep) {
                v = v * 10.0 + (c - '0');
            } else {
                scale /= 10.0;
                v += (c - '0') * scale;
            }
        } else if ((c == '.' || c == ',') && !afterSep) {
            afterSep = true;
        } else {
            return false;
        }
    }
    if (!anyDigit) return false;
    out = static_cast<float>(v);
    return true;
}

// Schreibt einen Wert 0..1 mit genau zwei Nachkommastellen und Punkt, z. B.
// 0.5 -> "0.50", 1 -> "1.00". Gerechnet wird in ganzen Hundertsteln:
// lround(0.5 * 100) = 50 -> vor dem Punkt 50 / 100 = 0, dahinter 50 % 100 = 50.
std::string FormatVolume(float v) {
    if (!(v >= 0.0f)) v = 0.0f;   // fängt auch NaN ab (jeder Vergleich mit NaN ist false)
    if (v > 1.0f) v = 1.0f;
    const long hundredths = std::lround(v * 100.0f);
    const long frac = hundredths % 100;
    return std::to_string(hundredths / 100) + "." + (frac < 10 ? "0" : "") + std::to_string(frac);
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
        } else if (key == "music") {
            // "1" = an, "0" = aus; alles andere: Standard behalten
            const std::string v = Clean(value, 8);
            if (v == "1") musicEnabled = true;
            else if (v == "0") musicEnabled = false;
        } else if (key == "music_volume") {
            float v = 0.0f;
            if (ParseNumber(Clean(value, 16), v)) musicVolume = v > 1.0f ? 1.0f : v;   // ParseNumber liefert nie < 0
        } else if (key == "clip_volume") {
            float v = 0.0f;
            if (ParseNumber(Clean(value, 16), v)) clipVolume = v > 1.0f ? 1.0f : v;
        } else if (key == "subtitles") {
            const std::string v = Clean(value, 8);
            if (v == "1") subtitles = true;
            else if (v == "0") subtitles = false;
        } else if (key == "tilt_shift") {
            float v = 0.0f;
            if (ParseNumber(Clean(value, 16), v)) tiltShift = v > 1.0f ? 1.0f : v;
        } else if (key == "msaa") {
            // nur die Stufen aus den Optionen; alles andere: Standard behalten
            const std::string v = Clean(value, 8);
            if (v == "0" || v == "2" || v == "4" || v == "8" || v == "16") msaa = std::stoi(v);
        } else if (key == "fxaa") {
            float v = 0.0f;
            if (ParseNumber(Clean(value, 16), v)) fxaa = v > 1.0f ? 1.0f : v;
        } else if (key == "track_time") {
            // nur die Stufen aus den Optionen (Sekunden); alles andere: Standard behalten
            const std::string v = Clean(value, 8);
            if (v == "0" || v == "15" || v == "30" || v == "60" || v == "120" || v == "300") trackTime = std::stoi(v);
        }
        // andere Schlüssel: ignorieren (z. B. aus einer neueren Spielversion)
    }
    unsaved = false;   // jetzt stimmen Speicher und Datei überein
    return true;
}

bool ClientConfig::Save() {
    std::ofstream f(Path(), std::ios::trunc);
    if (!f) return false;
    // Beim Schreiben ebenfalls bereinigen: ein Zeilenumbruch im Wert würde
    // sonst eine zweite (falsche) Zeile erzeugen.
    f << "server " << Clean(server, kMaxServerLen) << '\n';
    f << "name " << Clean(name, kMaxNameLen) << '\n';
    f << "music " << (musicEnabled ? 1 : 0) << '\n';
    f << "music_volume " << FormatVolume(musicVolume) << '\n';
    f << "clip_volume " << FormatVolume(clipVolume) << '\n';
    f << "subtitles " << (subtitles ? 1 : 0) << '\n';
    f << "tilt_shift " << FormatVolume(tiltShift) << '\n';
    f << "msaa " << msaa << '\n';
    f << "fxaa " << FormatVolume(fxaa) << '\n';
    f << "track_time " << trackTime << '\n';
    // Absichtlich KEIN Passwort (siehe ClientConfig.hpp).
    f.close();   // erst beim Schließen ist sicher alles geschrieben
    if (!f) return false;
    unsaved = false;
    return true;
}
