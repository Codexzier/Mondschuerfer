#pragma once
// ---------------------------------------------------------------------------
// GameSettings.hpp – Spielparameter, die der Server festlegt (Mehrspieler).
//
// Woher kommen die Werte?
//   1. Standardwerte aus docker-compose.yml (Umgebungsvariablen SERVER_NAME,
//      DIFFICULTY, ENEMY_SHIPS, TRADE_STATIONS, SHIPYARDS, MOON_COLOR,
//      MAX_PLAYERS; gelesen in server/src/main.cpp).
//   2. Änderungen auf der Webseite des Servers überschreiben sie und werden in
//      /data/settings.txt gespeichert (bleiben also nach einem Neustart erhalten;
//      Lesen/Schreiben: server/src/Accounts.cpp).
//
// Wer benutzt die Einstellungen?
//   - server/src/WebServer.cpp: zeigt sie an, prüft Eingaben und reicht
//     Änderungen an den Spiel-Thread weiter.
//   - server/src/GameServer.cpp (ApplySettings): wendet sie an. Ändern sich
//     Stationen oder Mondfarbe, wird die Welt neu aufgebaut
//     (NeedsWorldRebuild), sonst wird nur die Gegnerzahl angepasst.
//   - shared/World.cpp / WorldGen.cpp: Anzahl und Lage der Stationen
//     (BuildStations), Zahl der Piraten im Server (World::Update).
//   - shared/Protocol.cpp: Der Server schickt die Einstellungen beim
//     Verbinden an jeden Client (ServerInfo und Welcome).
//   - Client (src/NetClient.cpp, Multiplayer.cpp, Assets.cpp): Der Client
//     braucht sie, um dieselbe Mondoberfläche und dieselben Stationen zu
//     erzeugen (World::BuildMirror), die Mondfarbe einzustellen und die
//     Serverdaten im Menü anzuzeigen.
//
// Im Einzelspieler werden einfach die Standardwerte unten verwendet
// (eine Handelsstation, eine Werft, grauer Mond). Der Schwierigkeitsgrad
// spielt dort keine Rolle: Die Zahl der Piraten wächst im Einzelspieler mit
// dem Spielfortschritt (cfg::PIRATES_BASE .. PIRATES_MAX, World::Update).
//
// Abhängigkeiten: nur <string>. Umsetzung: GameSettings.cpp.
// ---------------------------------------------------------------------------
#include "Config.hpp"
#include <string>

// Schwierigkeitsgrad = Anzahl feindlicher Raumschiffe (Piraten) auf der Karte.
// Die Zahlenwerte (0..3) werden im Netzwerk als ein Byte übertragen
// (Protocol.cpp WriteSettings): nicht umnummerieren.
enum class Difficulty : int {
    Easy = 0,    // Leicht: keine feindlichen Raumschiffe
    Medium = 1,  // Mittel: 3 feindliche Raumschiffe
    Hard = 2,    // Schwer: 20 feindliche Raumschiffe
    Custom = 3,  // Benutzerdefiniert: Anzahl frei wählbar (enemyShips)
};

// Farbe der Mondoberfläche (Gestein, Krater, Boden).
// Wirkt nur im Client beim Zeichnen: src/Assets.cpp färbt die Materialien der
// Modelle um (MoonTone). Für die Spielregeln ist die Farbe egal.
// Zahlenwerte wie bei Difficulty Teil des Netzwerkprotokolls.
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
// Feste Obergrenze für "maximale Spieler". Bis 64 wurde der Server gemessen
// (Bandbreite pro Snapshot, Rechenzeit pro Schritt, siehe server/README.md);
// außerdem wird der Wert im Netzwerk als ein Byte übertragen.
constexpr int MAX_PLAYERS_LIMIT = 64;
constexpr int MAX_LIVES = 100;          // Obergrenze für "Leben pro Spieler" (im Netzwerk ein Byte)
constexpr int MAX_SPECTATORS = 16;      // Zuschauer zusätzlich zu den Spielern (fest, nicht einstellbar)
constexpr int MAX_SERVER_NAME = 40;     // in BYTES (UTF-8): ein "ü" zählt doppelt
} // namespace limits

struct GameSettings {
    std::string serverName = "Mondschürfer-Server";
    Difficulty difficulty = Difficulty::Medium;
    int enemyShips = 3;        // nur bei Custom frei, sonst aus dem Schwierigkeitsgrad
    int tradeStations = 1;     // Stationen, an denen Ressourcen verkauft werden (1..5)
    int shipyards = 1;         // Werften (Aufrüststationen) (1..4)
    MoonColor moon = MoonColor::Grey;
    int maxPlayers = 16;       // gleichzeitig angemeldete Spieler (ohne Zuschauer)
    int lives = cfg::START_LIVES;   // Leben pro Spieler (1..limits::MAX_LIVES)
};

// Feindliche Schiffe für einen Schwierigkeitsgrad (bei Custom: customCount, begrenzt).
int EnemiesForDifficulty(Difficulty d, int customCount);

// Alle Werte in die erlaubten Grenzen bringen und enemyShips zum Schwierigkeitsgrad passend setzen.
// Wird überall aufgerufen, wo Einstellungen von außen kommen: Umgebungs-
// variablen, settings.txt, Webseite, Netzwerk (ein Client darf auch einem
// fremden, fehlerhaften Server nicht blind glauben) und beim Weltaufbau.
// Danach kann sich jeder andere Code auf gültige Werte verlassen.
void Sanitize(GameSettings& s);

// Unterscheiden sich zwei Einstellungen so, dass die Welt neu aufgebaut werden muss?
// (Anzahl der Stationen oder Mondfarbe)
// Stationen: Die Lage aller Stationen hängt von ihrer Anzahl ab
// (BuildStations), und Mondoberfläche/Lagerstätten richten sich danach.
// Mondfarbe: Clients übernehmen sie beim Verbinden (ServerInfo/Welcome);
// laufende Clients erfahren eine Änderung nur über ein neues Welcome, und das
// schickt der Server nach einem Neuaufbau an alle.
// Gegnerzahl, Servername und maximale Spieler gelten dagegen sofort.
bool NeedsWorldRebuild(const GameSettings& a, const GameSettings& b);

// Texte für Anzeige und Konfigurationsdateien
// Name = deutscher Anzeigetext (Spiel, Server-Log), Key = fester englischer
// Schlüssel (settings.txt, JSON der Webseite, Umgebungsvariablen). Die
// Schlüssel nie ändern, sonst werden gespeicherte Dateien nicht mehr erkannt.
const char* DifficultyName(Difficulty d);   // "Leicht", "Mittel", "Schwer", "Benutzerdefiniert"
const char* DifficultyKey(Difficulty d);    // "easy", "medium", "hard", "custom"
const char* MoonColorName(MoonColor c);     // "Grau", "Orange-Rot", "Blau"
const char* MoonColorKey(MoonColor c);      // "grey", "orange", "blue"

// Text -> Wert. Akzeptiert die Schlüssel (easy ...) und die deutschen Namen.
// Groß/Kleinschreibung egal, auch die Zahlen "0".."3" gehen.
// Liefert false, wenn der Text nicht erkannt wurde ("out" bleibt dann unverändert).
bool ParseDifficulty(const std::string& text, Difficulty& out);
bool ParseMoonColor(const std::string& text, MoonColor& out);
