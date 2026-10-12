#pragma once
// ---------------------------------------------------------------------------
// GameServer: das Herz des Mehrspieler-Servers.
//
// Was macht er?
//   - Öffnet einen ENet-Host (UDP-Port, Standard 7777) und spricht mit allen
//     Clients nach dem Protokoll aus shared/Protocol.hpp (Ablauf: docs/Mehrspieler.md bzw.
//     Kommentar in Protocol.hpp).
//   - Rechnet die gemeinsame Spielwelt (shared/World.hpp) mit festen 30
//     Schritten pro Sekunde. Jeder Spieler steuert sein Schiff über
//     Input-Nachrichten; der Server entscheidet, was wirklich passiert
//     ("autoritativer Server"). Clients sagen ihr eigenes Schiff nur voraus.
//   - Schickt jedem Client, was er sehen darf: Snapshots (Momentaufnahmen)
//     der Umgebung, Ereignisse (Töne, Explosionen, Meldungen), entdeckte
//     Lagerstätten, Spielerliste und Bestenliste.
//   - Verwaltet Anmeldung (Konto anlegen, Challenge-Response) und speichert
//     den Fortschritt über Accounts (Datenordner).
//   - Veröffentlicht jede Sekunde einen Statusbericht für die Webseite und
//     übernimmt dort geänderte Einstellungen (SharedState).
//
// Threads: Alles in dieser Klasse läuft im Spiel-Thread (dem Haupt-Thread
// des Programms, siehe main.cpp). Der Web-Thread berührt sie nie; er spricht
// nur über SharedState (mit Mutex) mit ihr.
//
// Ablauf der Hauptschleife (Run):
//   warten auf Netzwerkpakete, höchstens bis zum nächsten Schritt
//     -> Pakete verarbeiten (Anmeldung, Eingaben in Warteschlangen legen ...)
//   alle 33,3 ms ein Schritt (Step):
//     Einstellungen von der Webseite anwenden -> je Spieler eine Eingabe aus
//     der Warteschlange nehmen -> World::Update -> Ereignisse verteilen ->
//     Snapshots/Lagerstätten/Listen senden -> ggf. speichern
//
// Benchmark (RunBenchmark): dieselbe Step()-Funktion, aber ohne Netzwerk.
// Statt echter Clients gibt es "Bots", deren Eingaben im Programm erzeugt
// werden; gesendete Bytes werden nur gezählt. So misst der Benchmark genau
// die Arbeit, die der echte Server pro Schritt erledigt.
//
// Abhängigkeiten: ENet (nur in GameServer.cpp eingebunden, hier nur
// vorwärts deklariert, damit main.cpp ENet nicht kennen muss),
// shared/World.hpp, shared/Protocol.hpp, shared/Crypto.hpp, Accounts.hpp,
// SharedState.hpp.
// ---------------------------------------------------------------------------
#include "Accounts.hpp"
#include "GameSettings.hpp"
#include "Protocol.hpp"
#include "SharedState.hpp"
#include "World.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

// ENet-Typen nur vorwärts deklarieren (vollständig in <enet/enet.h>).
struct _ENetHost;
struct _ENetPeer;

// Startkonfiguration (aus Umgebungsvariablen/Kommandozeile, siehe main.cpp)
struct ServerConfig {
    int port = net::DEFAULT_PORT;
    GameSettings defaults;          // aus docker-compose.yml / Umgebungsvariablen ("Standardwerte")
    GameSettings settings;          // Startwerte: defaults oder settings.txt (Webseite)
    // Höchstens so viele gleichzeitige Verbindungen von EINER öffentlichen
    // IP-Adresse (Schutz davor, dass ein Einzelner alle Plätze belegt).
    // Gilt auch für halboffene Verbindungen (GuardHalfOpenPeers) und für
    // gerade schließende (OnConnect).
    // 0 = unbegrenzt. Lokale/private Adressen (127.x, 10.x, 172.16-31.x,
    // 192.168.x) sind ausgenommen: LAN, Docker-Weiterleitung, Lasttest.
    int maxConnectionsPerIp = 8;
};

