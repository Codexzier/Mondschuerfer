#pragma once
// ---------------------------------------------------------------------------
// Accounts: alles, was der Server dauerhaft im Datenordner speichert.
//
// Wofür?
//   Der Server soll nach einem Neustart (z. B. "docker compose restart") noch
//   wissen, welche Spieler es gibt, wie weit sie gekommen sind, wer in der
//   Bestenliste steht und auf welcher Mondoberfläche gespielt wurde.
//
// Welche Dateien? (alle im Datenordner, Docker: /data, sonst ./data)
//   accounts.txt    Spielerkonten: Name, Anmeldeschlüssel, Zeiten, Fortschritt
//   highscores.txt  Bestenliste: jede erreichte Zielrunde eine Zeile
//   server.txt      Server-Salz (für die Anmeldeschlüssel), Welt-Seed,
//                   insgesamt abgelieferte Ressourcen (Ausbaustufe der Stationen)
//   settings.txt    nur vorhanden, wenn die Einstellungen auf der Webseite
//                   geändert wurden; überschreibt dann die Werte aus
//                   docker-compose.yml (Umgebungsvariablen)
//   ships.txt       nur vorhanden, wenn die Schiffe auf der Webseite geändert
//                   wurden; sonst gelten die Standard-Schiffe (cfg::SHIP_TYPES)
//
// Format: einfacher Text, eine Angabe pro Zeile bzw. pro Konto, "name=wert".
// So kann man die Dateien zur Not mit einem Texteditor ansehen (bei
// gestopptem Server!). Zeilen, die mit '#' beginnen, sind Kommentare.
//
// Sicherheit und Robustheit:
//   - Schreiben ist "atomar": erst in eine temporäre Datei (*.tmp), dann
//     umbenennen. Stürzt der Server mitten im Schreiben ab oder fällt der Strom
//     aus, bleibt die alte Datei vollständig erhalten.
//   - Lesen überspringt kaputte Zeilen, statt abzubrechen.
//   - Die Dateien werden (wo das Betriebssystem es erlaubt) nur für den
//     Besitzer lesbar angelegt, weil accounts.txt die Anmeldeschlüssel enthält.
//     Das Passwort selbst kennt der Server nie (siehe shared/Crypto.hpp).
//
// Wer benutzt die Klasse? Nur der Spiel-Thread (GameServer.cpp, main.cpp).
// Sie ist NICHT threadsicher; der Web-Thread greift nie direkt darauf zu.
//
// Schreib-Thread: Die Save*-Funktionen bauen den Dateiinhalt sofort (im
// Spiel-Thread, dauert Mikrosekunden) und übergeben ihn einem eigenen
// kleinen Thread, der die Datei schreibt. Grund: Das Schreiben auf eine
// SD-Karte (Raspberry Pi) kann mit fsync gelegentlich 100 ms und mehr dauern.
// Im Spiel-Thread würde das Spiel dabei kurz ruckeln. Liegt für eine Datei
// schon ein Auftrag in der Warteschlange, wird er durch den neueren ersetzt
// (es zählt nur der neueste Stand). Flush() wartet, bis alles geschrieben ist.
//
// Abhängigkeiten: shared/Protocol.hpp (Key32, Bytes16), shared/Config.hpp
// (Anzahl der Aufrüstungen), shared/GameSettings.hpp, shared/Crypto.hpp
// (Hex-Umwandlung, Zufallszahlen), std::filesystem (Ordner anlegen,
// umbenennen, Dateirechte).
// ---------------------------------------------------------------------------
#include "Config.hpp"
#include "GameSettings.hpp"
#include "Protocol.hpp"
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Fortschritt eines Spielers, so wie er zwischen zwei Anmeldungen erhalten
// bleibt. Entspricht den Feldern von PlayerState (shared/World.hpp), die nach
// dem Abmelden noch gebraucht werden.
struct PlayerProgress {
    long long balance = cfg::START_BALANCE;     // Kontostand (Ziel: cfg::GOAL_POINTS)
    long long delivered = 0;                    // selbst abgelieferte Ressourcen (Statistik)
    int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0, 0}; // Stufen 0..cfg::UPGRADE_MAX
    int shipType = 0;                           // Typ des geflogenen Schiffs (cfg::ShipTypeId)
    // Geparkte Schiffe (Raumschiffsverkäufer): Typ, eigene Stufen, Schild (-1 = voll)
    struct Parked {
        int type = 0;
        int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0, 0};
        float hp = -1.0f;
    };
    std::vector<Parked> hangar;
    int lives = -1;                             // verbleibende Leben (-1 = voll, Einstellung des Servers)
    int cargo = 0;                              // Ladung im Schiff
    float hp = -1.0f;                           // Schild beim Abmelden (-1 = voll), verhindert "Abmelden heilt"
    double shipTime = 0.0;                      // Zeit mit dem aktuellen Schiff
    double roundTime = 0.0;                     // Zeit in der aktuellen Zielrunde
    int round = 1;                              // Zielrunde mit diesem Schiff
    int kills = 0;
    int deaths = 0;
    int goals = 0;                              // so oft das Ziel erreicht
    // Ort des Schiffs beim Abmelden. Beim nächsten Anmelden geht es genau dort
    // weiter, statt an einer Handelsstation: Abmelden + Anmelden ist damit
    // keine Abkürzung zur Station (sonst spart man den Rückflug und die Zeit
    // fehlt in der Bestenliste). Gilt nur in der Welt, zu der er gehört
    // (posSeed = Welt-Seed); nach einem Neuaufbau der Karte könnte an der
    // Stelle jetzt eine Station stehen.
    bool hasPos = false;
    float posX = 0.0f, posZ = 0.0f, yaw = 0.0f;
    uint32_t posSeed = 0;
};

