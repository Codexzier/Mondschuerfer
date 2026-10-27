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
//                   insgesamt abgelieferte Ressourcen (Ausbaustufe der Stationen),
//                   aktive Karte
//   settings.txt    nur vorhanden, wenn die Einstellungen auf der Webseite
//                   geändert wurden; überschreibt dann die Werte aus
//                   docker-compose.yml (Umgebungsvariablen)
//   ships.txt       nur vorhanden, wenn die Schiffe auf der Webseite geändert
//                   wurden; sonst gelten die Standard-Schiffe (cfg::SHIP_TYPES)
//   karten/         hochgeladene Karten aus dem Karteneditor, je Karte eine
//                   Datei <besitzer>-<stamm>.karte (Format: shared/GameMap.hpp)
//   bans.txt        gesperrte Konten und IP-Adressen (Verwaltungsseite,
//                   Bereich "Sperren"); nur vorhanden, wenn es Sperren gab
//   logs/           Lastprotokoll: je Tag eine Datei last-JJJJ-MM-TT.csv mit
//                   einer Zeile pro Minute (GameServer::RecordLoad); Dateien
//                   älter als LOAD_LOG_DAYS werden gelöscht
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
#include "GameMap.hpp"
#include "GameSettings.hpp"
#include "IpRange.hpp"
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
    bool mapUpload = false;           // darf Karten hochladen (Verwaltungsseite, Bereich "Karten")
    PlayerProgress progress;
};

// Eine hochgeladene Karte (Karteneditor "An Server senden")
struct ServerMap {
    std::string file;       // Dateiname ohne Endung: <besitzer in Kleinbuchstaben>-<MapFileStem(Name)>
    GameMap map;            // map.author = Konto, das sie hochgeladen hat
    int loops = 0;          // geschlossene Monorail-Kreise (für die Webseite)
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

// Eine Sperre (Verwaltungsseite, Bereich "Sperren"): ein Konto oder eine
// IPv4-Adresse bzw. ein ganzer Adressbereich ("203.0.113.0/24", IpRange.hpp).
struct Ban {
    bool isIp = false;
    std::string target;     // Konto: Name (Originalschreibweise); IP: Text wie IpRangeText
    uint32_t net = 0;       // nur IP: erste Adresse des Bereichs in Lesereihenfolge (1.2.3.4 = 0x01020304)
    int prefix = 32;        // nur IP: Länge der Netzmaske (MIN_BAN_PREFIX..32, 32 = eine Adresse)
    std::string reason;     // Grund, wird dem Spieler angezeigt (darf leer sein)
    int64_t created = 0;    // Unix-Zeit
    int64_t until = 0;      // Unix-Zeit des Endes, 0 = dauerhaft
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

    // --- Karten ----------------------------------------------------------------------
    // Alle Karten werden beim Start gelesen und bleiben im Speicher (eine
    // große Karte braucht etwa 0,5 MB); deshalb gibt es Obergrenzen.
    static constexpr int MAX_MAPS = 20;
    static constexpr int MAX_MAPS_PER_PLAYER = 5;
    const std::vector<ServerMap>& Maps() const { return maps_; }
    const ServerMap* FindMap(const std::string& file) const;
    // Speichert eine hochgeladene Karte (m.author muss gesetzt sein). Gleicher
    // Name vom selben Spieler ersetzt die alte Fassung. Liefert den Dateinamen
    // in file, oder false + Grund in error (Obergrenze erreicht).
    bool StoreMap(const GameMap& m, std::string& file, std::string& error);
    bool RemoveMap(const std::string& file);
    // Aktive Karte für alle Spieler ("" = Standardkarte aus dem Seed). Steht in
    // server.txt; SetActiveMap speichert NICHT automatisch (SaveServerData).
    const std::string& ActiveMap() const { return activeMap_; }
    void SetActiveMap(const std::string& file) { activeMap_ = file; }
    std::vector<std::string> MapUploaders() const;   // Konten mit Account::mapUpload, nach Name sortiert

    // --- Sperren ---------------------------------------------------------------------
    // Alle Sperren stehen im Speicher; abgelaufene entfernt RemoveExpiredBans.
    // Add/Remove/RemoveExpired speichern NICHT automatisch (SaveBans).
    static constexpr size_t MAX_BANS = 1000;
    const std::vector<Ban>& Bans() const { return bans_; }
    // Neue Sperre; eine vorhandene für dasselbe Konto bzw. denselben Bereich
    // wird ersetzt. false, wenn MAX_BANS erreicht ist.
    bool AddBan(const Ban& b);
    bool RemoveBan(bool isIp, const std::string& target);   // target ohne Rücksicht auf Groß/Klein
    // Entfernt Sperren mit 0 < until <= now und hängt sie an removed an.
    void RemoveExpiredBans(int64_t now, std::vector<Ban>& removed);
    void SaveBans();

    // --- Lastprotokoll ---------------------------------------------------------------
    static constexpr int LOAD_LOG_DAYS = 30;
    std::string LoadLogDir() const;   // Ordner der Tagesdateien ("" = nur im Arbeitsspeicher)
    // Hängt line an logs/last-<day>.csv an (day = "JJJJ-MM-TT"); eine neue Datei
    // beginnt mit header. Beim ersten Eintrag eines Tages werden Dateien gelöscht,
    // die älter als LOAD_LOG_DAYS sind (pruneBefore = ältester Tag, der bleibt).
    // Schreibt über den Schreib-Thread, ohne fsync (eine verlorene Zeile ist egal).
    void AppendLoadLog(const std::string& day, const std::string& pruneBefore, const std::string& header,
                       const std::string& line);

    // Wartet, bis der Schreib-Thread alle Aufträge erledigt hat (beim Start
    // und beim Beenden). false, wenn seit dem letzten Flush etwas nicht
    // geschrieben werden konnte (Meldung steht dann schon auf stderr).
    bool Flush();

private:
    std::string PathOf(const char* file) const;
    std::string MapPath(const std::string& file) const;   // karten/<file>.karte
    // Auftrag an den Schreib-Thread. remove = true: Datei löschen statt schreiben.
    void QueueWrite(const std::string& path, std::string content, bool remove, const char* what);
    void WriterLoop();

    struct WriteJob {
        std::string content;
        bool remove = false;
        bool append = false;      // content anhängen statt die Datei zu ersetzen (header bei neuer Datei)
        bool pruneLogs = false;   // Pfad = Ordner logs/, content = ältester Tag, der bleibt
        std::string header;
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
    std::vector<ServerMap> maps_;           // nach Kartenname sortiert
    std::string activeMap_;
    std::vector<Ban> bans_;                 // Reihenfolge: älteste zuerst
    std::string loadLogDay_;                // Tag des letzten Eintrags im Lastprotokoll
};
