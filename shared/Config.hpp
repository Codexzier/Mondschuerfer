#pragma once
// ---------------------------------------------------------------------------
// Alle Balancing-Werte an einer Stelle. Hier kann das Spiel angepasst werden.
// ---------------------------------------------------------------------------

namespace cfg {

// Spielziel: so viele Ressourcenpunkte muss der Kontostand erreichen
constexpr int GOAL_POINTS = 100000;

// Welt
constexpr float WORLD_HALF = 150.0f;        // Spielfeld geht von -WORLD_HALF bis +WORLD_HALF
constexpr float SHIP_ALTITUDE = 3.2f;       // Flughöhe über der Mondoberfläche

// Scanner (Taste E)
constexpr float SCANNER_MAX_ENERGY = 10.0f; // 10 Sekunden Betrieb
constexpr float SCANNER_DRAIN = 1.0f;       // Einheiten pro Sekunde im Betrieb
constexpr float SCANNER_REGEN = 1.0f;       // Einheiten pro Sekunde, wenn aus
constexpr float SCANNER_RADIUS = 34.0f;
constexpr float SCANNER_PULSE_TIME = 1.25f;

// Abbau (Taste F)
constexpr float MINING_TIME = 5.0f;         // Sekunden, bis das Lager gefüllt ist
constexpr float MINING_RANGE = 6.5f;

// Andocken
constexpr float DOCK_RANGE = 15.0f;

// Waffen
constexpr float PLAYER_FIRE_INTERVAL = 0.5f; // 2 Schuss pro Sekunde
constexpr float PLAYER_DAMAGE = 10.0f;
constexpr float BULLET_SPEED = 55.0f;
constexpr float BULLET_LIFE = 1.05f;
constexpr float SHIP_HIT_RADIUS = 1.7f;

// Reparatur an der Handelsstation: Punkte pro Schildpunkt
constexpr int REPAIR_COST_PER_POINT = 2;

// Upgrades: Index 0 = Startwert, danach je Stufe
constexpr int UPGRADE_LEVELS = 5;
enum Upgrade { UP_SPEED = 0, UP_ARMOR = 1, UP_WEAPON = 2, UP_CARGO = 3, UP_COUNT = 4 };

constexpr float SPEED_VALUES[UPGRADE_LEVELS + 1]  = {16.0f, 19.0f, 22.0f, 25.0f, 28.5f, 32.0f};
constexpr float ARMOR_VALUES[UPGRADE_LEVELS + 1]  = {100.0f, 150.0f, 200.0f, 260.0f, 330.0f, 420.0f};
constexpr float WEAPON_DAMAGE[UPGRADE_LEVELS + 1] = {10.0f, 14.0f, 19.0f, 25.0f, 32.0f, 40.0f};
constexpr float WEAPON_RATE[UPGRADE_LEVELS + 1]   = {0.50f, 0.45f, 0.40f, 0.36f, 0.33f, 0.30f};
constexpr int   CARGO_VALUES[UPGRADE_LEVELS + 1]  = {300, 600, 1000, 1600, 2500, 4000};

constexpr int UPGRADE_COST[UP_COUNT][UPGRADE_LEVELS] = {
    {800, 2000, 4000, 7000, 12000},    // Antrieb
    {1000, 2500, 5000, 8000, 13000},   // Panzerung
    {1000, 2500, 5000, 8000, 13000},   // Bewaffnung
    {600, 1600, 3500, 6500, 11000},    // Lager
};

// Station wächst mit der Gesamtmenge abgelieferter Ressourcen
constexpr int STATION_STAGES = 8; // Stufe 0..7
constexpr int STATION_THRESHOLDS[STATION_STAGES] = {0, 1500, 5000, 12000, 25000, 45000, 70000, 100000};

// Lagerstätten
constexpr int DEPOSIT_COUNT = 48;
constexpr float DEPOSIT_RESPAWN = 25.0f;

// Gegner
constexpr int PIRATES_BASE = 4;
constexpr int PIRATES_MAX = 11;
constexpr int MINERS_COUNT = 4;
constexpr float PIRATE_AGGRO = 30.0f;
constexpr float PIRATE_DAMAGE = 10.0f;      // 10 Treffer = 100 Schild
constexpr float ELITE_DAMAGE = 10.0f;      // auch Elite-Piraten: 10 Treffer = 100 Schild
constexpr float STATION_SAFE_RADIUS = 34.0f; // Geschütztürme schützen die Stationen
constexpr float TURRET_RANGE = 36.0f;

// Mehrspieler (nur Server)
constexpr float MP_RESPAWN_TIME = 4.0f;       // Sekunden bis zum neuen Raumschiff nach einem Absturz
constexpr int MP_DEPOSITS_PER_PLAYER = 8;     // zusätzliche Lagerstätten je weiterem Spieler
constexpr int MP_DEPOSITS_MAX = 240;          // Obergrenze für Lagerstätten auf der Karte
constexpr float MP_SPAWN_DISTANCE = 24.0f;    // Abstand des neuen Schiffs zur Handelsstation

} // namespace cfg