struct Account {
    std::string name;                 // Original-Schreibweise (z. B. "Anna_42")
    net::Key32 key{};                 // Anmeldeschlüssel = PBKDF2(Passwort, Salz), gespeichert als Hex
    int64_t created = 0;              // Unix-Zeit (Sekunden seit 1.1.1970)
    int64_t lastLogin = 0;            // Unix-Zeit der letzten Anmeldung
    double totalLoginSeconds = 0.0;   // Summe aller Sitzungen
    PlayerProgress progress;
};

// Ein Eintrag der Bestenliste: ein Spieler hat das Ziel (cfg::GOAL_POINTS)
// erreicht. round > 1 heißt: mit einem Schiff, das seine Aufrüstungen aus der
// vorigen Runde behalten hat (im Spiel und auf der Webseite so angezeigt).
struct HighscoreRecord {
    std::string name;
    double seconds = 0.0;   // gebrauchte Zeit für diese Runde
    int round = 1;
    int64_t when = 0;       // Unix-Zeit
};

class Accounts {
public:
    // Obergrenzen, damit niemand die Festplatte des Servers mit Konten oder
    // Bestenlisten-Einträgen füllen kann (z. B. mit einem Skript).
    static constexpr int MAX_ACCOUNTS = 10000;
    static constexpr size_t MAX_HIGHSCORES_STORED = 10000;   // die besten bleiben erhalten
    static constexpr size_t HIGHSCORES_SHOWN = 20;           // im Spiel und auf der Webseite

    // dataDir leer: nur im Arbeitsspeicher arbeiten (Benchmark, Tests) –
    // dann wird nichts gelesen oder geschrieben.
    explicit Accounts(std::string dataDir);
    ~Accounts();   // wartet auf ausstehende Schreibaufträge (Flush) und beendet den Schreib-Thread
    Accounts(const Accounts&) = delete;
    Accounts& operator=(const Accounts&) = delete;

    // Ordner anlegen und alle Dateien lesen. Gibt es noch keine server.txt
    // (erster Start), wird ein zufälliges Server-Salz erzeugt und gespeichert.
    // Liefert false (mit Grund in error), wenn der Ordner nicht angelegt oder
    // eine vorhandene Datei nicht gelesen werden kann. Der Server darf dann
    // NICHT starten, sonst würde er beim nächsten Speichern alles überschreiben.
    bool Load(std::string& error);
    const std::string& DataDir() const { return dir_; }
    bool InMemoryOnly() const { return dir_.empty(); }

