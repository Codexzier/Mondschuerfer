#pragma once
// ---------------------------------------------------------------------------
// Datenaustausch zwischen dem Spiel-Thread und dem Web-Thread des Servers.
//
// Der Server hat zwei Threads (gleichzeitig laufende Programmteile):
//   - Spiel-Thread (GameServer): rechnet 30-mal pro Sekunde die Welt, spricht
//     über ENet mit den Spielern.
//   - Web-Thread (WebServer, cpp-httplib): beantwortet Anfragen der
//     Verwaltungs-Webseite.
//
// Beide dürfen niemals gleichzeitig dieselben Daten verändern. Deshalb gibt es
// diese kleine Klasse mit einem Mutex (Sperre):
//   - Der Spiel-Thread veröffentlicht einmal pro Sekunde eine KOPIE seines
//     Zustands (PublishStatus). Die Webseite liest nur diese Kopie.
//   - Die Webseite legt geänderte Einstellungen hier ab (RequestSettings,
//     Schiffe getrennt mit RequestShips, Karten mit RequestMapCommand); der
//     Spiel-Thread holt sie zu Beginn des nächsten Schritts ab und wendet sie
//     an (TakeSettingsRequest, TakeShipsRequest, TakeMapCommands).
// So greift der Web-Thread nie direkt auf die Spielwelt zu.
// ---------------------------------------------------------------------------
#include "GameSettings.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct PlayerStatus {
    int id = 0;
    std::string name;
    double progressPercent = 0.0;   // Kontostand / Ziel * 100
    long long balance = 0;
    int round = 1;                  // Zielrunde mit diesem Schiff
    int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0, 0};
    int lives = 0;
    double shipTime = 0.0;          // Sekunden mit dem aktuellen Schiff
    double sessionSeconds = 0.0;    // Sekunden seit dieser Anmeldung
    double totalLoginSeconds = 0.0; // Sekunden insgesamt angemeldet (alle Sitzungen)
    int kills = 0;
    int deaths = 0;
    int goals = 0;
    bool alive = false;
    bool docked = false;
    int pingMs = 0;
};

struct HighscoreStatus {
    std::string name;
    double seconds = 0.0;
    int round = 1;
    int64_t when = 0;               // Unix-Zeit
};

// Eine hochgeladene Karte (Bereich "Karten" der Webseite)
struct MapStatus {
    std::string file;               // Kennung (Dateiname ohne Endung)
    std::string name;
    std::string author;
    int pieces = 0;
    int loops = 0;                  // geschlossene Monorail-Kreise
};

// Auftrag der Webseite für den Bereich "Karten"
struct MapCommand {
    enum class Kind { Activate, Delete, Allow, Deny };
    Kind kind = Kind::Activate;
    std::string arg;                // Activate/Delete: Datei ("" = Standardkarte), Allow/Deny: Spielername
};

struct ServerStatus {
    std::string serverName;
    double uptimeSeconds = 0.0;
    int tickRate = 30;
    double avgTickMs = 0.0;         // durchschnittliche Rechenzeit pro Schritt (letzte Sekunde)
    double maxTickMs = 0.0;         // längster Schritt (letzte Sekunde)
    double loadPercent = 0.0;       // avgTickMs / (1000 / tickRate) * 100
    double cpuPercent = 0.0;        // CPU-Zeit des Prozesses / Wandzeit * 100 (ein Kern = 100)
    double memoryMB = 0.0;          // belegter Arbeitsspeicher (Linux: VmRSS)
    int cpuCores = 0;
    int playersOnline = 0;
    int maxPlayers = 0;
    int maxPlayersLimit = 0;
    int spectators = 0;
    int maxSpectators = 0;
    double netInKBs = 0.0;          // empfangene KB/s
    double netOutKBs = 0.0;         // gesendete KB/s
    long long totalDelivered = 0;
    int stationStage = 0;
    int enemiesAlive = 0;
    int registeredAccounts = 0;
    int gamePort = 0;
    std::vector<PlayerStatus> players;
    std::vector<HighscoreStatus> highscores;
    GameSettings settings;          // aktuell gültig
    GameSettings defaults;          // aus docker-compose.yml / Umgebungsvariablen
    std::vector<std::string> shipModels;   // wählbare Schiffsmodelle (ShipModelNames, Models.hpp)
    std::vector<MapStatus> maps;
    std::string activeMap;          // Datei der aktiven Karte, "" = Standardkarte
    std::vector<std::string> mapUploaders;   // Spieler, die Karten hochladen dürfen
    std::string mapNotice;          // Ergebnis des letzten Karten-Auftrags (z. B. "Konto nicht gefunden")
    int mapNoticeSeq = 0;           // zählt bei jeder neuen Meldung hoch
    bool mapNoticeError = false;    // Meldung ist ein Fehler (rot anzeigen)
};

class SharedState {
public:
    // --- Spiel-Thread ---------------------------------------------------------
    void PublishStatus(const ServerStatus& s) {
        std::lock_guard<std::mutex> lock(m_);
        status_ = s;
        hasStatus_ = true;
    }
    // Liefert true und füllt out, wenn die Webseite neue Einstellungen geschickt hat.
    // reset = true: zurück auf die Standardwerte aus docker-compose.yml.
    bool TakeSettingsRequest(GameSettings& out, bool& reset) {
        std::lock_guard<std::mutex> lock(m_);
        if (!pending_) return false;
        out = pendingSettings_;
        reset = pendingReset_;
        pending_ = false;
        pendingReset_ = false;
        return true;
    }

    // Liefert true und füllt out, wenn die Webseite geänderte Schiffe geschickt hat.
    bool TakeShipsRequest(std::vector<ShipType>& out) {
        std::lock_guard<std::mutex> lock(m_);
        if (!pendingShips_) return false;
        out = pendingShipList_;
        pendingShips_ = false;
        return true;
    }

    // Holt alle Aufträge für den Bereich "Karten" (älteste zuerst).
    bool TakeMapCommands(std::vector<MapCommand>& out) {
        std::lock_guard<std::mutex> lock(m_);
        if (mapCommands_.empty()) return false;
        out.swap(mapCommands_);
        mapCommands_.clear();
        return true;
    }

    // --- Web-Thread -------------------------------------------------------------
    bool GetStatus(ServerStatus& out) const {
        std::lock_guard<std::mutex> lock(m_);
        out = status_;
        return hasStatus_;
    }
    void RequestSettings(const GameSettings& s) {
        std::lock_guard<std::mutex> lock(m_);
        pendingSettings_ = s;
        pending_ = true;
        pendingReset_ = false;
    }
    void RequestReset() {
        std::lock_guard<std::mutex> lock(m_);
        pending_ = true;
        pendingReset_ = true;
    }

    void RequestShips(const std::vector<ShipType>& ships) {
        std::lock_guard<std::mutex> lock(m_);
        pendingShipList_ = ships;
        pendingShips_ = true;
    }

    // false, wenn schon zu viele Aufträge warten (der Spiel-Thread hängt)
    bool RequestMapCommand(const MapCommand& c) {
        std::lock_guard<std::mutex> lock(m_);
        if (mapCommands_.size() >= 32) return false;
        mapCommands_.push_back(c);
        return true;
    }

private:
    mutable std::mutex m_;
    ServerStatus status_;
    bool hasStatus_ = false;
    GameSettings pendingSettings_;
    bool pending_ = false;
    bool pendingReset_ = false;
    std::vector<ShipType> pendingShipList_;
    bool pendingShips_ = false;
    std::vector<MapCommand> mapCommands_;
};
