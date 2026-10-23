#pragma once
// ---------------------------------------------------------------------------
// Netzwerkprotokoll zwischen Spiel (Client) und Server.
//
// Transport: ENet (UDP-basiert, https://github.com/lsalzman/enet). ENet stellt
// eine Verbindung her, erkennt Zeitüberschreitungen und bietet zwei Arten von
// Kanälen, die wir beide nutzen:
//   - CHANNEL_RELIABLE   (zuverlässig, Reihenfolge garantiert): Anmeldung,
//     Ereignisse, Spielerliste, Bestenliste, Lagerstätten, Stationsaktionen.
//   - CHANNEL_UNRELIABLE (unzuverlässig, nur neueste zählt): Eingaben des
//     Spielers und die Momentaufnahmen (Snapshots) der Welt. Geht ein Paket
//     verloren, ist das egal: das nächste kommt 1/15 Sekunde später.
//
// Jede Nachricht beginnt mit einem Byte MsgType, danach folgen die Daten in
// fester Reihenfolge. Zahlen werden "little endian" geschrieben (niedrigstes
// Byte zuerst), unabhängig vom Prozessor. Positionen und Winkel werden
// "quantisiert" (z. B. Position in Zentimetern als 16-Bit-Zahl), damit die
// Pakete klein bleiben.
//
// Sicherheit: Der Server darf keinem Paket trauen. Der Reader prüft deshalb
// bei jedem Lesen, ob noch genug Bytes da sind. Ist ein Paket zu kurz oder
// kaputt, liefert Decode false und der Server verwirft es.
//
// Ablauf einer Sitzung:
//   1. Client verbindet sich (ENet-Verbindungsdaten = PROTOCOL_VERSION).
//   2. Server schickt ServerInfo (Name, Spieler online, Einstellungen, Salz).
//      Solange der Client in der Lobby ist, alle 2 Sekunden erneut.
//   3a. Zuschauer: Client schickt Spectate -> Server antwortet Welcome.
//   3b. Spieler: Client schickt Register (neues Konto) oder LoginStart,
//       Server antwortet LoginChallenge, Client antwortet LoginProof
//       -> Server antwortet Welcome oder Error.
//   4. Im Spiel: Client schickt Input (30/s) und DockAction.
//      Server schickt Snapshot (15/s, Zuschauer 10/s), Events (bei Bedarf),
//      Deposits (wenn sich die sichtbaren Lagerstätten ändern),
//      Players (1/s), Highscores (bei Änderung) und Settings (wenn der
//      Betreiber Einstellungen ohne Neuaufbau der Welt ändert, z. B. Schiffe).
//      Nach jedem Welcome und beim Kartenwechsel: MapData (aktive Karte).
//   Karte hochladen (Karteneditor): wie 3b LoginStart -> LoginChallenge, dann
//   MapUpload statt LoginProof -> Server antwortet MapUploadResult. Das meldet
//   nicht im Spiel an; der Client trennt danach die Verbindung.
// ---------------------------------------------------------------------------
#include "GameMap.hpp"
#include "GameSettings.hpp"
#include "SoundEvents.hpp"
#include "raylib.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace net {

// ---------------------------------------------------------------------------
// Konstanten
// ---------------------------------------------------------------------------
// Version 2: Welcome enthält den Simulationsschritt (tick), siehe WelcomeMsg.
// Version 3: Schiffstypen (SnapMe::shipType/hangar, DockAction BuyShip/SwitchShip).
// Version 4: Upgrade Kanonen (5 Stufenwerte), Leben (SnapMe/PlayerEntry::lives, GameSettings::lives).
// Version 5: Mini-Missionen (SnapMe Mission/Angebote/Markierungen/Funkspruch,
//            DockAction AcceptMission, GameSettings::missions, ShipKind::Convoy).
// Version 6: Schiffstypen vom Server (GameSettings::ships, Nachricht Settings),
//            Kanonen bis 5, Modellvariante craft_racer.
// Version 7: Karten aus dem Karteneditor (MapUpload, MapUploadResult, MapData).
constexpr uint16_t PROTOCOL_VERSION = 7;
constexpr uint16_t DEFAULT_PORT = 7777;