    // --- Konten --------------------------------------------------------------
    // Suche ohne Rücksicht auf Groß/Klein ("anna" findet "Anna").
    Account* Find(const std::string& name);
    const Account* Find(const std::string& name) const;
    // Legt ein Konto an. nullptr, wenn der Name (ohne Groß/Klein) schon vergeben
    // oder MAX_ACCOUNTS erreicht ist. Speichert NICHT automatisch.
    Account* Create(const std::string& name, const net::Key32& key, int64_t now);
    int Count() const { return static_cast<int>(accounts_.size()); }
    void SaveAccounts();     // Auftrag an den Schreib-Thread

    // --- Bestenliste -------------------------------------------------------------
    void AddHighscore(const HighscoreRecord& r);    // speichert NICHT automatisch
    std::vector<HighscoreRecord> TopHighscores(size_t n = HIGHSCORES_SHOWN) const;   // kürzeste Zeit zuerst
    size_t HighscoreCount() const { return highscores_.size(); }
    void SaveHighscores();

    // --- Serverdaten -------------------------------------------------------------
    const net::Bytes16& RealmSalt() const { return realmSalt_; }
    bool HasWorldSeed() const { return hasWorldSeed_; }
    uint32_t WorldSeed() const { return worldSeed_; }
    void SetWorldSeed(uint32_t seed) { worldSeed_ = seed; hasWorldSeed_ = true; }
    long long TotalDelivered() const { return totalDelivered_; }
    void SetTotalDelivered(long long v) { totalDelivered_ = v < 0 ? 0 : v; }
    void SaveServerData();

    // --- Einstellungen von der Webseite -------------------------------------------
    // Liest settings.txt, falls vorhanden: Werte darin überschreiben "inOut"
    // (vorher mit den Standardwerten füllen). Liefert true, wenn die Datei
    // existierte. Ergebnis ist immer mit Sanitize() geprüft.
    bool LoadSettings(GameSettings& inOut) const;
    void SaveSettings(const GameSettings& s);
    void RemoveSettings();   // "Standardwerte wiederherstellen": Datei löschen
    // Schiffe (Bereich "Schiffe" der Webseite) stehen getrennt in ships.txt:
    // "Auf Standard zurücksetzen" der Spielparameter lässt sie unverändert.
    // LoadShips liefert true und füllt out, wenn die Datei gültige Schiffe enthält.
    bool LoadShips(std::vector<ShipType>& out) const;
    void SaveShips(const std::vector<ShipType>& ships);

    // Wartet, bis der Schreib-Thread alle Aufträge erledigt hat (beim Start
    // und beim Beenden). false, wenn seit dem letzten Flush etwas nicht
    // geschrieben werden konnte (Meldung steht dann schon auf stderr).
    bool Flush();

private:
    std::string PathOf(const char* file) const;
    // Auftrag an den Schreib-Thread. remove = true: Datei löschen statt schreiben.
    void QueueWrite(const std::string& path, std::string content, bool remove, const char* what);
    void WriterLoop();

    struct WriteJob {
        std::string content;
        bool remove = false;
        std::string what;   // für Fehlermeldungen ("Konten", "Bestenliste" ...)
    };
    std::mutex writeMutex_;
    std::condition_variable writeCv_;     // weckt den Schreib-Thread
    std::condition_variable idleCv_;      // meldet "alles erledigt" an Flush()
    std::map<std::string, WriteJob> pendingWrites_;   // Schlüssel: Pfad
    bool writerBusy_ = false;
    int writeErrors_ = 0;
    bool stopWriter_ = false;
    std::thread writer_;                  // wird beim ersten Auftrag gestartet

    std::string dir_;
    std::map<std::string, Account> accounts_;   // Schlüssel: Name in Kleinbuchstaben
    std::vector<HighscoreRecord> highscores_;
    net::Bytes16 realmSalt_{};
    uint32_t worldSeed_ = 0;
    bool hasWorldSeed_ = false;
    long long totalDelivered_ = 0;
};
