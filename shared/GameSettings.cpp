#include "GameSettings.hpp"
#include "Util.hpp"
#include <algorithm>

int EnemiesForDifficulty(Difficulty d, int customCount) {
    switch (d) {
        case Difficulty::Easy: return 0;
        case Difficulty::Medium: return 3;
        case Difficulty::Hard: return 20;
        case Difficulty::Custom: return std::clamp(customCount, 0, limits::MAX_ENEMY_SHIPS);
    }
    return 3;
}

void Sanitize(GameSettings& s) {
    int d = static_cast<int>(s.difficulty);
    if (d < 0 || d > 3) s.difficulty = Difficulty::Medium;
    int m = static_cast<int>(s.moon);
    if (m < 0 || m > 2) s.moon = MoonColor::Grey;
    s.enemyShips = EnemiesForDifficulty(s.difficulty, s.enemyShips);
    s.tradeStations = std::clamp(s.tradeStations, 1, limits::MAX_TRADE_STATIONS);
    s.shipyards = std::clamp(s.shipyards, 1, limits::MAX_SHIPYARDS);
    s.maxPlayers = std::clamp(s.maxPlayers, 1, limits::MAX_PLAYERS_LIMIT);

    // Servername: nur druckbare Zeichen, nicht zu lang (wird auf der Webseite
    // und im Spiel angezeigt). Steuerzeichen werden entfernt.
    std::string clean;
    for (unsigned char c : s.serverName)
        if (c >= 32 && c != 127) clean.push_back(static_cast<char>(c));
    if (clean.size() > static_cast<size_t>(limits::MAX_SERVER_NAME)) clean.resize(limits::MAX_SERVER_NAME);
    // Keine halben UTF-8-Zeichen am Ende stehen lassen
    while (!clean.empty() && (static_cast<unsigned char>(clean.back()) & 0xC0) == 0x80) clean.pop_back();
    if (!clean.empty() && (static_cast<unsigned char>(clean.back()) & 0xC0) == 0xC0) clean.pop_back();
    if (clean.empty()) clean = "Mondschürfer-Server";
    s.serverName = clean;
}

bool NeedsWorldRebuild(const GameSettings& a, const GameSettings& b) {
    return a.tradeStations != b.tradeStations || a.shipyards != b.shipyards || a.moon != b.moon;
}

const char* DifficultyName(Difficulty d) {
    switch (d) {
        case Difficulty::Easy: return "Leicht";
        case Difficulty::Medium: return "Mittel";
        case Difficulty::Hard: return "Schwer";
        case Difficulty::Custom: return "Benutzerdefiniert";
    }
    return "Mittel";
}

const char* DifficultyKey(Difficulty d) {
    switch (d) {
        case Difficulty::Easy: return "easy";
        case Difficulty::Medium: return "medium";
        case Difficulty::Hard: return "hard";
        case Difficulty::Custom: return "custom";
    }
    return "medium";
}

const char* MoonColorName(MoonColor c) {
    switch (c) {
        case MoonColor::Grey: return "Grau";
        case MoonColor::Orange: return "Orange-Rot";
        case MoonColor::Blue: return "Blau";
    }
    return "Grau";
}

const char* MoonColorKey(MoonColor c) {
    switch (c) {
        case MoonColor::Grey: return "grey";
        case MoonColor::Orange: return "orange";
        case MoonColor::Blue: return "blue";
    }
    return "grey";
}

bool ParseDifficulty(const std::string& text, Difficulty& out) {
    const std::string t = ToLowerAscii(text);
    if (t == "easy" || t == "leicht" || t == "0") { out = Difficulty::Easy; return true; }
    if (t == "medium" || t == "mittel" || t == "1") { out = Difficulty::Medium; return true; }
    if (t == "hard" || t == "schwer" || t == "2") { out = Difficulty::Hard; return true; }
    if (t == "custom" || t == "benutzerdefiniert" || t == "3") { out = Difficulty::Custom; return true; }
    return false;
}

bool ParseMoonColor(const std::string& text, MoonColor& out) {
    const std::string t = ToLowerAscii(text);
    if (t == "grey" || t == "gray" || t == "grau" || t == "0") { out = MoonColor::Grey; return true; }
    if (t == "orange" || t == "orange-rot" || t == "rot" || t == "red" || t == "original" || t == "1") {
        out = MoonColor::Orange;
        return true;
    }
    if (t == "blue" || t == "blau" || t == "2") { out = MoonColor::Blue; return true; }
    return false;
}