class GameServer {
public:
    // shared darf nullptr sein (Benchmark: keine Webseite)
    GameServer(const ServerConfig& config, Accounts& accounts, SharedState* shared);
    ~GameServer();
    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;

    // ENet starten, Port öffnen, Welt aufbauen. false + Grund, wenn das nicht geht.
    bool Start(std::string& error);
    // Hauptschleife, bis stop gesetzt wird (Signal SIGINT/SIGTERM, siehe main.cpp)
    void Run(const std::atomic<bool>& stop);
    // Alle speichern, Clients benachrichtigen ("Server wird beendet"), aufräumen
    void Shutdown();

    // --benchmark N --seconds S: misst Rechenzeit und Datenmenge für N Spieler
    // ohne Netzwerk und gibt eine Einschätzung aus. Rückgabe = Exit-Code.
    static int RunBenchmark(int players, double seconds, const GameSettings& settings);

private:
    enum class Role { Lobby, Spectator, Player };

    // Simulierter Spieler für den Benchmark: erzeugt abwechslungsreiche Eingaben
    struct BotBrain {
        std::mt19937 rng;
        float thrust = 1.0f;
        float turn = 0.0f;
        bool fire = false;
        float changeT = 0.0f;       // bis zum nächsten Richtungswechsel
        float scanT = 3.0f;         // bis zum nächsten Druck auf "Scanner"
        float interactT = 2.0f;     // bis zum nächsten Druck auf "Interaktion"
        float dockedT = 0.0f;       // so lange schon angedockt
        uint32_t seq = 0;
        uint8_t scanPresses = 0, interactPresses = 0;
        std::deque<net::InputRecord> recent;   // wie ein echter Client: die letzten 4 Eingaben mitschicken
    };

    // Eine Verbindung (echter Client) oder ein Bot (Benchmark, peer == nullptr)
    struct Client {
        uint32_t connId = 0;                // fortlaufende Nummer (nur für Meldungen)
        _ENetPeer* peer = nullptr;
        uint32_t ip = 0;                    // IPv4 in Netzwerk-Bytereihenfolge (wie ENet)
        std::string ipText;
        Role role = Role::Lobby;
        bool closing = false;               // Trennung eingeleitet: nichts mehr annehmen/senden
        double connectedAt = 0.0;           // Now() beim Verbinden
        int badPackets = 0;                 // kaputte/unbekannte Nachrichten
        int msgsThisSecond = 0;             // einfache Flutbremse

        // --- Anmeldung (Lobby) ---
        bool challengePending = false;      // LoginChallenge geschickt, LoginProof erwartet
        net::Bytes16 nonce{};
        std::string challengeName;          // Name aus LoginStart (ungeprüft!)
        int failedLogins = 0;               // pro Verbindung höchstens 5

        // --- Spieler ---
        int playerId = -1;                  // PlayerState::id in der Welt
        Account* account = nullptr;         // Konto (Zeiger in Accounts, bleibt gültig); Bots: nullptr
        std::string name;
        double sessionStart = 0.0;          // Now() bei der Anmeldung
        double loginAccountedUntil = 0.0;   // bis hierhin ist die Anmeldezeit im Konto verbucht

        // --- Eingaben (siehe HandleInput / ApplyInputs) ---
        std::deque<net::InputRecord> inputs;   // empfangen, noch nicht verbraucht
        bool anyInput = false;
        uint32_t lastReceivedSeq = 0;
        uint32_t ackSeq = 0;                // zuletzt angewendet (geht im Snapshot zurück)
        bool countersKnown = false;
        uint8_t lastScanPresses = 0, lastInteractPresses = 0;
        net::InputRecord held;              // zuletzt angewendete Eingabe (wenn nichts Neues kommt)
        int queueMinInWindow = 1 << 30;     // kürzeste Schlange im laufenden Fenster (ApplyInputs)
        int queueWindowSteps = 0;           // Schritte im laufenden Fenster
        uint64_t noInteractUntilTick = 0;   // kurz nach dem Abdocken "Interaktion" ignorieren (sonst sofort wieder angedockt)

