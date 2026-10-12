// ---------------------------------------------------------------------------
// GameSettings.cpp – Prüfen und Umwandeln der Server-Einstellungen
// (Schnittstelle und Erklärung, woher die Werte kommen: GameSettings.hpp).
//
// Inhalt:
//   - EnemiesForDifficulty: Schwierigkeitsgrad -> Anzahl Piraten (Balancing).
//   - Sanitize: alle Werte in erlaubte Grenzen bringen. Das ist die eine
//     Stelle, an der ungültige Werte abgefangen werden, egal ob sie aus einer
//     Umgebungsvariablen, aus settings.txt, von der Webseite oder über das
//     Netzwerk kommen.
//   - NeedsWorldRebuild: Entscheidung "Welt neu aufbauen oder nur anpassen".
//   - Text <-> Wert für Anzeige, Dateien und Webseite.
//
// Wird von Server, Client und Lasttest benutzt (Teil von shared/).
// Abhängigkeiten: Util.hpp (ToLowerAscii), <algorithm> (std::clamp).
// ---------------------------------------------------------------------------
#include "GameSettings.hpp"
#include "Util.hpp"
#include <algorithm>

// Balancing: Mit 3 Piraten (Mittel) begegnet man auf der 300 x 300 großen
// Karte nur ab und zu einem; mit 20 (Schwer) fast ständig. Zum Vergleich:
// Im Einzelspieler sind es anfangs 4 und höchstens 11 (cfg::PIRATES_BASE/MAX).
// Die feste Zahl gilt für die ganze Karte, unabhängig von der Spielerzahl.
int EnemiesForDifficulty(Difficulty d, int customCount) {
    switch (d) {
        case Difficulty::Easy: return 0;
        case Difficulty::Medium: return 3;
        case Difficulty::Hard: return 20;
        case Difficulty::Custom: return std::clamp(customCount, 0, limits::MAX_ENEMY_SHIPS);
    }
    // Nur erreichbar, wenn d keinen der vier Werte hat (z. B. eine kaputte
    // Zahl aus dem Netzwerk). Ohne diese Zeile warnt der Compiler außerdem,
    // dass die Funktion ohne Rückgabewert enden könnte.
    return 3;
}

void Sanitize(GameSettings& s) {
    // Aufzählungen: Ein static_cast aus einer beliebigen Zahl ergibt in C++
    // ein gültiges Objekt mit einem Wert, den die Aufzählung gar nicht kennt.
    // Deshalb hier über den Zahlenwert prüfen.
    int d = static_cast<int>(s.difficulty);
    if (d < 0 || d > 3) s.difficulty = Difficulty::Medium;
    int m = static_cast<int>(s.moon);
    if (m < 0 || m > 2) s.moon = MoonColor::Grey;
    // Bei Leicht/Mittel/Schwer wird enemyShips überschrieben; nur bei
    // Benutzerdefiniert bleibt der (begrenzte) eigene Wert.
    s.enemyShips = EnemiesForDifficulty(s.difficulty, s.enemyShips);
    // Mindestens eine Station jeder Art: Das Spiel braucht eine
    // Handelsstation (Verkaufen, Reparieren; sie ist immer stations[0] in der
    // Kartenmitte) und eine Werft (Aufrüsten).
    s.tradeStations = std::clamp(s.tradeStations, 1, limits::MAX_TRADE_STATIONS);
    s.shipyards = std::clamp(s.shipyards, 1, limits::MAX_SHIPYARDS);
    s.maxPlayers = std::clamp(s.maxPlayers, 1, limits::MAX_PLAYERS_LIMIT);

    // Servername: nur druckbare Zeichen, nicht zu lang (wird auf der Webseite
    // und im Spiel angezeigt). Steuerzeichen werden entfernt.
    // Bytes < 32 sind Steuerzeichen (Zeilenumbruch, Tabulator ...), 127 ist
    // "DEL". Bytes ab 128 bleiben erhalten: Das sind die Teile von
    // UTF-8-Zeichen wie "ü", die erlaubt sein sollen.
    std::string clean;
    for (unsigned char c : s.serverName)
        if (c >= 32 && c != 127) clean.push_back(static_cast<char>(c));
    if (clean.size() > static_cast<size_t>(limits::MAX_SERVER_NAME)) {
        clean.resize(limits::MAX_SERVER_NAME);
        // Beim Kürzen kein halbes UTF-8-Zeichen (z. B. halbes "ü") am Ende stehen lassen:
        // Anfang der letzten Bytefolge suchen und prüfen, ob sie vollständig ist.
        // Kurz zu UTF-8: Ein Zeichen besteht aus 1 bis 4 Bytes. Das erste
        // ("Startbyte") verrät die Länge: 0xxxxxxx = 1 Byte (ASCII),
        // 110xxxxx (ab 0xC0) = 2, 1110xxxx (ab 0xE0) = 3, 11110xxx (ab 0xF0)
        // = 4 Bytes. Alle Folgebytes haben die Form 10xxxxxx; das prüft
        // "(byte & 0xC0) == 0x80".
        // Schritt 1: von hinten über die Folgebytes zurückgehen. Danach ist
        // clean[k - 1] das Startbyte des letzten Zeichens.
        size_t k = clean.size();
        while (k > 0 && (static_cast<unsigned char>(clean[k - 1]) & 0xC0) == 0x80) --k;
        if (k > 0) {
            // Schritt 2: Wie viele Bytes bräuchte dieses Zeichen, und wie
            // viele sind noch da? Fehlen welche, das ganze Zeichen weglassen.
            const unsigned char lead = static_cast<unsigned char>(clean[k - 1]);
            const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (clean.size() - (k - 1) < need) clean.resize(k - 1);
        }
    }
    if (clean.empty()) clean = "Mondschürfer-Server";
    s.serverName = clean;
}

bool NeedsWorldRebuild(const GameSettings& a, const GameSettings& b) {
    return a.tradeStations != b.tradeStations || a.shipyards != b.shipyards || a.moon != b.moon;
}

// Die folgenden vier Funktionen haben nach dem switch noch ein return, falls
// der Wert ungültig ist (siehe EnemiesForDifficulty). Da Sanitize vorher
// aufgeräumt hat, ist das nur eine Absicherung.
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

// Bewusst großzügig: Wer docker-compose.yml von Hand ausfüllt, soll mit
// "Schwer", "hard" oder "2" gleichermaßen Erfolg haben. Unbekannter Text
// lässt den alten Wert stehen; der Aufrufer meldet dann eine Warnung
// (server/src/main.cpp) bzw. einen Fehler (Webseite).
bool ParseDifficulty(const std::string& text, Difficulty& out) {
    const std::string t = ToLowerAscii(text);
    if (t == "easy" || t == "leicht" || t == "0") { out = Difficulty::Easy; return true; }
    if (t == "medium" || t == "mittel" || t == "1") { out = Difficulty::Medium; return true; }
    if (t == "hard" || t == "schwer" || t == "2") { out = Difficulty::Hard; return true; }
    if (t == "custom" || t == "benutzerdefiniert" || t == "3") { out = Difficulty::Custom; return true; }
    return false;
}

// Auch hier mehrere Schreibweisen: britisch "grey", amerikanisch "gray",
// deutsch; für Orange zusätzlich "rot"/"red" und "original" (die
// Originalfarbe der Kenney-Modelle).
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
