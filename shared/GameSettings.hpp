#pragma once
// ---------------------------------------------------------------------------
// Spielparameter, die der Server festlegt (Mehrspieler).
//
// Woher kommen die Werte?
//   1. Standardwerte aus docker-compose.yml (Umgebungsvariablen, siehe
//      server/src/main.cpp).
//   2. Änderungen auf der Webseite des Servers überschreiben sie und werden in
//      /data/settings.txt gespeichert (bleiben also nach einem Neustart erhalten).
//
// Der Server schickt die Einstellungen beim Verbinden an jeden Client
// (Protocol.cpp). Der Client braucht sie, um dieselbe Mondoberfläche und
// dieselben Stationen zu erzeugen und die Mondfarbe einzustellen.
//
// Im Einzelspieler werden einfach die Standardwerte unten verwendet
// (eine Handelsstation, eine Werft, grauer Mond).
// ---------------------------------------------------------------------------
#include <string>

// Schwierigkeitsgrad = Anzahl feindlicher Raumschiffe (Piraten) auf der Karte.
enum class Difficulty : int {
    Easy = 0,    // Leicht: keine feindlichen Raumschiffe
    Medium = 1,  // Mittel: 3 feindliche Raumschiffe
    Hard = 2,    // Schwer: 20 feindliche Raumschiffe
    Custom = 3,  // Benutzerdefiniert: Anzahl frei wählbar (enemyShips)
};

// Farbe der Mondoberfläche (Gestein, Krater, Boden).
enum class MoonColor : int {
    Grey = 0,    // Grau (Standard, wie im Einzelspieler)
    Orange = 1,  // Orange-Rot: die Originalfarbe der Kenney-Modelle
    Blue = 2,    // Blau
};

// Grenzen für die Einstellungen. Der Server prüft jede Änderung dagegen.
namespace limits {
constexpr int MAX_ENEMY_SHIPS = 40;     // mehr Piraten machen die 300x300-Karte zu voll
constexpr int MAX_TRADE_STATIONS = 5;
constexpr int MAX_SHIPYARDS = 4;        // Handelsstationen + Werften passen auf 9 Bauplätze
constexpr int MAX_PLAYERS_LIMIT = 64;   // feste Obergrenze für "maximale Spieler"
constexpr int MAX_SPECTATORS = 16;      // Zuschauer zusätzlich zu den Spielern
constexpr int MAX_SERVER_NAME = 40;
} // namespace limits

struct GameSettings {
    std::string serverName = "Mondschürfer-Server";
    Difficulty difficulty = Difficulty::Medium;
    int enemyShips = 3;        // nur bei Custom frei, sonst aus dem Schwierigkeitsgrad
    int tradeStations = 1;     // Stationen, an denen Ressourcen verkauft werden
    int shipyards = 1;         // Werften (Aufrüststationen)
    MoonColor moon = MoonColor::Grey;
    int maxPlayers = 16;       // gleichzeitig angemeldete Spieler (ohne Zuschauer)
};

// Feindliche Schiffe für einen Schwierigkeitsgrad (bei Custom: customCount, begrenzt).
int EnemiesForDifficulty(Difficulty d, int customCount);

// Alle Werte in die erlaubten Grenzen bringen und enemyShips zum Schwierigkeitsgrad passend setzen.
void Sanitize(GameSettings& s);

// Unterscheiden sich zwei Einstellungen so, dass die Welt neu aufgebaut werden muss?
// (Anzahl der Stationen oder Mondfarbe)
bool NeedsWorldRebuild(const GameSettings& a, const GameSettings& b);

// Texte für Anzeige und Konfigurationsdateien
const char* DifficultyName(Difficulty d);   // "Leicht", "Mittel", "Schwer", "Benutzerdefiniert"
const char* DifficultyKey(Difficulty d);    // "easy", "medium", "hard", "custom"
const char* MoonColorName(MoonColor c);     // "Grau", "Orange-Rot", "Blau"
const char* MoonColorKey(MoonColor c);      // "grey", "orange", "blue"

// Text -> Wert. Akzeptiert die Schlüssel (easy ...) und die deutschen Namen.
// Liefert false, wenn der Text nicht erkannt wurde.
bool ParseDifficulty(const std::string& text, Difficulty& out);
bool ParseMoonColor(const std::string& text, MoonColor& out);