        // --- Senden ---
        uint64_t timeBaseTick = 0;          // serverTime in Snapshots zählt ab hier (siehe ClientTime)
        int lastShipId = -1;                // für das Ereignis "Respawned"
        int lastDeaths = 0;                 // Absturz erkennen -> speichern
        Vector3 lastPos{};                  // Mittelpunkt der Sichtweite (auch ohne Schiff)
        bool depositsSent = false;
        int sentDepositRevision = -1, sentRevealRevision = -1;
        uint64_t sentDepositHash = 0;
        uint64_t lastDepositsTick = 0;
        std::vector<net::NetEvent> events;  // Ereignisse dieses Schritts

        // --- Statistik (Benchmark) ---
        uint64_t bytesSent = 0, packetsSent = 0, snapshotBytes = 0, snapshotCount = 0, eventBytes = 0;

        bool isBot = false;
        BotBrain bot;
    };

    // Spieler, der sich außerhalb einer Station abgemeldet hat: Sein Schiff
    // bleibt noch LOGOUT_GRACE Sekunden ohne Steuerung in der Welt und kann
    // abgeschossen werden (siehe LogoutPlayer). Die Verbindung ist schon weg.
    struct Departing {
        int playerId = -1;
        Account* account = nullptr;
        std::string name;
        double until = 0.0;         // Now(), ab dem das Schiff verschwindet
        int deaths = 0;             // PlayerState::deaths beim Abmelden (erhöht = abgeschossen)
    };

    // --- Zeit -------------------------------------------------------------------
    double Now() const;                                 // Sekunden seit Serverstart (steady_clock)
    float ClientTime(const Client& c) const;            // serverTime für diesen Client

    // --- Welt -------------------------------------------------------------------
    void InitWorld();
    void RebuildWorld();
    void ApplySettingsRequest();
    void ApplySettings(const GameSettings& s);
    void RemoveSurplusPirates();
    int NewPlayerId();
    void RestoreProgress(PlayerState& ps, const PlayerProgress& p);
    PlayerProgress CaptureProgress(const PlayerState& ps) const;

    // --- ein Simulationsschritt -----------------------------------------------------
    void Step();
    void ApplyInputs();
    void DistributeWorldEvents(int stageBefore);
    void SendEvents();
    void SendSnapshots();
    void SendDepositUpdates();
    void Housekeeping();

    // --- Netzwerk -------------------------------------------------------------------
    void ServiceNetwork(uint32_t timeoutMs);
    void GuardHalfOpenPeers();
    void OnConnect(_ENetPeer* peer, uint32_t data);
    void OnReceive(Client& c, const uint8_t* data, size_t len);
    void OnDisconnect(Client& c);
    void HandleSpectate(Client& c);
    void HandleRegister(Client& c, const net::RegisterMsg& m);
    void HandleLoginStart(Client& c, const net::LoginStartMsg& m);
    void HandleLoginProof(Client& c, const net::LoginProofMsg& m);
    void HandleInput(Client& c, const net::InputMsg& m);
    void HandleDockAction(Client& c, const net::DockActionMsg& m);
    void HandleLeave(Client& c);
    void BadPacket(Client& c, const char* what);
    void FailLogin(Client& c, net::ErrorCode code);

