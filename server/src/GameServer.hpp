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
//   - Nimmt Karten aus dem Karteneditor an (MapUpload, nur mit Berechtigung)
//     und schickt allen die aktive Karte (MapData).
//   - Event-Editor im Spiel (EventOpen, EventEdit): angemeldete Spieler mit
//     der Rolle Event Manager richten Events ein wie auf der Verwaltungsseite.
//   - SG-Teams (FPS-Modus): Rollenwahl, Teams im Command Center, Anwahl des
//     Stargates; die Figuren rechnet die Welt (shared/Trooper.cpp) wie die
//     Schiffe, der Client sagt seine eigene Figur nur voraus.
//   - Einsatzkarten (Instanzen): Wählt ein Team eine heute wählbare
//     Einsatzkarte als Ziel (freigegeben oder über ein Event, MapEvents.hpp),
//     bekommt es eine eigene Welt (World::NewInstance),
//     die in jedem Schritt mitgerechnet wird (höchstens
//     GameSettings::maxInstances gleichzeitig).
//   - Weltraumkarte der Piloten (shared/Orbit.cpp): Wer auf dem Mond das Ziel
//     (100.000) erreicht, fliegt in einer gemeinsamen zweiten Welt weiter
//     (orbit_), bis alle Leben verbraucht sind. Ohne Spieler wird sie aufgelöst.
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
//     der Warteschlange nehmen -> World::Update (Mondwelt, alle
//     Instanzen und die Weltraumkarte) -> Ereignisse verteilen ->
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

        // --- SG-Team (siehe HandleTeamAction) ---
        bool roleChosen = false;            // Rollenwahl nach der Anmeldung erledigt (bis dahin ist das Schiff versteckt)
        bool eventsOpen = false;            // Event-Editor offen (EventOpen bis zur Rollenwahl), siehe HandleEventOpen
        int sentMissionMaps = -1;           // Stand der Einsatzkarten (missionMapsRev_), den der Client schon hat
        int sentScoreboard = -1;            // Stand des Scoreboards (scoreboardRev_), den der Client schon hat

        // --- Chat (siehe HandleChat) ---
        bool chatting = false;              // Chat-Eingabe offen (Sprechblase am Schiff, ApplyInputs)
        double chatTokens = 0.0;            // Flutbremse: so viele Nachrichten darf er gerade schicken
        double chatTokensAt = 0.0;          // Now() der letzten Berechnung von chatTokens

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
        uint64_t bytesSentMinuteStart = 0;  // bytesSent zu Beginn der Minute (Lastprotokoll: meiste Daten)

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
        bool onFoot = false;        // SG-Team im Einsatz: statt des Schiffs bleibt die Figur stehen
    };

    // SG-Team im Mehrspieler (bis zu cfg::SG_TEAM_MAX Spieler), gebildet im
    // Command Center. Der Gründer wählt das Tor an, sobald alle anderen
    // bereit sind; nach der Anwahl kommt er als Erster durch ein freies
    // Stargate der Mondkarte, die anderen folgen (PlayerState::sgFollowers).
    // Nach dem Einsatz bleibt das Team zusammen.
    struct SgTeam {
        int number = 0;             // SG-Nummer (1 = SG1)
        std::vector<int> members;   // Spieler-IDs, Gründer zuerst
        std::vector<int> ready;     // Spieler-IDs, die "Bereit" gesetzt haben
        float dialT = -1.0f;        // Anwahl: Sekunden seit dem Start, -1 = keine
        std::string target;         // Ziel: "" = Mondkarte, sonst Einsatzkarte (ServerMap::file)
        // Einsatz läuft: Ergebnisse der schon Zurückgekehrten (RecordMissions)
        int homePoints = 0;
        bool homeSuccess = false;   // mindestens einer kam durch das Stargate zurück
        bool homePending = false;   // es gibt Ergebnisse, TeamHome steht noch aus
    };

    // Einsatzkarte eines SG-Teams: eigene Welt (World::NewInstance), nur das
    // Team und KI-Gegner. Die Spieler bleiben in world_ (Konto, Schiff
    // versteckt wie im Command Center); in der Instanz steht eine Kopie mit
    // derselben ID und der Figur. Ist jemand zurück (oder gescheitert),
    // nimmt LeaveInstance sein Ergebnis mit in world_. Ohne Spieler endet die
    // Instanz.
    struct Instance {
        int team = 0;               // SG-Nummer
        std::string file;           // Kartendatei (ServerMap::file)
        std::string name;           // Kartenname (Protokoll)
        std::unique_ptr<World> world;
    };

    // --- Zeit -------------------------------------------------------------------
    double Now() const;                                 // Sekunden seit Serverstart (steady_clock)
    float ClientTime(const Client& c) const;            // serverTime für diesen Client

    // --- Welt -------------------------------------------------------------------
    void InitWorld();
    void RebuildWorld();
    void ApplySettingsRequest();
    void ApplySettings(const GameSettings& s);
    void ApplyShipsRequest();
    void BroadcastSettings();   // geänderte Einstellungen ohne Neuaufbau an alle Clients (net::SettingsMsg)
    void ApplyMapCommands();    // Bereich "Karten" der Webseite: aktivieren, löschen, Berechtigung
    void UpdateMapData();       // mapData_ aus der aktiven Karte bauen
    void MapNotice(const std::string& text, bool error = false);   // Ergebnis für die Webseite (und das Protokoll)
    void ApplyBanCommands();    // Bereich "Sperren" der Webseite: Konto/Adresse sperren, entsperren, trennen
    void BanNotice(const std::string& text, bool error = false);   // wie MapNotice
    void ApplyEventCommands();  // Bereich "Events" der Webseite: Event speichern, löschen
    void EventNotice(const std::string& text, bool error = false); // wie MapNotice
    // Event speichern oder löschen (Verwaltungsseite und Event-Editor im
    // Spiel). by = wer ein neues Event einrichtet. Liefert false mit dem
    // Grund in text, sonst das Ergebnis in text (e.id = Nummer des Events).
    // Danach bekommen alle offenen Event-Editoren außer skip die neue Liste.
    bool ChangeEvent(bool remove, MapEvent& e, const std::string& by, std::string& text, const Client* skip);
    net::EventListMsg MakeEventList() const;     // Einsatzkarten und Events für den Event-Editor
    void SendEventLists(const Client* skip);     // an alle offenen Event-Editoren (außer skip)
    void ApplyRoleCommands();   // Bereich "Rollen" der Webseite: Rolle zuweisen, entziehen
    void RoleNotice(const std::string& text, bool error = false);  // wie MapNotice
    void RemoveSurplusPirates();
    // Aktive Karte auch in der eigenen Welt: Stargates und Hindernisse der
    // Figuren kommen aus der Deko und müssen zu dem passen, was die Clients
    // zeichnen. Laufende Einsätze enden ohne Wertung.
    void ApplyMapToWorld();
    int NewPlayerId();
    void RestoreProgress(PlayerState& ps, const PlayerProgress& p);
    PlayerProgress CaptureProgress(const PlayerState& ps) const;

    // --- ein Simulationsschritt -----------------------------------------------------
    void Step();
    void ApplyInputs();
    void DistributeWorldEvents(int stageBefore);
    void SendEvents();
    void FlushEvents(Client& c);   // gesammelte Ereignisse dieses Clients jetzt schicken
    void SendSnapshots();
    void SendDepositUpdates();
    void Housekeeping();
    // SG-Teams: Anwahl weiterzählen, danach der Einsatz (StartSgMission);
    // Teamliste an alle im Command Center schicken (bei Änderung und 1/s)
    void UpdateTeams();
    void SendTeams();
    // Beendete Einsätze (PlayerState::sgEnded) speichern; ist ein Team ganz
    // zurück, erfahren es alle (EventKind::TeamHome)
    void RecordMissions();
    void EndFpsMode();   // FPS-Modus ausgeschaltet: Einsätze enden ohne Wertung, alle SG-Teams zurück ins Schiff
    SgTeam* TeamOf(int playerId);
    SgTeam* FindTeam(int number);
    bool TeamBusy(const SgTeam& t) const;   // Anwahl läuft oder jemand ist im Einsatz
    void LeaveTeam(int playerId);
    // --- Einsatzkarten (Instanzen) ---
    std::vector<const ServerMap*> MissionMaps() const;   // heute wählbar, Reihenfolge wie net::MissionMapsMsg
    // Heute wählbare Einsatzkarten (missionFiles_) neu bestimmen: freigegebene
    // und die der heutigen Events, ohne die heute gesperrten. Nach jeder
    // Änderung an Karten oder Events und beim Tageswechsel (Housekeeping).
    void UpdateMissionMaps();
    bool MissionAvailable(const std::string& file) const;   // heute im Command Center wählbar?
    void ForgetTarget(const std::string& file);          // Karte gesperrt/gelöscht: Teams zielen wieder auf die Mondkarte
    bool StartInstance(SgTeam& t);                       // false: geht nicht (das Team bekommt eine Meldung)
    // Alle Instanzwelten rechnen, ihre Ereignisse verteilen, Rückkehrer
    // zurück in world_ holen, leere Instanzen beenden
    void UpdateInstances();
    void DistributeInstanceEvents(Instance& in);
    // Spieler verlässt die Instanz: Scoreboard und Ergebnis in world_,
    // notify = InstanceEnd und Lagerstätten der Mondkarte schicken
    void LeaveInstance(Instance& in, int playerId, bool notify);
    void EndAllInstances();                              // Neuaufbau der Welt: alle Einsätze enden ohne Wertung
    Instance* InstanceOf(int playerId);
    const Instance* InstanceOf(int playerId) const;
    const World& WorldOf(int playerId) const;            // Instanzwelt, Weltraumkarte oder world_
    // --- Weltraumkarte der Piloten (orbit_) ---
    // Ein Spieler steht mit Konto und Schiff entweder in world_ (Mond) oder in
    // orbit_, nie in beiden. HomeOf liefert die Welt, in der er steht.
    World* OrbitOf(int playerId);                        // orbit_, wenn der Spieler dort ist, sonst nullptr
    const World* OrbitOf(int playerId) const;
    World& HomeOf(int playerId);
    const World& HomeOf(int playerId) const;
    World& OrbitWorld();                                 // orbit_, bei Bedarf neu angelegt (World::NewOrbit)
    void EnterOrbit(Client& c);                          // Ziel auf dem Mond erreicht oder Kartenwechsel: Spieler wechselt ins All
    void LeaveOrbit(Client& c, bool lost);               // alle Leben verbraucht (lost) oder Kartenwechsel: zurück auf den Mond
    void SendOrbitStart(Client& c);
    // Weltraumkarte rechnen, Ereignisse verteilen, Rückkehrer auf den Mond
    // holen, leere Weltraumkarte auflösen
    void UpdateOrbitWorld();
    void DistributeOrbitEvents();
    // Ziel erreicht (Mond oder All): Bestenliste, Nachricht an alle, speichern
    void AnnounceGoal(const World& w, const GoalEvent& g);

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
    void HandleMapUpload(Client& c, const net::MapUploadMsg& m);
    void HandleEventOpen(Client& c);
    void HandleEventEdit(Client& c, const net::EventEditMsg& m);
    // Prüft den Beweis zur Nonce aus LoginStart (LoginProof, MapUpload). Liefert
    // das Konto oder nullptr; dann ist die Fehlermeldung schon verschickt.
    Account* CheckProof(Client& c, const net::Key32& proof, const char* what);
    void HandleInput(Client& c, const net::InputMsg& m);
    void HandleDockAction(Client& c, const net::DockActionMsg& m);
    void HandleChat(Client& c, const net::ChatMsg& m);
    void HandleChatTyping(Client& c, const net::ChatTypingMsg& m);
    void HandleTeamAction(Client& c, const net::TeamActionMsg& m);
    void HandleLeave(Client& c);
    void BadPacket(Client& c, const char* what);
    void FailLogin(Client& c, net::ErrorCode code, const std::string& text = std::string());

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
    net::TeamsMsg MakeTeams(const PlayerState& ps) const;
    net::ScoreboardMsg MakeScoreboard() const;

    // --- Zählen und Schutzmaßnahmen ---------------------------------------------------
    int PlayersOnline() const;
    int SpectatorsOnline() const;
    bool LoginBlocked(const std::string& lowerName, uint32_t ip, double now);
    void RecordLoginFailure(const std::string& lowerName, bool accountExists, const Client& c, double now);
    void RememberTrustedIp(const std::string& lowerName, uint32_t ip);
    bool RegistrationAllowed(uint32_t ip, double now);
    void RecordRegistration(uint32_t ip, double now);
    // Sperren (Accounts::Bans): gültige Sperre für diese Adresse (wie ENet) bzw.
    // dieses Konto, sonst nullptr. Abgelaufene gelten nicht mehr.
    const Ban* FindIpBan(uint32_t ip) const;
    const Ban* FindAccountBan(const std::string& name) const;
    std::string BanText(const Ban& b) const;   // Grund und Dauer für die Meldung im Spiel
    void ExpireBans();                         // einmal pro Minute: abgelaufene entfernen
    // Spieler oder Zuschauer vom Betreiber entfernen: Schiff sofort aus der
    // Welt (keine 10 s Frist), Meldung schicken, Verbindung trennen.
    void RemoveByOperator(Client& c, net::ErrorCode code, const std::string& text, const char* reason);

    // --- Status für die Webseite ---------------------------------------------------------
    void PublishStatus();
    // Lastprotokoll: die Messwerte jeder Sekunde (aus PublishStatus) zu einer
    // Minute zusammenfassen und je Minute eine Zeile schreiben (Accounts::AppendLoadLog).
    void RecordLoad(const ServerStatus& st);
    void FinishLoadMinute();

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
    std::vector<uint8_t> mapData_;      // aktive Karte als fertige MapData-Nachricht (nach jedem Welcome)
    std::vector<Deco> baseDecos_;       // Zufalls-Deko der Welt (für den Wechsel zurück zur Standardkarte)
    std::vector<SgTeam> teams_;
    bool teamsChanged_ = false;         // Teamliste neu schicken (SendTeams)
    std::vector<Instance> instances_;   // laufende Einsätze auf Einsatzkarten
    std::unique_ptr<World> orbit_;      // Weltraumkarte der Piloten (nullptr, solange niemand dort ist)
    int missionMapsRev_ = 0;            // zählt hoch, wenn sich die heute wählbaren Einsatzkarten ändern
    std::vector<std::string> missionFiles_;   // heute wählbare Einsatzkarten (Dateien), Reihenfolge wie net::MissionMapsMsg
    std::string missionDay_;            // Tag, für den missionFiles_ gilt (JJJJ-MM-TT, Ortszeit)
    std::vector<uint8_t> scoreboard_;   // zuletzt gebautes Scoreboard (fertige Nachricht)
    int scoreboardRev_ = 0;             // zählt hoch, wenn es sich ändert
    std::string mapNotice_;             // für den Status (ServerStatus::mapNotice)
    int mapNoticeSeq_ = 0;
    bool mapNoticeError_ = false;

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
    std::map<uint32_t, double> bannedLogAt_;   // gesperrte Adresse -> letzte Meldung im Log (höchstens 1/min)
    std::string banNotice_;             // für den Status (ServerStatus::banNotice)
    int banNoticeSeq_ = 0;
    bool banNoticeError_ = false;
    std::string eventNotice_;           // für den Status (ServerStatus::eventNotice)
    int eventNoticeSeq_ = 0;
    bool eventNoticeError_ = false;
    std::string roleNotice_;            // für den Status (ServerStatus::roleNotice)
    int roleNoticeSeq_ = 0;
    bool roleNoticeError_ = false;
    uint64_t halfOpenDropped_ = 0;      // verworfene halboffene Verbindungen (Meldung 1x pro Minute)

    // Messwerte für den Status (werden jede Sekunde ausgewertet und geleert)
    std::chrono::steady_clock::time_point startTime_;
    std::vector<double> tickMs_;
    // Lastprotokoll: Summen und Höchstwerte der laufenden Minute
    struct LoadMinute {
        int64_t minute = -1;            // Unix-Zeit / 60
        int samples = 0;                // Anzahl Sekundenwerte
        double tickSum = 0.0, tickMax = 0.0, cpuSum = 0.0, cpuMax = 0.0, memMax = 0.0;
        double inSum = 0.0, inMax = 0.0, outSum = 0.0, outMax = 0.0;
        int players = 0, spectators = 0, connections = 0;   // Höchstwerte
    };
    LoadMinute load_;
    double lastStatusAt_ = 0.0;
    double lastCpuSeconds_ = 0.0;
};