constexpr int TICK_RATE = 30;                    // Simulationsschritte pro Sekunde (Server und Vorhersage)
constexpr float TICK_DT = 1.0f / TICK_RATE;
constexpr int SNAPSHOT_EVERY = 2;                // Spieler: jeder 2. Schritt -> 15 Snapshots/s
constexpr int SPECTATOR_SNAPSHOT_EVERY = 3;      // Zuschauer: 10 Snapshots/s
constexpr float INTERP_DELAY = 0.12f;            // Client zeigt fremde Schiffe 120 ms "in der Vergangenheit"

constexpr float AOI_SHIPS = 110.0f;
constexpr int MAX_HANGAR = 8;                    // höchstens so viele geparkte Schiffe in SnapMe (>= limits::MAX_SHIP_TYPES - 1)
constexpr int MAX_MARKERS = 16;                  // höchstens so viele Missions-Markierungen in SnapMe
constexpr float AOI_BULLETS = 75.0f;             // Sichtweite für Geschosse
constexpr float AOI_EVENTS = 120.0f;             // Hörweite/Sichtweite für Ereignisse

constexpr int CHANNEL_RELIABLE = 0;
constexpr int CHANNEL_UNRELIABLE = 1;
constexpr int CHANNEL_COUNT = 2;

constexpr int MIN_NAME = 3;                      // Spielername: 3..16 Zeichen A-Z a-z 0-9 _ -
constexpr int MAX_NAME = 16;
constexpr int MIN_PASSWORD = 6;                  // prüft nur der Client (der Server sieht das Passwort nie)
constexpr uint32_t KDF_ITERATIONS = 20000;       // PBKDF2-Runden für den Anmeldeschlüssel
constexpr int MAX_INPUTS_PER_MSG = 4;            // Eingaben werden mehrfach geschickt (gegen Paketverlust)

using Key32 = std::array<uint8_t, 32>;
using Bytes16 = std::array<uint8_t, 16>;

// ---------------------------------------------------------------------------
// Nachrichtentypen
// ---------------------------------------------------------------------------
enum class MsgType : uint8_t {
    // Client -> Server
    Spectate = 1,
    Register = 2,
    LoginStart = 3,
    LoginProof = 4,
    Input = 5,
    DockAction = 6,
    Leave = 7,
    MapUpload = 8,
    // Server -> Client
    ServerInfo = 100,
    Error = 101,
    LoginChallenge = 102,
    Welcome = 103,
    Snapshot = 104,
    Deposits = 105,
    Events = 106,
    Players = 107,
    Highscores = 108,
    Settings = 109,
    MapUploadResult = 110,
    MapData = 111,
};

enum class ErrorCode : uint8_t {
    None = 0,
    VersionMismatch = 1,
    ServerFull = 2,
    NameInvalid = 3,
    NameTaken = 4,
    WrongLogin = 5,
    AlreadyOnline = 6,
    TooManyAttempts = 7,
    SpectatorsFull = 8,
    BadRequest = 9,
    ServerShutdown = 10,
};

// Deutsche Beschreibung eines Fehlers (für die Anzeige im Spiel)
const char* ErrorText(ErrorCode c);

// Prüft einen Spielernamen (Länge und erlaubte Zeichen)
bool ValidPlayerName(const std::string& name);

// ---------------------------------------------------------------------------
// Schreiben und Lesen von Bytes
// ---------------------------------------------------------------------------
class Writer {
public:
    void U8(uint8_t v) { buf.push_back(v); }
    void I8(int8_t v) { U8(static_cast<uint8_t>(v)); }
    void U16(uint16_t v);
    void I16(int16_t v) { U16(static_cast<uint16_t>(v)); }
    void U32(uint32_t v);
    void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
    void I64(int64_t v);
    void F32(float v);
    void Str(const std::string& s);   // Länge (1 Byte, max 255) + Bytes
    void Bytes(const uint8_t* data, size_t n);

    std::vector<uint8_t> buf;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : data_(data), len_(len) {}
    uint8_t U8();
    int8_t I8() { return static_cast<int8_t>(U8()); }
    uint16_t U16();
    int16_t I16() { return static_cast<int16_t>(U16()); }
    uint32_t U32();
    int32_t I32() { return static_cast<int32_t>(U32()); }
    int64_t I64();
    float F32();                       // NaN/unendlich werden zu 0
    std::string Str();
    void Bytes(uint8_t* out, size_t n);