    // resumePlayerId >= 0: das noch in der Welt verbliebene Schiff übernehmen (Departing)
    void LoginPlayer(Client& c, Account* account, const std::string& name, int resumePlayerId = -1);
    // allowGrace = false: Schiff sofort entfernen (Server wird beendet)
    void LogoutPlayer(Client& c, const char* reason, bool saveNow, bool allowGrace = true);
    void UpdateDeparting(bool finishAll);
    void FinishDeparting(const Departing& d);
    bool CrashOnLogout(PlayerState& ps);
    void StoreProgress(Client& c);
    void SaveAll();

    void Send(Client& c, const std::vector<uint8_t>& bytes, bool reliable);
    void SendToAllInGame(const std::vector<uint8_t>& bytes);   // Spieler + Zuschauer
    void SendError(Client& c, net::ErrorCode code, const std::string& text = std::string());
    void SendWelcome(Client& c);
    void SendDepositsNow(Client& c);
    void Disconnect(Client& c);
    Client* FindClient(_ENetPeer* peer);
    void RemoveClient(Client& c);

    // --- Nachrichten bauen ------------------------------------------------------------
    net::ServerInfoMsg MakeServerInfo() const;
    net::SnapshotMsg MakeSnapshot(const Client& c) const;
    net::DepositsMsg MakeDeposits(const Client& c) const;
    net::PlayersMsg MakePlayers() const;
    net::HighscoresMsg MakeHighscores() const;

    // --- Zählen und Schutzmaßnahmen ---------------------------------------------------
    int PlayersOnline() const;
    int SpectatorsOnline() const;
    bool LoginBlocked(const std::string& lowerName, uint32_t ip, double now);
    void RecordLoginFailure(const std::string& lowerName, bool accountExists, const Client& c, double now);
    void RememberTrustedIp(const std::string& lowerName, uint32_t ip);
    bool RegistrationAllowed(uint32_t ip, double now);
    void RecordRegistration(uint32_t ip, double now);

    // --- Status für die Webseite ---------------------------------------------------------
    void PublishStatus();

    // --- Benchmark --------------------------------------------------------------------------
    void AddBot(int index);
    void BotThink(Client& c);

    void Log(const std::string& text) const;

    // --- Daten ------------------------------------------------------------------------------
    ServerConfig config_;
    Accounts& accounts_;
    SharedState* shared_ = nullptr;
    GameSettings settings_;             // aktuell gültige Einstellungen
    World world_;
    net::ModelIds models_{};            // Modell-ID -> Variantennummer fürs Netz

    _ENetHost* host_ = nullptr;
    bool enetReady_ = false;
    bool benchmark_ = false;
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<Departing> departing_;
    uint32_t nextConnId_ = 1;
    int nextPlayerId_ = 1;
    uint64_t tick_ = 0;
    bool highscoresChanged_ = false;
    long long savedTotalDelivered_ = -1;
    net::Key32 dummyKey_{};             // für unbekannte Namen (gleicher Rechenweg wie bei echten Konten)

    // Fehlversuche bei der Anmeldung und neue Konten: Zeitpunkte (Now()) je
    // Schlüssel (siehe LoginBlocked). Namen in Kleinbuchstaben, IPs wie ENet.
    std::map<std::pair<std::string, uint32_t>, std::deque<double>> failuresByNameIp_;
    std::map<std::string, std::deque<double>> failuresByName_;
    std::map<uint32_t, std::deque<double>> failuresByIp_;
    std::map<std::string, std::vector<uint32_t>> trustedIps_;   // erfolgreiche Anmeldungen seit Serverstart
    std::map<uint32_t, std::deque<double>> registrations_;
    double lastTableFullLog_ = -1e9;
    uint64_t halfOpenDropped_ = 0;      // verworfene halboffene Verbindungen (Meldung 1x pro Minute)

    // Messwerte für den Status (werden jede Sekunde ausgewertet und geleert)
    std::chrono::steady_clock::time_point startTime_;
    std::vector<double> tickMs_;
    double lastStatusAt_ = 0.0;
    double lastCpuSeconds_ = 0.0;
};