    bool ok() const { return ok_; }    // false, sobald einmal über das Ende gelesen wurde
    size_t Remaining() const { return len_ - pos_; }

private:
    bool Need(size_t n);
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// Erstes Byte einer Nachricht (oder 0, wenn leer)
inline uint8_t PeekType(const uint8_t* data, size_t len) { return len > 0 ? data[0] : 0; }

// ---------------------------------------------------------------------------
// Quantisierung: Gleitkommazahlen <-> kleine Ganzzahlen
// ---------------------------------------------------------------------------
int16_t QPos(float v);            // Position in cm (±327 m)
float DPos(int16_t q);
int16_t QVel(float v);            // Geschwindigkeit in cm/s (±327 m/s)
float DVel(int16_t q);
uint16_t QAngle(float rad);       // Winkel 0..2π auf 0..65535
float DAngle(uint16_t q);         // Ergebnis -π..π
uint8_t QUnit(float v);           // 0..1 auf 0..255
float DUnit(uint8_t q);
uint8_t QHeight(float y);         // Höhe 0..12.75 in 5-cm-Schritten
float DHeight(uint8_t q);

// ---------------------------------------------------------------------------
// Client -> Server
// ---------------------------------------------------------------------------
struct RegisterMsg {
    std::string name;
    Key32 key{};                  // PBKDF2(Passwort, Salz); das Passwort selbst wird nie gesendet
};

struct LoginStartMsg {
    std::string name;
};

struct LoginProofMsg {
    Key32 proof{};                // HMAC(Schlüssel, Nonce + Name), siehe Crypto.hpp
};

// Eine Eingabe für einen Simulationsschritt. Tastendrücke (Scanner, Interaktion)
// werden als fortlaufende Zähler übertragen: Geht ein Paket verloren, sieht der
// Server am nächsten Zählerstand trotzdem, dass gedrückt wurde.
struct InputRecord {
    uint32_t seq = 0;             // fortlaufende Nummer
    float thrust = 0.0f;          // -1..1
    float turn = 0.0f;            // -1..1
    bool fire = false;
    uint8_t scanPresses = 0;      // Zähler (läuft bei 255 über)
    uint8_t interactPresses = 0;  // Zähler
};

struct InputMsg {
    std::vector<InputRecord> records;   // älteste zuerst, höchstens MAX_INPUTS_PER_MSG
};

enum class DockActionType : uint8_t { Undock = 0, Repair = 1, BuyUpgrade = 2, BuyShip = 3, SwitchShip = 4, AcceptMission = 5 };

struct DockActionMsg {
    DockActionType action = DockActionType::Undock;
    // BuyUpgrade: Upgrade-Typ (cfg::Upgrade). BuyShip: Schiffstyp (Bit 0-6,
    // cfg::ShipTypeId), Bit 7 = altes Schiff in Zahlung geben. SwitchShip: Schiffstyp.
    // AcceptMission: Missionsart (MissionType).
    uint8_t param = 0;
};

// Karte hochladen (Karteneditor "An Server senden"). Kommt statt LoginProof
// nach LoginChallenge: Der Beweis gilt nur für dieses Hochladen, angemeldet
// wird nicht. Die Karte wird kompakt übertragen (Segment-Nummer statt
// Modellname, Position in cm), höchstens mapgrid::MAX_PIECES Teile = gut
// 54 KB, also unter der Paketgrenze des Servers (64 KB).
struct MapUploadMsg {
    Key32 proof{};                // wie LoginProofMsg::proof
    GameMap map;                  // ohne author (den setzt der Server)
};

// ---------------------------------------------------------------------------
// Server -> Client
// ---------------------------------------------------------------------------
struct ServerInfoMsg {
    uint16_t version = PROTOCOL_VERSION;
    std::string serverName;
    uint8_t playersOnline = 0;
    uint8_t maxPlayers = 0;
    uint8_t spectators = 0;
    uint8_t maxSpectators = 0;
    GameSettings settings;
    Bytes16 realmSalt{};          // zufälliges Salz dieses Servers (für den Anmeldeschlüssel)
    int64_t totalDelivered = 0;
};

struct ErrorMsg {
    ErrorCode code = ErrorCode::None;
    std::string text;             // zusätzlicher Text (optional)
};

struct LoginChallengeMsg {
    Bytes16 nonce{};              // Zufallszahl, nur für diesen Anmeldeversuch gültig
};

enum class Role : uint8_t { Spectator = 1, Player = 2 };

// Antwort auf erfolgreiche Anmeldung/Zuschauen. Auch nach einem Welt-Neuaufbau
// (geänderte Einstellungen) erneut geschickt.
struct WelcomeMsg {
    Role role = Role::Spectator;
    int32_t playerId = -1;        // eigene Spieler-ID (Zuschauer: -1)
    uint32_t worldSeed = 0;       // Mondoberfläche: Client erzeugt sie mit demselben Seed
    GameSettings settings;
    int64_t totalDelivered = 0;
    float serverTime = 0.0f;
    // Simulationsschritt des Servers beim Welcome (wie SnapshotMsg::tick).
    // Der Client rechnet seine Zeitachse aus diesen ganzen Zahlen statt aus
    // serverTime: Ein float wird nach Tagen zu ungenau für die Interpolation,
    // ein Schrittzähler nie (NetClient::Impl::TickTime).
    uint32_t tick = 0;
};

// Eigener Zustand (nur für Spieler) – genau genug für die Vorhersage
struct SnapMe {
    int32_t shipId = -1;          // -1 = gerade kein Schiff (wartet auf neues)
    uint32_t ackSeq = 0;          // letzte vom Server verarbeitete Eingabe
    float x = 0, y = 0, z = 0;
    float vx = 0, vz = 0;
    float yaw = 0, bank = 0;
    float hp = 0, maxHp = 0;
    float hitFlash = 0;
    float thrustVisual = 0;
    bool crashing = false;
    int32_t cargo = 0, cargoMax = 0;
    int64_t balance = 0;
    uint8_t upgrades[cfg::UP_COUNT] = {0, 0, 0, 0, 0};
    uint8_t shipType = 0;         // Typ des geflogenen Schiffs (cfg::ShipTypeId)
    struct Parked {
        uint8_t type = 0;
        uint8_t upgrades[cfg::UP_COUNT] = {0, 0, 0, 0, 0};
    };
    std::vector<Parked> hangar;   // geparkte eigene Schiffe (Tab "Schiff"), höchstens MAX_HANGAR
    float scanEnergy = 0;
    bool scanOn = false;
    float scanPhase = 0;          // Fortschritt des aktuellen Scannerrings in Sekunden (0..PULSE_TIME)
    bool mining = false;
    uint8_t mineKind = 0;
    int32_t mineId = -1;
    float mineT = 0;
    int8_t dockedAt = -1;
    int32_t lastSale = 0;
    float shipTime = 0;           // Zeit mit diesem Schiff
    float roundTime = 0;          // Zeit in der aktuellen Zielrunde
    uint16_t round = 1;
    uint16_t kills = 0;
    float respawnT = 0;
    uint8_t lives = 0;            // verbleibende Leben
    // Mini-Missionen: eigene laufende Mission (-1 = keine) mit Fortschritt,
    // Ziel und (Konvoi) Frachtern unterwegs; Arten der offenen Angebote
    // (höchstens limits::MAX_MISSIONS); Markierungen für die Minikarte
    // (Personen, Ziele, Frachter; auch außerhalb der Sichtweite);
    // Funkspruch (PlayerState::cue, -1 = keiner) mit Restzeit.
    int8_t missionType = -1;
    uint8_t missionProgress = 0, missionGoal = 0, missionAlive = 0;
    std::vector<uint8_t> offers;
    struct Marker {
        uint8_t kind = 0;         // MissionMarker::kind
        float x = 0, z = 0;
    };
    std::vector<Marker> markers;  // höchstens MAX_MARKERS
    int8_t cue = -1;
    float cueT = 0;
};

struct SnapShip {
    uint32_t id = 0;
    uint8_t kind = 0;             // ShipKind
    uint8_t variant = 0;          // Modellvariante (ShipVariant)
    float x = 0, y = 0, z = 0;
    float yaw = 0, bank = 0;
    float hpFrac = 1;
    float thrustVisual = 0;
    float hitFlash = 0;
    bool crashing = false;
    bool hostile = false;         // Händler, der Spieler angreift
    bool mining = false;
    bool docked = false;
    float mineX = 0, mineZ = 0;   // Ziel des Abbaustrahls
    int16_t playerId = -1;        // >= 0: Schiff eines Spielers
};

struct SnapBullet {
    float x = 0, y = 0, z = 0;
    float vx = 0, vz = 0;
    uint8_t type = 0;             // 0 Spieler, 1 Pirat, 2 Händler, 3 Geschützturm
};

struct SnapWreck {
    uint32_t id = 0;
    float x = 0, z = 0;
    float yaw = 0, tilt = 0;
    uint8_t variant = 0;
    uint16_t amount = 0, initial = 0;
    float age = 0;
};

struct SnapshotMsg {
    uint32_t tick = 0;
    float serverTime = 0;
    int64_t totalDelivered = 0;
    uint8_t stationStage = 0;
    bool hasMe = false;
    SnapMe me;
    std::vector<SnapShip> ships;      // ohne das eigene Schiff
    std::vector<SnapBullet> bullets;
    std::vector<SnapWreck> wrecks;
};

struct NetDeposit {
    uint32_t id = 0;
    float x = 0, z = 0;
    float yaw = 0, scale = 1;
    uint8_t variant = 0;
    uint16_t amount = 0, initial = 0;
};

// Alle Lagerstätten, die der Empfänger sehen darf (Spieler: selbst entdeckte,
// Zuschauer: alle). Ersetzt die vorige Liste vollständig.
struct DepositsMsg {
    std::vector<NetDeposit> deposits;
};

enum class EventKind : uint8_t {
    Sound = 0,        // sfx, Position, Lautstärke, Verzögerung, Quelle
    Explosion = 1,    // Position, Größe
    Sparks = 2,       // Position, Farbe, Anzahl
    Notice = 3,       // Text, Farbe (nur für den Empfänger)
    Goal = 4,         // Spieler hat das Ziel erreicht (an alle)
    StationGrow = 5,  // Handelsstationen wachsen (an alle)
    Respawned = 6,    // eigenes neues Schiff
};

struct NetEvent {
    EventKind kind = EventKind::Sound;
    Sfx sfx = Sfx::UiMove;
    float x = 0, y = 0, z = 0;
    float volume = 1, delay = 0;
    int32_t sourceShip = -1;
    float size = 1;
    Color color = WHITE;
    uint8_t count = 0;
    std::string text;             // Notice: Meldung, Goal: Spielername
    int32_t playerId = -1;        // Goal
    float seconds = 0;            // Goal: gebrauchte Zeit
    uint16_t round = 1;           // Goal
    uint8_t stage = 0;            // StationGrow
};

struct EventsMsg {
    std::vector<NetEvent> events;
};

struct PlayerEntry {
    int32_t id = -1;
    std::string name;
    int32_t shipId = -1;
    int64_t balance = 0;
    uint16_t round = 1;
    float shipTime = 0;
    float roundTime = 0;
    uint8_t upgrades[cfg::UP_COUNT] = {0, 0, 0, 0, 0};
    bool alive = false;
    bool docked = false;
    uint16_t kills = 0;
    uint16_t goals = 0;
    uint8_t lives = 0;
};

// Alle angemeldeten Spieler (für Namen über den Schiffen, Top 3 im HUD,
// Liste in der Zuschaueransicht)
struct PlayersMsg {
    int64_t totalDelivered = 0;
    uint8_t spectators = 0;
    uint8_t maxPlayers = 0;
    std::vector<PlayerEntry> players;
};

struct HighscoreEntry {
    std::string name;
    float seconds = 0;            // gebrauchte Zeit bis zum Ziel
    uint16_t round = 1;           // 1 = neues Schiff, >1 = mit behaltenen Aufrüstungen
    int64_t when = 0;             // Unix-Zeit
};

struct HighscoresMsg {
    std::vector<HighscoreEntry> entries;   // sortiert: kürzeste Zeit zuerst
};

// Geänderte Einstellungen ohne Neuaufbau der Welt (Schiffe, Gegnerzahl,
// Leben ...). Der Client übernimmt sie in seine Welt (World::settings).
struct SettingsMsg {
    GameSettings settings;
};

// Antwort auf MapUpload
struct MapUploadResultMsg {
    bool ok = false;
    std::string text;             // Meldung für den Spieler (Erfolg oder Grund der Ablehnung)
};

// Aktive Karte des Servers (auf der Verwaltungsseite gewählt). Nur Kulisse:
// Der Client setzt sie mit World::ApplyMap in seine Spiegelwelt.
// custom = false: Standardkarte (Zufalls-Deko aus dem Seed).
struct MapDataMsg {
    bool custom = false;
    GameMap map;                  // ohne author
};

// ---------------------------------------------------------------------------
// Kodieren (inkl. Typ-Byte) und Dekodieren (Typ-Byte bereits gelesen)
// ---------------------------------------------------------------------------
std::vector<uint8_t> Encode(const RegisterMsg& m);
std::vector<uint8_t> Encode(const LoginStartMsg& m);
std::vector<uint8_t> Encode(const LoginProofMsg& m);
std::vector<uint8_t> Encode(const InputMsg& m);
std::vector<uint8_t> Encode(const DockActionMsg& m);
std::vector<uint8_t> Encode(const MapUploadMsg& m);
std::vector<uint8_t> EncodeSimple(MsgType t);   // Nachrichten ohne Inhalt (Spectate, Leave)
std::vector<uint8_t> Encode(const ServerInfoMsg& m);
std::vector<uint8_t> Encode(const ErrorMsg& m);
std::vector<uint8_t> Encode(const LoginChallengeMsg& m);
std::vector<uint8_t> Encode(const WelcomeMsg& m);
std::vector<uint8_t> Encode(const SnapshotMsg& m);
std::vector<uint8_t> Encode(const DepositsMsg& m);
std::vector<uint8_t> Encode(const EventsMsg& m);
std::vector<uint8_t> Encode(const PlayersMsg& m);
std::vector<uint8_t> Encode(const HighscoresMsg& m);
std::vector<uint8_t> Encode(const SettingsMsg& m);
std::vector<uint8_t> Encode(const MapUploadResultMsg& m);
std::vector<uint8_t> Encode(const MapDataMsg& m);

bool Decode(Reader& r, RegisterMsg& m);
bool Decode(Reader& r, LoginStartMsg& m);
bool Decode(Reader& r, LoginProofMsg& m);
bool Decode(Reader& r, InputMsg& m);
bool Decode(Reader& r, DockActionMsg& m);
bool Decode(Reader& r, MapUploadMsg& m);
bool Decode(Reader& r, ServerInfoMsg& m);
bool Decode(Reader& r, ErrorMsg& m);
bool Decode(Reader& r, LoginChallengeMsg& m);
bool Decode(Reader& r, WelcomeMsg& m);
bool Decode(Reader& r, SnapshotMsg& m);
bool Decode(Reader& r, DepositsMsg& m);
bool Decode(Reader& r, EventsMsg& m);
bool Decode(Reader& r, PlayersMsg& m);
bool Decode(Reader& r, HighscoresMsg& m);
bool Decode(Reader& r, SettingsMsg& m);
bool Decode(Reader& r, MapUploadResultMsg& m);
bool Decode(Reader& r, MapDataMsg& m);

// ---------------------------------------------------------------------------
// Modellvarianten: über das Netz werden kleine Nummern statt Modell-IDs geschickt
// ---------------------------------------------------------------------------
struct ModelIds {
    int player, pirateA, pirateB, elite, miner, cargoA, cargoB;
    int crystalSmall, crystalA, crystalB;
    // Alle Schiffsmodelle in fester Reihenfolge: die sieben oben, danach die
    // übrigen aus ShipModelNames (Spielerschiffe können jedes davon haben).
    std::vector<int> ships;
};
ModelIds DefaultModelIds();                           // dieselben Modelle wie World::ResolveModels
uint8_t ShipVariant(const ModelIds& m, int model);    // Index in m.ships
int ShipModel(const ModelIds& m, uint8_t variant);
uint8_t DepositVariant(const ModelIds& m, int model); // 0..2
int DepositModel(const ModelIds& m, uint8_t variant);

} // namespace net
