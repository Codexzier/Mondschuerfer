// ---------------------------------------------------------------------------
// LoadTest.cpp – Lasttest für den Mondschürfer-Server
// (ausführbare Datei "mondschuerfer-loadtest", siehe server/CMakeLists.txt).
//
// Wozu?
//   Der eingebaute Benchmark des Servers (mondschuerfer-server --benchmark N)
//   misst nur die Rechenzeit, ganz ohne Netzwerk. Dieser Lasttest geht den
//   echten Weg: Er startet N simulierte Spieler ("Bots") und M Zuschauer,
//   die sich über UDP/ENet genau wie das Spiel mit einem LAUFENDEN Server
//   verbinden, ein Konto anlegen bzw. sich anmelden, 30-mal pro Sekunde
//   Eingaben schicken und alles empfangen, was der Server verschickt.
//   Gemessen wird, was beim Client ankommt: Snapshots pro Sekunde, Größe und
//   Menge der Nachrichten, Paketlaufzeit (RTT), Verbindungsabbrüche und
//   fehlgeschlagene Anmeldungen. Die Rechenlast des Servers selbst zeigt
//   gleichzeitig seine Verwaltungsseite (/api/status: avgTickMs, cpu).
//
// Benutzung (Beispiele):
//   mondschuerfer-loadtest --bots 32 --seconds 60
//   mondschuerfer-loadtest --server 192.168.1.20:7777 --bots 16 --spectators 2
//   Voraussetzungen am Server:
//     - genug Plätze: MAX_PLAYERS (Standard 16) >= Anzahl der Bots,
//       Zuschauer höchstens limits::MAX_SPECTATORS (16);
//     - steht der Server unter einer öffentlichen Adresse, kommen alle Bots
//       von EINER IP: dann MAX_CONNECTIONS_PER_IP erhöhen bzw. 0 setzen und
//       beachten, dass dort höchstens 10 neue Konten pro Stunde und IP
//       angelegt werden dürfen. Lokale/private Adressen sind davon ausgenommen.
//   Die Bots legen Konten an (Name = Präfix + Nummer, z. B. bot007). Beim
//   nächsten Lauf existieren diese Konten schon: Der Server antwortet dann
//   "NameTaken", und der Bot meldet sich stattdessen mit demselben Passwort an.
//
// Abhängigkeiten:
//   ENet (third_party/enet)    UDP-Verbindungen mit zuverlässigen Kanälen
//   shared/Protocol.hpp/.cpp   Nachrichten kodieren/dekodieren (wie Spiel und Server)
//   shared/Crypto.hpp/.cpp     Anmeldeschlüssel (PBKDF2) und Anmeldebeweis (HMAC)
//   shared/Util.hpp/.cpp       ToLowerAscii, AngleDiff, YawTowards, Fmt
//   Eine Spielwelt (World) braucht der Lasttest nicht: Die Bots steuern nach
//   ihrem eigenen Zustand, den der Server in jedem Snapshot mitschickt (SnapMe).
//
// Threads:
//   Alles läuft im Haupt-Thread in EINER Schleife für alle Bots. Einzige
//   Ausnahme ist der Anmeldeschlüssel: PBKDF2 ist absichtlich langsam (Schutz
//   gegen Passwort-Raten, hier ca. 20 ms je Schlüssel). Im Haupt-Thread würde
//   das die Schleife für ALLE anderen Bots anhalten (keine Eingaben, keine
//   Pakete abgeholt, RTT-Messung verfälscht). Darum rechnet std::async ihn in
//   einem Hilfs-Thread. Der Hilfs-Thread bekommt nur Kopien (Passwort, Name,
//   Salz) und liefert das Ergebnis über ein std::future zurück – es gibt
//   keine gemeinsam benutzten Daten und daher auch keine Sperren (Mutex).
//
// Warum EIN ENetHost für alle Bots statt einem pro Bot?
//   - Ein Host = ein UDP-Socket = eine Schleife mit enet_host_service. Das ist
//     einfach und reicht für Hunderte Verbindungen (ENet erlaubt bis zu 4095
//     Peers pro Host).
//   - Der Server sieht trotzdem N eigenständige Verbindungen: ENet
//     unterscheidet Peers mit derselben Absenderadresse und demselben Port
//     über eine zufällige connectID, und jeder Peer bekommt seine eigenen
//     UDP-Pakete (ENet bündelt nur Befehle für DENSELBEN Peer). Die
//     Netzlast am Server ist also dieselbe wie bei N echten Spielern.
//   - Die Byte-Zähler des Hosts (totalSentData/totalReceivedData) liefern
//     direkt die Summe über alle Bots.
//   Nachteil: Alle Bots teilen sich den Empfangspuffer des einen Sockets.
//   Den vergrößern wir (SOCKET_BUFFER_BYTES), damit bei kurzen Stockungen
//   des Lasttest-Rechners keine Pakete verloren gehen, die dann fälschlich
//   dem Server angelastet würden. Mehrere Hosts brächten keine andere
//   Absender-IP und damit beim Verbindungslimit pro IP keinen Vorteil.
//
// Ablauf:
//   1. Kommandozeile lesen, ENet starten, Host anlegen, Serveradresse auflösen.
//   2. Hauptschleife (bis --seconds abgelaufen, alle Bots beendet oder Strg+C):
//        - "Rampe": alle --ramp ms baut der nächste Bot seine Verbindung auf
//          (ein echter Server bekommt auch nicht alle Anmeldungen gleichzeitig)
//        - je Bot den Zustandsautomaten weiterschalten: Zeitüberschreitungen,
//          fertig berechnete Schlüssel, Eingaben im 30-Hz-Takt, Andocken, Ping
//        - Netzwerk bedienen (enet_host_service): Verbindungen, Nachrichten
//        - Messwerte sammeln, alle 10 s eine kurze Zwischenmeldung ausgeben
//   3. Ende: Jeder Bot meldet sich mit Leave ab und trennt die Verbindung
//      sauber (der Server speichert dabei das Konto); danach Abschlussbericht.
//
// Zustände eines Bots (enum Phase), Protokoll wie in DESIGN Abschnitt 3:
//   Waiting -> Connecting -> Lobby (wartet auf ServerInfo)
//     Spieler:   -> DerivingKey -> Registering --Welcome--> InGame
//                                      \--Error NameTaken--> AwaitChallenge
//                       --LoginChallenge--> AwaitWelcome --Welcome--> InGame
//     Zuschauer: --Spectate--> AwaitWelcome --Welcome--> InGame
//   Abbrüche: Failed (Anmeldung gescheitert, Bot trennt selbst),
//             Lost (Verbindung kam nicht zustande oder ging verloren).
//   Testende: Leaving -> Closed.
//
// Exit-Code: 0 = Test gelaufen und mindestens ein Bot war im Spiel,
//            1 = Server nicht erreichbar / kein Bot angemeldet / ENet-Fehler,
//            2 = ungültige Kommandozeile.
// ---------------------------------------------------------------------------
#include "Config.hpp"
#include "Crypto.hpp"
#include "GameSettings.hpp"
#include "Protocol.hpp"
#include "Util.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <random>
#include <string>
#include <system_error>
#include <vector>

// ENet bindet unter Windows <winsock2.h>/<windows.h> ein. Deren Namen stoßen
// mit raylib.h zusammen (über Protocol.hpp schon eingebunden: Rectangle,
// CloseWindow, DrawText ...). Mit diesen Schaltern lässt Windows die
// betroffenen Teile (GDI, USER) weg; NOMINMAX verhindert die Makros min/max,
// die std::min/std::max kaputt machen würden. (Gleiches Vorgehen wie in
// GameServer.cpp.)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <enet/enet.h>
#if defined(_WIN32)
#include <windows.h>   // SetConsoleOutputCP (Umlaute in der Konsole)
#undef near
#undef far
#endif

namespace {

using Clock = std::chrono::steady_clock;

// ===========================================================================
// 1. Konstanten
// ===========================================================================

// --- Zeitgrenzen (Sekunden) ---
constexpr double CONNECT_TIMEOUT = 5.0;      // so lange wartet auch das Spiel, dann "Server nicht erreichbar"
constexpr double SERVERINFO_TIMEOUT = 10.0;  // der Server schickt ServerInfo sofort nach dem Verbinden
constexpr double LOGIN_TIMEOUT = 15.0;       // Antwort auf Register / LoginStart / LoginProof / Spectate
constexpr double RETRY_DELAY = 3.0;          // "bereits angemeldet": kurz warten, dann erneut versuchen
constexpr int MAX_RETRIES = 6;               // 6 x 3 s > 15 s = längste ENet-Zeitüberschreitung am Server
constexpr double LEAVE_TIMEOUT = 3.0;        // am Ende so lange auf die Bestätigung der Trennungen warten

// --- Takte (Sekunden) ---
constexpr double PING_INTERVAL = 0.5;        // eigene ENet-Pings, damit die RTT aktuell bleibt (siehe UpdateBot)
constexpr double RTT_SAMPLE_INTERVAL = 1.0;  // RTT aller Bots einmal pro Sekunde einsammeln
constexpr double REPORT_INTERVAL = 10.0;     // kurze Zwischenmeldung
constexpr double MAX_INPUT_BACKLOG = 0.25;   // so weit darf der Eingabetakt zurückfallen, dann neu ansetzen
constexpr int MAX_INPUTS_PER_UPDATE = 4;     // höchstens so viele Schritte auf einmal nachholen

// --- Verhalten angedockt (DESIGN 3.5 und 9) ---
constexpr double DOCK_ACTION_DELAY = 0.3;    // nach dem Andocken: reparieren oder aufrüsten ...
constexpr double UNDOCK_DELAY = 1.0;         // ... und nach ~1 s wieder abdocken
constexpr double UNDOCK_REPEAT = 2.0;        // immer noch angedockt? Abdocken noch einmal schicken

// --- Sonstiges ---
constexpr int SOCKET_BUFFER_BYTES = 4 * 1024 * 1024;   // Empfangspuffer des gemeinsamen UDP-Sockets
constexpr int MAX_BOT_LOG_LINES = 40;        // Einzelmeldungen der Bots (danach nur noch zählen)
constexpr int MAX_BOTS = 1000;
constexpr int MAX_SPECTATOR_BOTS = 1000;

// ===========================================================================
// 2. Signale (Strg+C)
//
// Ein Signal-Handler darf fast nichts tun; erlaubt ist das Setzen einer
// sperrfreien atomaren Variable. Die Hauptschleife fragt sie spätestens alle
// paar Millisekunden ab und beendet den Test dann geordnet (Leave + Trennen).
// Ein zweites Strg+C beendet sofort.
// ===========================================================================
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "atomic<bool> muss sperrfrei sein (Signal-Handler)");

extern "C" void OnSignal(int) {
    if (g_stop.exchange(true)) std::_Exit(130);
}

// ===========================================================================
// 3. Kommandozeile
// ===========================================================================
struct Options {
    std::string host = "localhost";
    uint16_t port = net::DEFAULT_PORT;
    int bots = 16;
    int seconds = 60;
    std::string prefix = "bot";
    std::string password = "loadtest-pw";
    int spectators = 0;
    int rampMs = 100;
};

void PrintHelp() {
    std::printf(
        "Mondschürfer-Lasttest: simulierte Spieler (Bots) verbinden sich über das Netzwerk\n"
        "mit einem laufenden Mondschürfer-Server und messen, wie gut er mithält.\n"
        "\n"
        "Aufruf: mondschuerfer-loadtest [Optionen]\n"
        "  --server HOST[:PORT]  Adresse des Servers (Standard localhost:%d, nur IPv4)\n"
        "  --bots N              Anzahl simulierter Spieler (Standard 16, 0..%d)\n"
        "  --spectators M        zusätzliche Zuschauer (Standard 0, 0..%d)\n"
        "  --seconds S           Testdauer in Sekunden inkl. Verbindungsaufbau (Standard 60)\n"
        "  --prefix NAME         Namensanfang der Bots, Nummer wird angehängt (Standard bot -> bot001)\n"
        "  --password PW         Passwort der Bot-Konten (Standard loadtest-pw, mind. %d Zeichen)\n"
        "  --ramp MS             Abstand zwischen zwei Verbindungsaufbauten in ms (Standard 100)\n"
        "  --help                diese Hilfe\n"
        "\n"
        "Hinweise:\n"
        "  - Am Server MAX_PLAYERS mindestens so groß wie --bots setzen (Standard 16).\n"
        "  - Öffentliche Serveradresse: alle Bots kommen von einer IP. Dann am Server\n"
        "    MAX_CONNECTIONS_PER_IP=0 setzen (lokale/private Adressen sind ausgenommen).\n"
        "  - Die Bots legen Konten an. Beim nächsten Lauf melden sie sich mit demselben\n"
        "    Passwort an (\"NameTaken\" beim Anlegen ist dann normal).\n"
        "  - Die Rechenlast des Servers zeigt währenddessen seine Verwaltungsseite.\n",
        static_cast<int>(net::DEFAULT_PORT), MAX_BOTS, MAX_SPECTATOR_BOTS, net::MIN_PASSWORD);
}

bool ParseInt(const std::string& text, int lo, int hi, int& out) {
    int v = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto res = std::from_chars(first, last, v);
    if (res.ec != std::errc() || res.ptr != last || v < lo || v > hi) return false;
    out = v;
    return true;
}

// Anzahl Stellen der Bot-Nummer: mindestens 3 (bot007), mehr bei >= 1000 Bots
int NumberWidth(int bots) {
    int w = 3;
    for (int n = 1000; n <= bots; n *= 10) ++w;
    return w;
}

std::string BotName(const std::string& prefix, int number, int width) {
    std::string digits = std::to_string(number);
    if (static_cast<int>(digits.size()) < width) digits.insert(0, static_cast<size_t>(width) - digits.size(), '0');
    return prefix + digits;
}

enum class ParseResult { Ok, Help, Error };

ParseResult ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") return ParseResult::Help;
        // Alle übrigen Optionen haben genau einen Wert
        if (i + 1 >= argc) {
            std::fprintf(stderr, "Fehler: Unbekannte Option oder fehlender Wert: %s\n", arg.c_str());
            return ParseResult::Error;
        }
        const std::string value = argv[++i];
        bool ok = true;
        if (arg == "--server") {
            // HOST[:PORT]. ENet 1.3 kann nur IPv4; eine IPv6-Adresse hätte mehrere ':'.
            const size_t colon = value.find(':');
            if (colon != std::string::npos && value.find(':', colon + 1) != std::string::npos) {
                std::fprintf(stderr, "Fehler: IPv6-Adressen werden nicht unterstützt (ENet 1.3 kann nur IPv4): %s\n",
                             value.c_str());
                return ParseResult::Error;
            }
            o.host = value.substr(0, colon);
            if (colon != std::string::npos) {
                int port = 0;
                ok = ParseInt(value.substr(colon + 1), 1, 65535, port);
                o.port = static_cast<uint16_t>(port);
            }
            ok = ok && !o.host.empty();
        } else if (arg == "--bots") {
            ok = ParseInt(value, 0, MAX_BOTS, o.bots);
        } else if (arg == "--spectators") {
            ok = ParseInt(value, 0, MAX_SPECTATOR_BOTS, o.spectators);
        } else if (arg == "--seconds") {
            ok = ParseInt(value, 1, 7 * 24 * 3600, o.seconds);
        } else if (arg == "--ramp") {
            ok = ParseInt(value, 0, 60000, o.rampMs);
        } else if (arg == "--prefix") {
            o.prefix = value;
        } else if (arg == "--password") {
            o.password = value;
        } else {
            std::fprintf(stderr, "Fehler: Unbekannte Option: %s (siehe --help)\n", arg.c_str());
            return ParseResult::Error;
        }
        if (!ok) {
            std::fprintf(stderr, "Fehler: Ungültiger Wert für %s: \"%s\" (siehe --help)\n", arg.c_str(), value.c_str());
            return ParseResult::Error;
        }
    }

    if (o.bots + o.spectators == 0) {
        std::fprintf(stderr, "Fehler: Mindestens ein Bot oder Zuschauer ist nötig.\n");
        return ParseResult::Error;
    }
    if (o.bots > 0) {
        // Der Server lehnt ungültige Namen ab (NameInvalid). Lieber gleich hier
        // prüfen – mit dem kürzesten UND dem längsten Namen dieses Laufs.
        const int width = NumberWidth(o.bots);
        const std::string first = BotName(o.prefix, 1, width);
        const std::string last = BotName(o.prefix, o.bots, width);
        if (!net::ValidPlayerName(first) || !net::ValidPlayerName(last)) {
            std::fprintf(stderr,
                         "Fehler: Mit dem Präfix \"%s\" entstehen ungültige Namen (z. B. \"%s\"). Erlaubt sind %d-%d "
                         "Zeichen: A-Z, a-z, 0-9, _ und -.\n",
                         o.prefix.c_str(), last.c_str(), net::MIN_NAME, net::MAX_NAME);
            return ParseResult::Error;
        }
        // Dieselbe Mindestlänge prüft auch das Spiel (der Server sieht das Passwort nie)
        if (static_cast<int>(o.password.size()) < net::MIN_PASSWORD) {
            std::fprintf(stderr, "Fehler: Das Passwort muss mindestens %d Zeichen haben.\n", net::MIN_PASSWORD);
            return ParseResult::Error;
        }
    }
    return ParseResult::Ok;
}

// ===========================================================================
// 4. Messwerte
// ===========================================================================

// Arten empfangener Nachrichten (für die Aufschlüsselung nach Datenmenge)
enum Category : int {
    CAT_SERVERINFO = 0,
    CAT_ERROR,
    CAT_CHALLENGE,
    CAT_WELCOME,
    CAT_SNAPSHOT,
    CAT_DEPOSITS,
    CAT_EVENTS,
    CAT_PLAYERS,
    CAT_HIGHSCORES,
    CAT_UNKNOWN,
    CAT_COUNT
};

const char* const CATEGORY_NAMES[CAT_COUNT] = {
    "ServerInfo", "Error", "LoginChallenge", "Welcome", "Snapshot (Momentaufnahme)", "Deposits (Lagerstätten)",
    "Events (Ereignisse)", "Players (Spielerliste)", "Highscores (Bestenliste)", "unbekannter Typ",
};

Category CategoryOf(uint8_t type) {
    switch (static_cast<net::MsgType>(type)) {
        case net::MsgType::ServerInfo: return CAT_SERVERINFO;
        case net::MsgType::Error: return CAT_ERROR;
        case net::MsgType::LoginChallenge: return CAT_CHALLENGE;
        case net::MsgType::Welcome: return CAT_WELCOME;
        case net::MsgType::Snapshot: return CAT_SNAPSHOT;
        case net::MsgType::Deposits: return CAT_DEPOSITS;
        case net::MsgType::Events: return CAT_EVENTS;
        case net::MsgType::Players: return CAT_PLAYERS;
        case net::MsgType::Highscores: return CAT_HIGHSCORES;
        default: return CAT_UNKNOWN;
    }
}

struct Counter {
    uint64_t count = 0;
    uint64_t bytes = 0;   // Nutzdaten (ohne ENet-/UDP-Kopf)
};

// Messwerte einer Gruppe (Spieler-Bots bzw. Zuschauer-Bots). Es gibt je
// Gruppe zwei Exemplare: "cur" sammelt seit der letzten Zwischenmeldung,
// "total" über den ganzen Test (cur wird nach jeder Meldung hineinaddiert).
struct Stats {
    std::array<Counter, CAT_COUNT> recv{};   // empfangen, nach Art
    uint64_t broken = 0;            // ließ sich nicht dekodieren (zu kurz, unsinnige Werte)
    uint64_t unexpected = 0;        // passte nicht zum Zustand des Bots (z. B. Challenge ohne LoginStart)
    uint64_t snapshotsLost = 0;     // Lücken in der tick-Folge der Snapshots (geschätzt)
    uint64_t snapshotsOld = 0;      // älter als der zuletzt empfangene (vertauscht) -> verworfen
    uint64_t shipsInSnapshots = 0;  // für "Ø Schiffe pro Snapshot"
    uint64_t events = 0;            // einzelne Ereignisse in allen Events-Nachrichten
    uint64_t inputsSent = 0, inputBytes = 0;
    uint64_t dockActions = 0;       // gesendete DockAction (Reparatur/Aufrüsten/Abdocken)
    uint64_t ackLagSum = 0, ackLagSamples = 0;   // unbestätigte Eingaben beim Empfang eines Snapshots
    uint64_t rttSum = 0, rttSamples = 0;
    uint32_t rttMax = 0;
    double clientSeconds = 0.0;     // Summe der Spielzeit aller Bots der Gruppe ("Bot-Sekunden")
};

void AddStats(Stats& into, const Stats& from) {
    for (size_t i = 0; i < into.recv.size(); ++i) {
        into.recv[i].count += from.recv[i].count;
        into.recv[i].bytes += from.recv[i].bytes;
    }
    into.broken += from.broken;
    into.unexpected += from.unexpected;
    into.snapshotsLost += from.snapshotsLost;
    into.snapshotsOld += from.snapshotsOld;
    into.shipsInSnapshots += from.shipsInSnapshots;
    into.events += from.events;
    into.inputsSent += from.inputsSent;
    into.inputBytes += from.inputBytes;
    into.dockActions += from.dockActions;
    into.ackLagSum += from.ackLagSum;
    into.ackLagSamples += from.ackLagSamples;
    into.rttSum += from.rttSum;
    into.rttSamples += from.rttSamples;
    into.rttMax = std::max(into.rttMax, from.rttMax);
    into.clientSeconds += from.clientSeconds;
}

// Lesbare Datenmengen (1 KB = 1024 Byte, wie im Server-Benchmark)
std::string Size(double bytes) {
    if (bytes < 1024.0) return Fmt("%.0f B", bytes);
    if (bytes < 1024.0 * 1024.0) return Fmt("%.1f KB", bytes / 1024.0);
    if (bytes < 1024.0 * 1024.0 * 1024.0) return Fmt("%.2f MB", bytes / (1024.0 * 1024.0));
    return Fmt("%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

std::string Rate(double bytesPerSecond) { return Size(bytesPerSecond) + "/s"; }

double SafeDiv(double a, double b) { return b > 0.0 ? a / b : 0.0; }

const char* ErrorCodeName(net::ErrorCode c) {
    switch (c) {
        case net::ErrorCode::None: return "None";
        case net::ErrorCode::VersionMismatch: return "VersionMismatch";
        case net::ErrorCode::ServerFull: return "ServerFull";
        case net::ErrorCode::NameInvalid: return "NameInvalid";
        case net::ErrorCode::NameTaken: return "NameTaken";
        case net::ErrorCode::WrongLogin: return "WrongLogin";
        case net::ErrorCode::AlreadyOnline: return "AlreadyOnline";
        case net::ErrorCode::TooManyAttempts: return "TooManyAttempts";
        case net::ErrorCode::SpectatorsFull: return "SpectatorsFull";
        case net::ErrorCode::BadRequest: return "BadRequest";
        case net::ErrorCode::ServerShutdown: return "ServerShutdown";
    }
    return "?";
}

// "5 WrongLogin (Name oder Passwort falsch.)" – Nummer, Name im Protokoll, deutscher Text
std::string ErrorLabel(net::ErrorCode c) {
    return Fmt("Fehler %d %s (%s)", static_cast<int>(c), ErrorCodeName(c), net::ErrorText(c));
}

// ===========================================================================
// 5. Ein Bot
// ===========================================================================
enum class Phase {
    Waiting,         // noch nicht gestartet (Rampe)
    Connecting,      // enet_host_connect aufgerufen, warte auf CONNECT
    Lobby,           // verbunden, warte auf ServerInfo
    DerivingKey,     // ServerInfo da, Anmeldeschlüssel wird im Hilfs-Thread berechnet
    Registering,     // Register geschickt, warte auf Welcome oder Error
    AwaitChallenge,  // LoginStart geschickt, warte auf LoginChallenge
    AwaitWelcome,    // LoginProof bzw. Spectate geschickt, warte auf Welcome
    RetryWait,       // "bereits angemeldet": warte kurz, dann neuer Anmeldeversuch
    InGame,          // Welcome erhalten: spielt bzw. schaut zu
    Failed,          // Anmeldung endgültig gescheitert (Bot trennt selbst)
    Lost,            // Verbindung kam nicht zustande oder ging verloren
    Leaving,         // Testende: Leave/Trennung geschickt, warte auf Bestätigung
    Closed,          // sauber getrennt
};

bool InLobbyPhase(Phase p) {
    return p == Phase::Lobby || p == Phase::DerivingKey || p == Phase::Registering || p == Phase::AwaitChallenge ||
           p == Phase::AwaitWelcome || p == Phase::RetryWait;
}

struct Bot {
    int index = 0;
    bool spectator = false;
    std::string name;              // Spieler: Kontoname; Zuschauer: nur für Meldungen
    std::string nameLower;         // für Schlüssel und Anmeldebeweis (Crypto.hpp)
    std::mt19937 rng;              // eigener Zufall je Bot (fester Startwert: Läufe vergleichbar)
    ENetPeer* peer = nullptr;      // nullptr = keine Verbindung (mehr)
    Phase phase = Phase::Waiting;
    double phaseSince = 0.0;       // Beginn der aktuellen Phase (für Zeitüberschreitungen)
    double connectStartedAt = 0.0;
    double connectedAt = 0.0;

    // --- Anmeldung ---
    std::future<crypto::Digest> keyJob;   // läuft im Hilfs-Thread (siehe Dateikopf)
    net::Key32 key{};
    bool viaLogin = false;         // Konto existierte schon (NameTaken) -> Anmeldung per Challenge
    int retries = 0;
    double retryAt = 0.0;
    net::ErrorCode lastError = net::ErrorCode::None;   // zuletzt vom Server gemeldet (für Abbruchgründe)

    // --- Eingaben (wie der Benchmark-Bot im Server, GameServer::BotThink) ---
    uint32_t seq = 0;
    std::deque<net::InputRecord> recent;   // die letzten 4 Eingaben gehen in jedem Paket mit
    double nextInputAt = 0.0;
    float thrust = 1.0f, turn = 0.0f;
    bool fire = false;
    float changeT = 0.0f;          // bis zum nächsten Wechsel von Schub/Drehung/Feuer
    float scanT = 3.0f;            // bis zum nächsten Druck auf "Scanner"
    float interactT = 2.0f;        // bis zum nächsten Druck auf "Interaktion" (Abbau/Andocken)
    uint8_t scanPresses = 0, interactPresses = 0;
    double nextPingAt = 0.0;

    // --- letzter bekannter eigener Zustand (aus dem neuesten Snapshot) ---
    bool anyTick = false;
    uint32_t lastTick = 0;
    bool hasMe = false;
    int32_t shipId = -1;
    float x = 0.0f, z = 0.0f, yaw = 0.0f;
    int dockedAt = -1;

    // --- Andocken ---
    double dockedSince = -1.0;     // < 0: nicht angedockt
    bool dockActionSent = false;
    double undockSentAt = -1.0;
};

// ===========================================================================
// 6. Der Lasttest
// ===========================================================================
class LoadTest {
public:
    explicit LoadTest(const Options& o) : opt_(o), start_(Clock::now()) {}
    ~LoadTest();
    LoadTest(const LoadTest&) = delete;
    LoadTest& operator=(const LoadTest&) = delete;

    int Run();

private:
    // Vorbereitung
    bool Init();
    double Now() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }

    // Zustände der Bots
    void SetPhase(Bot& b, Phase p) {
        b.phase = p;
        b.phaseSince = Now();
    }
    void StartDueBots(double now);
    void Connect(Bot& b);
    void UpdateBot(Bot& b, double now);
    void UpdatePlayer(Bot& b, double now);
    void SendInput(Bot& b);
    void SendRegister(Bot& b);
    void StartLogin(Bot& b);
    void LoginError(Bot& b, net::ErrorCode code);
    void Fail(Bot& b, const std::string& reason);
    void ResetPeer(Bot& b);
    bool SendTo(Bot& b, const std::vector<uint8_t>& bytes, bool reliable);

    // Netzwerk
    void Service(uint32_t timeoutMs);
    void HandleEvent(ENetEvent& ev);
    void OnConnect(Bot& b);
    void OnDisconnect(Bot& b);
    void OnReceive(Bot& b, const uint8_t* data, size_t len);
    void OnServerInfo(Bot& b, const net::ServerInfoMsg& m);
    void OnError(Bot& b, const net::ErrorMsg& m);
    void OnChallenge(Bot& b, const net::LoginChallengeMsg& m);
    void OnWelcome(Bot& b, const net::WelcomeMsg& m);
    void OnSnapshot(Bot& b, const net::SnapshotMsg& m);
    uint32_t NextTimeoutMs(double now) const;

    // Messen und Ausgeben
    Stats& Cur(const Bot& b) { return b.spectator ? curSpectators_ : curPlayers_; }
    void AccumulateClientTime(double dt);
    void SampleRtt();
    void HarvestEnetTotals();
    void PrintInterval(double now);
    void PrintFinal(const std::string& endReason);
    void LogBot(const Bot& b, const std::string& text);
    void Log(const std::string& text) const;
    void LeaveAll();
    bool AllFinished() const;
    int CountPhase(bool spectator, Phase p) const;
    int CountConnected() const;

    Options opt_;
    Clock::time_point start_;
    ENetHost* host_ = nullptr;
    bool enetReady_ = false;
    ENetAddress address_{};
    std::string addressText_;
    std::vector<Bot> bots_;        // feste Größe: peer->data zeigt auf Elemente, darf nie umziehen
    size_t nextBot_ = 0;           // nächster Bot, der verbinden soll (Rampe)

    // Erste ServerInfo (für Hinweise wie "Server zu klein")
    bool haveInfo_ = false;
    net::ServerInfoMsg info_;

    // Messwerte
    Stats curPlayers_, curSpectators_, totalPlayers_, totalSpectators_;
    uint64_t bytesIn_ = 0, bytesOut_ = 0, packetsIn_ = 0, packetsOut_ = 0;   // ENet-Summen (64 Bit)
    uint64_t lastReportBytesIn_ = 0, lastReportBytesOut_ = 0;
    double lastReportAt_ = 0.0;
    double measureEnd_ = 0.0;
    bool anyConnected_ = false;
    int connectFailures_ = 0;      // Verbindung kam nicht zustande
    int connectedEver_ = 0;
    int registered_ = 0;           // neues Konto angelegt (und direkt angemeldet)
    int loggedInViaLogin_ = 0;     // Konto existierte, per Challenge angemeldet
    int spectating_ = 0;           // Zuschauer angenommen
    int retriesTotal_ = 0;
    int rewelcomes_ = 0;           // erneutes Welcome im Spiel (Welt neu aufgebaut)
    int docks_ = 0, crashes_ = 0;
    uint64_t inputSkips_ = 0;      // Eingabeschritte, die der Lasttest selbst nicht rechtzeitig geschafft hat
    double connectTimeSum_ = 0.0, connectTimeMax_ = 0.0;
    int connectTimeCount_ = 0;
    double loginTimeSum_ = 0.0, loginTimeMax_ = 0.0;
    int loginTimeCount_ = 0;
    std::map<std::string, int> failures_;   // Grund -> Anzahl (Anmeldung gescheitert)
    std::map<std::string, int> losses_;     // Grund -> Anzahl (im Spiel getrennt)
    std::map<int, int> errorCodes_;         // alle empfangenen Error-Nachrichten nach Code
    int lostTotal_ = 0;
    int leaveSent_ = 0, closedClean_ = 0, closedForced_ = 0;
    int botLogLines_ = 0;
};

LoadTest::~LoadTest() {
    // Laufende Schlüsselberechnungen abwarten: Der Destruktor eines
    // std::async-future wartet ohnehin; hier passiert es geordnet VOR dem
    // Abbau von ENet.
    for (Bot& b : bots_)
        if (b.keyJob.valid()) b.keyJob.wait();
    if (host_) enet_host_destroy(host_);
    if (enetReady_) enet_deinitialize();
}

void LoadTest::Log(const std::string& text) const {
    std::printf("[%7.1f s] %s\n", Now(), text.c_str());
    std::fflush(stdout);
}

// Meldungen einzelner Bots begrenzen: Bei 500 Bots, die alle "Server voll"
// bekommen, soll die Ausgabe lesbar bleiben. Gezählt wird trotzdem alles.
void LoadTest::LogBot(const Bot& b, const std::string& text) {
    if (botLogLines_ < MAX_BOT_LOG_LINES) Log(b.name + ": " + text);
    else if (botLogLines_ == MAX_BOT_LOG_LINES) Log("(weitere Einzelmeldungen der Bots werden nicht mehr angezeigt, aber gezählt)");
    ++botLogLines_;
}

// ---------------------------------------------------------------------------
// Vorbereitung: ENet, Host, Adresse, Bots
// ---------------------------------------------------------------------------
bool LoadTest::Init() {
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "Fehler: ENet konnte nicht gestartet werden.\n");
        return false;
    }
    enetReady_ = true;

    // Adresse auflösen (DNS-Name oder IPv4). Ohne gültige Adresse ist jeder
    // Verbindungsversuch sinnlos -> sofort mit klarer Meldung aufhören.
    if (enet_address_set_host(&address_, opt_.host.c_str()) != 0) {
        std::fprintf(stderr, "Fehler: Die Serveradresse \"%s\" lässt sich nicht auflösen (Tippfehler? DNS?).\n",
                     opt_.host.c_str());
        return false;
    }
    address_.port = opt_.port;
    char ip[64] = {0};
    if (enet_address_get_host_ip(&address_, ip, sizeof(ip)) != 0) std::snprintf(ip, sizeof(ip), "?");
    addressText_ = Fmt("%s:%d (%s)", opt_.host.c_str(), static_cast<int>(opt_.port), ip);

    // Ein Host als Client (kein eigener fester Port, daher nullptr) mit einem
    // Peer pro Bot; 0/0 = keine eigene Bandbreitenbegrenzung durch ENet.
    const size_t peers = static_cast<size_t>(opt_.bots + opt_.spectators);
    host_ = enet_host_create(nullptr, peers, net::CHANNEL_COUNT, 0, 0);
    if (!host_) {
        std::fprintf(stderr, "Fehler: ENet-Host für %d Verbindungen konnte nicht angelegt werden.\n",
                     static_cast<int>(peers));
        return false;
    }
    // Größere Socket-Puffer (siehe Dateikopf). Das Betriebssystem darf den
    // Wert begrenzen (Linux: net.core.rmem_max); ein Fehler ist nicht schlimm.
    enet_socket_set_option(host_->socket, ENET_SOCKOPT_RCVBUF, SOCKET_BUFFER_BYTES);
    enet_socket_set_option(host_->socket, ENET_SOCKOPT_SNDBUF, SOCKET_BUFFER_BYTES);

    // Bots anlegen: zuerst die Spieler, dann die Zuschauer. Die Größe des
    // Vektors ändert sich danach nie mehr (Zeiger in peer->data!).
    bots_.resize(peers);
    const int width = NumberWidth(opt_.bots);
    for (size_t i = 0; i < bots_.size(); ++i) {
        Bot& b = bots_[i];
        b.index = static_cast<int>(i);
        b.spectator = static_cast<int>(i) >= opt_.bots;
        b.name = b.spectator ? Fmt("Zuschauer%02d", static_cast<int>(i) - opt_.bots + 1)
                             : BotName(opt_.prefix, static_cast<int>(i) + 1, width);
        b.nameLower = ToLowerAscii(b.name);
        b.rng.seed(static_cast<std::mt19937::result_type>(0x4D6F6E64u + 7919u * i));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rampe und Verbindungsaufbau
// ---------------------------------------------------------------------------
void LoadTest::StartDueBots(double now) {
    const double ramp = opt_.rampMs / 1000.0;
    while (nextBot_ < bots_.size() && now >= static_cast<double>(nextBot_) * ramp) Connect(bots_[nextBot_++]);
}

void LoadTest::Connect(Bot& b) {
    // Die Protokollversion geht als "Verbindungsdaten" mit (DESIGN 3.1). Ein
    // Server mit anderer Version antwortet mit Error{VersionMismatch}.
    ENetPeer* peer = enet_host_connect(host_, &address_, net::CHANNEL_COUNT, net::PROTOCOL_VERSION);
    b.connectStartedAt = Now();
    if (!peer) {
        // Kann nur passieren, wenn kein Peer-Platz frei ist (der Host hat genau
        // einen pro Bot) – also eigentlich nie.
        ++connectFailures_;
        SetPhase(b, Phase::Lost);
        LogBot(b, "kein freier ENet-Verbindungsplatz");
        return;
    }
    peer->data = &b;   // so findet HandleEvent zu jedem ENet-Ereignis den Bot
    b.peer = peer;
    // Zeitüberschreitungen wie beim Spiel (DESIGN 3.9). Für den
    // Verbindungsaufbau selbst gilt zusätzlich CONNECT_TIMEOUT (UpdateBot).
    enet_peer_timeout(peer, 32, 5000, 15000);
    SetPhase(b, Phase::Connecting);
}

// Verbindung lokal sofort verwerfen (ohne Abmeldung beim Server)
void LoadTest::ResetPeer(Bot& b) {
    if (!b.peer) return;
    b.peer->data = nullptr;
    enet_peer_reset(b.peer);
    b.peer = nullptr;
}

bool LoadTest::SendTo(Bot& b, const std::vector<uint8_t>& bytes, bool reliable) {
    if (!b.peer || bytes.empty()) return false;
    // Eingaben unzuverlässig (eine verlorene ist egal, die nächste kommt
    // 33 ms später und enthält die letzten 4 ohnehin noch einmal), alles
    // andere zuverlässig – genau wie das Spiel (NetClient).
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!packet) return false;
    const auto channel = static_cast<enet_uint8>(reliable ? net::CHANNEL_RELIABLE : net::CHANNEL_UNRELIABLE);
    // Bei Erfolg gehört das Paket ENet; bei einem Fehler müssen wir es freigeben.
    if (enet_peer_send(b.peer, channel, packet) < 0) {
        if (packet->referenceCount == 0) enet_packet_destroy(packet);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Zustandsautomat je Bot (wird in jedem Schleifendurchlauf aufgerufen)
// ---------------------------------------------------------------------------
void LoadTest::UpdateBot(Bot& b, double now) {
    switch (b.phase) {
        case Phase::Connecting:
            // ENet selbst würde erst nach bis zu ~30 s aufgeben. Das Spiel meldet
            // nach 5 s "Server nicht erreichbar" – der Lasttest auch.
            if (now - b.phaseSince > CONNECT_TIMEOUT) {
                ResetPeer(b);
                ++connectFailures_;
                SetPhase(b, Phase::Lost);
                LogBot(b, Fmt("keine Antwort vom Server innerhalb von %.0f s", CONNECT_TIMEOUT));
            }
            break;
        case Phase::Lobby:
            if (now - b.phaseSince > SERVERINFO_TIMEOUT) Fail(b, "keine ServerInfo vom Server erhalten");
            break;
        case Phase::DerivingKey:
            // Fertig gerechnet? wait_for(0) blockiert nicht, es fragt nur nach.
            if (b.keyJob.valid() && b.keyJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const crypto::Digest k = b.keyJob.get();
                std::copy(k.begin(), k.end(), b.key.begin());
                SendRegister(b);
            }
            break;
        case Phase::Registering:
        case Phase::AwaitChallenge:
        case Phase::AwaitWelcome:
            if (now - b.phaseSince > LOGIN_TIMEOUT) Fail(b, "Zeitüberschreitung bei der Anmeldung (keine Antwort)");
            break;
        case Phase::RetryWait:
            if (now >= b.retryAt) StartLogin(b);
            break;
        case Phase::InGame:
            if (!b.spectator) UpdatePlayer(b, now);
            // ENet misst die RTT nur an Bestätigungen ZUVERLÄSSIGER Pakete und
            // pingt von sich aus nur, wenn eine Weile nichts ankam. Im Spiel
            // kommen aber ständig Snapshots, und die Bots senden selbst fast
            // nur unzuverlässige Eingaben -> peer->roundTripTime würde auf dem
            // Wert der Anmeldung stehen bleiben. Ein eigener Ping alle 0,5 s
            // (wenige Bytes, wird vom Server-ENet automatisch bestätigt) hält
            // die Messung aktuell.
            if (now >= b.nextPingAt && b.peer) {
                enet_peer_ping(b.peer);
                b.nextPingAt = now + PING_INTERVAL;
            }
            break;
        default:
            break;
    }
}

// Spieler im Spiel: Eingaben im 30-Hz-Takt und Verhalten an der Station
void LoadTest::UpdatePlayer(Bot& b, double now) {
    // --- Eingaben: jeder Bot hat seinen eigenen Takt (nextInputAt). Weil die
    // Bots nacheinander ins Spiel kommen, sind die Takte gegeneinander
    // versetzt – wie bei echten Spielern, nicht alle im selben Moment.
    if (now - b.nextInputAt > MAX_INPUT_BACKLOG) {
        // Der Lasttest-Rechner kam nicht hinterher (überlastet?). Nicht alle
        // verpassten Schritte auf einmal nachschicken, sondern neu ansetzen
        // und zählen: Viele ausgelassene Schritte verfälschen die Messung.
        inputSkips_ += static_cast<uint64_t>((now - b.nextInputAt) / net::TICK_DT);
        b.nextInputAt = now;
    }
    for (int n = 0; n < MAX_INPUTS_PER_UPDATE && now >= b.nextInputAt; ++n) {
        SendInput(b);
        b.nextInputAt += net::TICK_DT;
    }

    // --- Angedockt (me.dockedAt >= 0 im letzten Snapshot): wie ein Spieler im
    // Stationsfenster erst reparieren oder aufrüsten, nach ~1 s abdocken.
    // Welche Station welche Art ist, weiß der Bot ohne Spielwelt nicht; er
    // wählt zufällig. Passt die Aktion nicht zur Station (Reparatur nur an
    // Handelsstationen, Aufrüsten nur an Werften), ignoriert der Server sie –
    // auch das ist ein Pfad, den ein Lasttest durchlaufen soll.
    if (b.dockedAt >= 0) {
        if (b.dockedSince < 0.0) {
            b.dockedSince = now;
            b.dockActionSent = false;
            b.undockSentAt = -1.0;
            ++docks_;
        }
        if (!b.dockActionSent && now - b.dockedSince >= DOCK_ACTION_DELAY) {
            net::DockActionMsg a;
            const bool repair = std::uniform_int_distribution<int>(0, 1)(b.rng) == 0;
            a.action = repair ? net::DockActionType::Repair : net::DockActionType::BuyUpgrade;
            a.param = static_cast<uint8_t>(std::uniform_int_distribution<int>(0, cfg::UP_COUNT - 1)(b.rng));
            if (SendTo(b, net::Encode(a), true)) ++Cur(b).dockActions;
            b.dockActionSent = true;
        }
        // Abdocken NUR als DockAction, nicht über den Interaktions-Zähler
        // (DESIGN 9). Der Snapshot, der "abgedockt" zeigt, kommt erst eine
        // Paketlaufzeit später – bis dahin nicht ständig erneut senden.
        if (now - b.dockedSince >= UNDOCK_DELAY && (b.undockSentAt < 0.0 || now - b.undockSentAt >= UNDOCK_REPEAT)) {
            net::DockActionMsg a;
            a.action = net::DockActionType::Undock;
            if (SendTo(b, net::Encode(a), true)) ++Cur(b).dockActions;
            b.undockSentAt = now;
        }
    } else {
        b.dockedSince = -1.0;
    }
}

// Eine Eingabe für einen Simulationsschritt erzeugen und senden. Das
// Verhalten entspricht dem Benchmark-Bot des Servers (GameServer::BotThink):
// meist Vollgas, zufällig drehen, oft feuern, alle paar Sekunden Scanner und
// Interaktion (Abbau bzw. Andocken in der Nähe einer Station).
void LoadTest::SendInput(Bot& b) {
    const float dt = net::TICK_DT;
    auto rnd = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(b.rng); };

    b.changeT -= dt;
    if (b.changeT <= 0.0f) {
        const float r = rnd(0.0f, 1.0f);
        b.thrust = r < 0.75f ? 1.0f : (r < 0.9f ? 0.0f : -0.5f);
        b.turn = rnd(0.0f, 1.0f) < 0.35f ? 0.0f : rnd(-1.0f, 1.0f);
        b.fire = rnd(0.0f, 1.0f) < 0.6f;
        b.changeT = rnd(0.4f, 2.5f);
    }
    float turn = b.turn;
    // Nicht am Kartenrand kleben bleiben: zur Mitte zurückdrehen. Position und
    // Drehwinkel stammen aus dem letzten Snapshot (höchstens ~70 ms alt).
    if (b.hasMe && b.shipId >= 0) {
        const float edge = cfg::WORLD_HALF - 25.0f;
        if (std::fabs(b.x) > edge || std::fabs(b.z) > edge) {
            const float want = YawTowards(Vector3{b.x, 0.0f, b.z}, Vector3{0.0f, 0.0f, 0.0f});
            turn = std::clamp(AngleDiff(b.yaw, want) * 2.0f, -1.0f, 1.0f);
        }
    }
    // Tastendrücke als fortlaufende Zähler (DESIGN 3.5). Der Server wertet
    // die erste Eingabe nur als Ausgangsstand – Beginn bei 0 ist richtig.
    b.scanT -= dt;
    if (b.scanT <= 0.0f) {
        ++b.scanPresses;
        b.scanT = rnd(3.0f, 9.0f);
    }
    // Angedockt keine Interaktion drücken: Abdocken geht nur per DockAction.
    if (b.dockedAt < 0) {
        b.interactT -= dt;
        if (b.interactT <= 0.0f) {
            ++b.interactPresses;
            b.interactT = rnd(1.5f, 5.0f);
        }
    }

    net::InputRecord rec;
    rec.seq = ++b.seq;
    rec.thrust = b.thrust;
    rec.turn = turn;
    rec.fire = b.fire;
    rec.scanPresses = b.scanPresses;
    rec.interactPresses = b.interactPresses;
    // Die letzten bis zu 4 Eingaben mitschicken (älteste zuerst, seq
    // aufsteigend): Geht ein Paket verloren, hat der Server die Eingabe mit
    // dem nächsten Paket trotzdem.
    b.recent.push_back(rec);
    while (b.recent.size() > static_cast<size_t>(net::MAX_INPUTS_PER_MSG)) b.recent.pop_front();
    net::InputMsg msg;
    msg.records.assign(b.recent.begin(), b.recent.end());
    const std::vector<uint8_t> bytes = net::Encode(msg);
    if (SendTo(b, bytes, false)) {
        Stats& s = Cur(b);
        ++s.inputsSent;
        s.inputBytes += bytes.size();
    }
}

// Konto anlegen (DESIGN 3.3). Gibt es den Namen schon, antwortet der Server
// mit NameTaken und der Bot meldet sich an (OnError -> StartLogin).
void LoadTest::SendRegister(Bot& b) {
    net::RegisterMsg m;
    m.name = b.name;
    m.key = b.key;
    SendTo(b, net::Encode(m), true);
    SetPhase(b, Phase::Registering);
}

// Anmelden (DESIGN 3.4): Name schicken, auf die Herausforderung warten
void LoadTest::StartLogin(Bot& b) {
    net::LoginStartMsg m;
    m.name = b.name;
    b.viaLogin = true;
    SendTo(b, net::Encode(m), true);
    SetPhase(b, Phase::AwaitChallenge);
}

// Anmeldung endgültig gescheitert: zählen, Verbindung selbst beenden
void LoadTest::Fail(Bot& b, const std::string& reason) {
    ++failures_[reason];
    LogBot(b, "Anmeldung fehlgeschlagen – " + reason);
    if (b.peer) enet_peer_disconnect(b.peer, 0);   // DISCONNECT-Ereignis folgt (OnDisconnect)
    SetPhase(b, Phase::Failed);
}

void LoadTest::LoginError(Bot& b, net::ErrorCode code) {
    // "Bereits angemeldet": typisch, wenn ein vorheriger Lasttest gerade erst
    // beendet (oder abgestürzt) ist und der Server die alte Verbindung noch
    // nicht als abgelaufen erkannt hat (bis zu 15 s). Dann lohnt ein neuer
    // Versuch. Der Server zählt das nicht als Fehlversuch.
    if (code == net::ErrorCode::AlreadyOnline && b.retries < MAX_RETRIES) {
        ++b.retries;
        ++retriesTotal_;
        b.retryAt = Now() + RETRY_DELAY;
        SetPhase(b, Phase::RetryWait);
        if (b.retries == 1) LogBot(b, Fmt("noch angemeldet (alte Verbindung?) – neuer Versuch alle %.0f s", RETRY_DELAY));
        return;
    }
    Fail(b, ErrorLabel(code));
}

// ---------------------------------------------------------------------------
// Netzwerk
// ---------------------------------------------------------------------------
void LoadTest::Service(uint32_t timeoutMs) {
    ENetEvent ev;
    // Wartet höchstens timeoutMs auf das erste Ereignis; schon angekommene
    // weitere holt enet_host_check_events ohne erneutes Warten ab.
    int r = enet_host_service(host_, &ev, timeoutMs);
    if (r < 0) {
        // Socket-Fehler (selten, z. B. Netzwerk weg). Nicht in einer Schleife
        // mit voller CPU-Last immer wieder versuchen, höchstens 1x/s melden.
        static double lastReport = -1e9;
        if (Now() - lastReport > 1.0) {
            Log("ENet-Fehler beim Empfangen (Netzwerk gestört?)");
            lastReport = Now();
        }
        enet_time_set(enet_time_get());   // nur Zeit verstreichen lassen (kein Schlaf nötig, s. u.)
        return;
    }
    int handled = 0;
    while (r > 0) {
        HandleEvent(ev);
        // Bei einer Paketflut nicht ewig hier hängen: die Eingaben haben Vorrang
        if (++handled >= 10000) break;
        r = enet_host_check_events(host_, &ev);
    }
}

void LoadTest::HandleEvent(ENetEvent& ev) {
    Bot* b = ev.peer ? static_cast<Bot*>(ev.peer->data) : nullptr;
    // ENet verwendet freie Peer-Plätze wieder. Gehört der Peer inzwischen
    // einem anderen Bot (oder keinem), ist das Ereignis veraltet.
    if (b && b->peer != ev.peer) b = nullptr;
    switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (b) OnConnect(*b);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (b) {
                // Ein kaputtes Paket darf den Lasttest nicht beenden. Protocol.cpp
                // prüft alle Längen; die Ausnahme fängt nur Unvorhergesehenes ab
                // (z. B. kein Speicher für eine riesige Liste).
                try {
                    OnReceive(*b, ev.packet->data, ev.packet->dataLength);
                } catch (const std::exception&) {
                    ++Cur(*b).broken;
                }
            }
            enet_packet_destroy(ev.packet);   // Paket gehört uns: immer freigeben
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            ev.peer->data = nullptr;
            if (b) OnDisconnect(*b);
            break;
        default:
            break;
    }
}

void LoadTest::OnConnect(Bot& b) {
    if (b.phase != Phase::Connecting) return;
    const double now = Now();
    b.connectedAt = now;
    anyConnected_ = true;
    ++connectedEver_;
    const double t = now - b.connectStartedAt;
    connectTimeSum_ += t;
    connectTimeMax_ = std::max(connectTimeMax_, t);
    ++connectTimeCount_;
    SetPhase(b, Phase::Lobby);   // jetzt kommt ServerInfo (OnServerInfo)
}

void LoadTest::OnDisconnect(Bot& b) {
    b.peer = nullptr;   // ENet gibt den Peer nach diesem Ereignis selbst frei
    switch (b.phase) {
        case Phase::Connecting:
            // Server hat den Aufbau abgelehnt oder ENet hat aufgegeben
            ++connectFailures_;
            SetPhase(b, Phase::Lost);
            LogBot(b, "Verbindungsaufbau fehlgeschlagen");
            break;
        case Phase::Failed:
            break;   // wir hatten selbst getrennt
        case Phase::Leaving:
            ++closedClean_;
            SetPhase(b, Phase::Closed);
            break;
        case Phase::InGame: {
            // Unerwartet verloren. Hat der Server vorher einen Grund genannt
            // (z. B. "Server wird beendet"), diesen nehmen.
            const std::string reason = b.lastError != net::ErrorCode::None ? ErrorLabel(b.lastError)
                                                                           : std::string("Verbindung verloren");
            ++losses_[reason];
            ++lostTotal_;
            LogBot(b, "im Spiel getrennt – " + reason);
            SetPhase(b, Phase::Lost);
            break;
        }
        default:
            if (InLobbyPhase(b.phase)) {
                const std::string reason = b.lastError != net::ErrorCode::None
                                               ? ErrorLabel(b.lastError)
                                               : std::string("Verbindung während der Anmeldung getrennt");
                ++failures_[reason];
                LogBot(b, "Anmeldung fehlgeschlagen – " + reason);
                SetPhase(b, Phase::Lost);
            }
            break;
    }
}

void LoadTest::OnReceive(Bot& b, const uint8_t* data, size_t len) {
    Stats& s = Cur(b);
    if (len == 0) {
        ++s.broken;
        return;
    }
    const uint8_t type = data[0];
    const Category cat = CategoryOf(type);
    ++s.recv[cat].count;
    s.recv[cat].bytes += len;

    // Jede Nachricht vollständig dekodieren: So prüft der Lasttest nebenbei,
    // dass alles, was der Server unter Last verschickt, gültig ist.
    net::Reader r(data, len);
    r.U8();   // Typ-Byte (schon ausgewertet)
    switch (static_cast<net::MsgType>(type)) {
        case net::MsgType::ServerInfo: {
            net::ServerInfoMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            else OnServerInfo(b, m);
            break;
        }
        case net::MsgType::Error: {
            net::ErrorMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            else OnError(b, m);
            break;
        }
        case net::MsgType::LoginChallenge: {
            net::LoginChallengeMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            else OnChallenge(b, m);
            break;
        }
        case net::MsgType::Welcome: {
            net::WelcomeMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            else OnWelcome(b, m);
            break;
        }
        case net::MsgType::Snapshot: {
            net::SnapshotMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            else OnSnapshot(b, m);
            break;
        }
        case net::MsgType::Deposits: {
            net::DepositsMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            break;
        }
        case net::MsgType::Events: {
            net::EventsMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            else s.events += m.events.size();
            break;
        }
        case net::MsgType::Players: {
            net::PlayersMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            break;
        }
        case net::MsgType::Highscores: {
            net::HighscoresMsg m;
            if (!net::Decode(r, m)) ++s.broken;
            break;
        }
        default:
            // Unbekannter Typ (oder ein Client->Server-Typ): nur zählen (CAT_UNKNOWN)
            break;
    }
}

void LoadTest::OnServerInfo(Bot& b, const net::ServerInfoMsg& m) {
    if (!haveInfo_) {
        // Beim ersten Mal den Server vorstellen und vor zu kleinen Grenzen warnen
        haveInfo_ = true;
        info_ = m;
        Log(Fmt("Server \"%s\": %d/%d Spieler, %d/%d Zuschauer online; %s (%d feindliche Schiffe), %d "
                "Handelsstation(en), %d Werft(en), Mond %s",
                m.serverName.c_str(), m.playersOnline, m.maxPlayers, m.spectators, m.maxSpectators,
                DifficultyName(m.settings.difficulty), m.settings.enemyShips, m.settings.tradeStations,
                m.settings.shipyards, MoonColorName(m.settings.moon)));
        if (opt_.bots + m.playersOnline > m.maxPlayers)
            Log(Fmt("Achtung: Der Server erlaubt nur %d Spieler gleichzeitig (%d schon online). Überzählige Bots "
                    "bekommen \"Server voll\" – am Server MAX_PLAYERS erhöhen (höchstens %d).",
                    m.maxPlayers, m.playersOnline, limits::MAX_PLAYERS_LIMIT));
        if (opt_.spectators + m.spectators > m.maxSpectators)
            Log(Fmt("Achtung: Der Server hat nur %d Zuschauerplätze (%d belegt).", m.maxSpectators, m.spectators));
    }
    // Der Server wiederholt ServerInfo alle 2 s, solange wir in der Lobby sind.
    // Nur die erste löst die Anmeldung aus.
    if (b.phase != Phase::Lobby) return;

    if (b.spectator) {
        SendTo(b, net::EncodeSimple(net::MsgType::Spectate), true);
        SetPhase(b, Phase::AwaitWelcome);
        return;
    }
    // Schlüssel = PBKDF2(Passwort, Salz aus Server-Salz und Name), DESIGN 3.3.
    // Im Hilfs-Thread rechnen (siehe Dateikopf). Die Lambda-Funktion bekommt
    // KOPIEN aller Werte, damit sie nichts aus dem Haupt-Thread anfasst.
    const std::string password = opt_.password;
    const std::string nameLower = b.nameLower;
    const std::array<uint8_t, 16> salt = m.realmSalt;
    auto derive = [password, nameLower, salt]() {
        return crypto::DeriveLoginKey(password, nameLower, salt, net::KDF_ITERATIONS);
    };
    try {
        b.keyJob = std::async(std::launch::async, derive);
    } catch (const std::system_error&) {
        // Kein Thread verfügbar (sehr selten): dann eben hier rechnen
        std::promise<crypto::Digest> done;
        done.set_value(derive());
        b.keyJob = done.get_future();
    }
    SetPhase(b, Phase::DerivingKey);
}

void LoadTest::OnError(Bot& b, const net::ErrorMsg& m) {
    ++errorCodes_[static_cast<int>(m.code)];
    b.lastError = m.code;
    switch (b.phase) {
        case Phase::Registering:
            // Konto gibt es schon (z. B. vom letzten Lasttest): anmelden statt anlegen
            if (m.code == net::ErrorCode::NameTaken) StartLogin(b);
            else LoginError(b, m.code);
            break;
        case Phase::Lobby:
        case Phase::DerivingKey:
        case Phase::AwaitChallenge:
        case Phase::AwaitWelcome:
        case Phase::RetryWait:
            // z. B. VersionMismatch gleich nach dem Verbinden, WrongLogin,
            // ServerFull, SpectatorsFull, TooManyAttempts
            LoginError(b, m.code);
            break;
        case Phase::InGame:
            // Im Spiel bedeutet ein Fehler meist "Server wird beendet"; die
            // Trennung folgt (OnDisconnect nimmt lastError als Grund).
            LogBot(b, "Server meldet im Spiel: " + ErrorLabel(m.code));
            break;
        default:
            break;   // Failed/Leaving: z. B. TooManyAttempts nach dem letzten Fehlversuch
    }
}

void LoadTest::OnChallenge(Bot& b, const net::LoginChallengeMsg& m) {
    if (b.phase != Phase::AwaitChallenge) {
        ++Cur(b).unexpected;
        return;
    }
    // Beweis, dass wir den Schlüssel kennen, ohne ihn zu senden:
    // HMAC(Schlüssel, Nonce + Name klein). Die Nonce gilt nur für diesen Versuch.
    net::LoginProofMsg p;
    const crypto::Digest proof = crypto::LoginProof(b.key, m.nonce, b.nameLower);
    std::copy(proof.begin(), proof.end(), p.proof.begin());
    SendTo(b, net::Encode(p), true);
    SetPhase(b, Phase::AwaitWelcome);
}

void LoadTest::OnWelcome(Bot& b, const net::WelcomeMsg& m) {
    if (b.phase == Phase::InGame) {
        // Erneutes Welcome = Welt wurde neu aufgebaut (Einstellungen auf der
        // Webseite geändert, DESIGN 3.8). Eingaben laufen einfach weiter: Die
        // fortlaufende seq ist für den Server in jedem Fall gültig.
        ++rewelcomes_;
        return;
    }
    if (b.phase != Phase::Registering && b.phase != Phase::AwaitWelcome) {
        ++Cur(b).unexpected;
        return;
    }
    const net::Role expected = b.spectator ? net::Role::Spectator : net::Role::Player;
    if (m.role != expected) ++Cur(b).unexpected;   // trotzdem weitermachen

    const double now = Now();
    if (b.spectator) ++spectating_;
    else if (b.phase == Phase::Registering) ++registered_;
    else ++loggedInViaLogin_;
    const double t = now - b.connectedAt;   // Anmeldung inkl. PBKDF2 (ca. 20 ms) beim Bot
    loginTimeSum_ += t;
    loginTimeMax_ = std::max(loginTimeMax_, t);
    ++loginTimeCount_;

    b.lastError = net::ErrorCode::None;
    b.nextInputAt = now;
    b.nextPingAt = now + PING_INTERVAL;
    SetPhase(b, Phase::InGame);
}

void LoadTest::OnSnapshot(Bot& b, const net::SnapshotMsg& m) {
    if (b.phase != Phase::InGame) return;   // z. B. ein Nachzügler nach Leave
    Stats& s = Cur(b);
    s.shipsInSnapshots += m.ships.size();

    // Reihenfolge und Verluste: tick ist der Simulationsschritt des Servers.
    // Spieler bekommen jeden 2., Zuschauer jeden 3. Schritt einen Snapshot
    // (DESIGN 3.6/3.7). Größere Abstände = unterwegs verlorene Pakete (oder
    // ein Server, der nicht mehr mitkommt). Unzuverlässige Pakete können sich
    // auch überholen: Ein älterer als der letzte wird verworfen, wie im Spiel.
    if (b.anyTick) {
        const auto diff = static_cast<int32_t>(m.tick - b.lastTick);   // Überlauf-fest
        if (diff <= 0) {
            ++s.snapshotsOld;
            return;
        }
        const int32_t step = b.spectator ? net::SPECTATOR_SNAPSHOT_EVERY : net::SNAPSHOT_EVERY;
        if (diff > step) s.snapshotsLost += static_cast<uint64_t>(diff / step - 1);
    }
    b.anyTick = true;
    b.lastTick = m.tick;
    if (!m.hasMe) return;   // Zuschauer haben keinen eigenen Zustand

    const net::SnapMe& me = m.me;
    // Unbestätigte Eingaben: gesendet, aber vom Server noch nicht angewendet
    // (unterwegs oder in seiner Warteschlange). Mal 33 ms ergibt ungefähr die
    // Eingabeverzögerung, die ein Spieler spüren würde.
    if (me.ackSeq > 0 && me.ackSeq <= b.seq) {
        s.ackLagSum += b.seq - me.ackSeq;
        ++s.ackLagSamples;
    }
    if (b.hasMe && b.shipId >= 0 && me.shipId < 0) ++crashes_;   // Schiff weg -> wartet auf ein neues
    b.hasMe = true;
    b.shipId = me.shipId;
    b.x = me.x;
    b.z = me.z;
    b.yaw = me.yaw;
    b.dockedAt = me.dockedAt;
}

// Wie lange darf enet_host_service höchstens warten? Bis zur nächsten
// fälligen Eingabe eines Bots (bzw. Rampe), aber mindestens 1 ms (sonst
// liefe die Schleife ohne Pause und belegte einen ganzen CPU-Kern) und
// höchstens 5 ms (Zeitüberschreitungen, Pings, Abbruch durch Strg+C).
uint32_t LoadTest::NextTimeoutMs(double now) const {
    double next = now + 0.005;
    if (nextBot_ < bots_.size()) next = std::min(next, static_cast<double>(nextBot_) * (opt_.rampMs / 1000.0));
    for (const Bot& b : bots_)
        if (b.phase == Phase::InGame && !b.spectator) next = std::min(next, b.nextInputAt);
    const double ms = (next - now) * 1000.0;
    if (ms <= 0.0) return 0;
    return static_cast<uint32_t>(std::clamp(std::ceil(ms), 1.0, 5.0));
}

// ---------------------------------------------------------------------------
// Messen
// ---------------------------------------------------------------------------
int LoadTest::CountPhase(bool spectator, Phase p) const {
    int n = 0;
    for (const Bot& b : bots_)
        if (b.spectator == spectator && b.phase == p) ++n;
    return n;
}

int LoadTest::CountConnected() const {
    int n = 0;
    for (const Bot& b : bots_)
        if (b.peer && (b.phase == Phase::InGame || InLobbyPhase(b.phase))) ++n;
    return n;
}

bool LoadTest::AllFinished() const {
    for (const Bot& b : bots_)
        if (b.phase != Phase::Failed && b.phase != Phase::Lost && b.phase != Phase::Closed) return false;
    return true;
}

// "Bot-Sekunden" im Spiel aufsummieren. Alle Raten (Snapshots pro Sekunde
// und Bot, Bytes pro Sekunde und Bot) teilen durch diese Summe. So stimmen
// die Werte auch, wenn Bots erst während der Messung dazukommen (Rampe) oder
// zwischendurch verloren gehen.
void LoadTest::AccumulateClientTime(double dt) {
    curPlayers_.clientSeconds += dt * CountPhase(false, Phase::InGame);
    curSpectators_.clientSeconds += dt * CountPhase(true, Phase::InGame);
}

// RTT (Round Trip Time) = Zeit von einem zuverlässigen Paket bis zu seiner
// Bestätigung. ENet glättet den Wert (gleitender Mittelwert), "max" ist also
// das Maximum der geglätteten Werte, keine einzelne Paketspitze.
void LoadTest::SampleRtt() {
    for (const Bot& b : bots_) {
        if (b.phase != Phase::InGame || !b.peer) continue;
        Stats& s = b.spectator ? curSpectators_ : curPlayers_;
        const uint32_t rtt = b.peer->roundTripTime;
        s.rttSum += rtt;
        ++s.rttSamples;
        s.rttMax = std::max(s.rttMax, rtt);
    }
}

// ENet zählt in 32-Bit-Variablen; bei 1-2 MB/s liefen sie nach etwa einer
// Stunde über. Die ENet-Doku sieht vor, dass der Benutzer sie selbst
// zurücksetzt: Darum regelmäßig in 64-Bit-Summen übernehmen und auf 0 setzen.
// Gezählt wird alles, was über den UDP-Socket geht, inkl. ENet-Kopf und
// Bestätigungen (ohne die 28 Byte UDP/IP-Kopf je Paket).
void LoadTest::HarvestEnetTotals() {
    bytesIn_ += host_->totalReceivedData;
    bytesOut_ += host_->totalSentData;
    packetsIn_ += host_->totalReceivedPackets;
    packetsOut_ += host_->totalSentPackets;
    host_->totalReceivedData = host_->totalSentData = 0;
    host_->totalReceivedPackets = host_->totalSentPackets = 0;
}

void LoadTest::PrintInterval(double now) {
    HarvestEnetTotals();
    const double secs = std::max(1e-6, now - lastReportAt_);
    const Stats& p = curPlayers_;
    const Stats& sp = curSpectators_;

    int failed = 0;
    for (const auto& f : failures_) failed += f.second;
    const int playersIn = CountPhase(false, Phase::InGame);
    const int spectatorsIn = CountPhase(true, Phase::InGame);

    std::string line1 = Fmt("Spieler %d/%d im Spiel", playersIn, opt_.bots);
    if (opt_.spectators > 0) line1 += Fmt(", Zuschauer %d/%d", spectatorsIn, opt_.spectators);
    line1 += Fmt(" | verbunden %d | Anmeldung fehlgeschl. %d | Verbindung verloren %d", CountConnected(),
                 failed + connectFailures_, lostTotal_);
    Log(line1);

    if (p.clientSeconds > 0.0) {
        const Counter& snap = p.recv[CAT_SNAPSHOT];
        std::printf("            je Spieler: %.1f Snapshots/s (Ø %s, verloren %.1f %%) | Events %s | Deposits %s | "
                    "Players %s\n",
                    SafeDiv(static_cast<double>(snap.count), p.clientSeconds),
                    Size(SafeDiv(static_cast<double>(snap.bytes), static_cast<double>(snap.count))).c_str(),
                    100.0 * SafeDiv(static_cast<double>(p.snapshotsLost), static_cast<double>(snap.count + p.snapshotsLost)),
                    Rate(SafeDiv(static_cast<double>(p.recv[CAT_EVENTS].bytes), p.clientSeconds)).c_str(),
                    Rate(SafeDiv(static_cast<double>(p.recv[CAT_DEPOSITS].bytes), p.clientSeconds)).c_str(),
                    Rate(SafeDiv(static_cast<double>(p.recv[CAT_PLAYERS].bytes), p.clientSeconds)).c_str());
    }
    if (sp.clientSeconds > 0.0) {
        const Counter& snap = sp.recv[CAT_SNAPSHOT];
        uint64_t all = 0;
        for (const Counter& c : sp.recv) all += c.bytes;
        std::printf("            je Zuschauer: %.1f Snapshots/s (Ø %s) | gesamt %s\n",
                    SafeDiv(static_cast<double>(snap.count), sp.clientSeconds),
                    Size(SafeDiv(static_cast<double>(snap.bytes), static_cast<double>(snap.count))).c_str(),
                    Rate(SafeDiv(static_cast<double>(all), sp.clientSeconds)).c_str());
    }
    const uint64_t rttSamples = p.rttSamples + sp.rttSamples;
    std::printf("            Netz: ein %s, aus %s | RTT Ø %.0f ms, max %u ms | unbestätigte Eingaben Ø %.1f\n",
                Rate(static_cast<double>(bytesIn_ - lastReportBytesIn_) / secs).c_str(),
                Rate(static_cast<double>(bytesOut_ - lastReportBytesOut_) / secs).c_str(),
                SafeDiv(static_cast<double>(p.rttSum + sp.rttSum), static_cast<double>(rttSamples)),
                static_cast<unsigned>(std::max(p.rttMax, sp.rttMax)),
                SafeDiv(static_cast<double>(p.ackLagSum), static_cast<double>(p.ackLagSamples)));
    std::fflush(stdout);

    // Abschnitt abschließen: in die Gesamtsummen übernehmen, neu beginnen
    AddStats(totalPlayers_, curPlayers_);
    AddStats(totalSpectators_, curSpectators_);
    curPlayers_ = Stats{};
    curSpectators_ = Stats{};
    lastReportBytesIn_ = bytesIn_;
    lastReportBytesOut_ = bytesOut_;
    lastReportAt_ = now;
}

// ---------------------------------------------------------------------------
// Testende: alle abmelden
// ---------------------------------------------------------------------------
void LoadTest::LeaveAll() {
    for (Bot& b : bots_) {
        if (!b.peer) continue;
        if (b.phase == Phase::Connecting) {
            // Verbindung gibt es noch nicht: lokal verwerfen genügt
            ResetPeer(b);
            SetPhase(b, Phase::Closed);
        } else if (b.phase == Phase::InGame) {
            // Leave: Der Server speichert das Konto und entfernt das Schiff
            // sofort. disconnect_later trennt erst, wenn Leave bestätigt ist
            // (sonst könnte die Trennung das Leave "überholen").
            if (SendTo(b, net::EncodeSimple(net::MsgType::Leave), true)) ++leaveSent_;
            enet_peer_disconnect_later(b.peer, 0);
            SetPhase(b, Phase::Leaving);
        } else if (InLobbyPhase(b.phase)) {
            enet_peer_disconnect(b.peer, 0);
            SetPhase(b, Phase::Leaving);
        }
        // Failed: Trennung läuft schon; das DISCONNECT-Ereignis wird unten abgeholt
    }
    enet_host_flush(host_);

    // Auf die Bestätigungen warten (höchstens LEAVE_TIMEOUT). Empfangene
    // Pakete werden nur noch freigegeben, die Messung ist vorbei.
    const double until = Now() + LEAVE_TIMEOUT;
    auto anyOpen = [&] {
        for (const Bot& b : bots_)
            if (b.peer) return true;
        return false;
    };
    while (anyOpen() && Now() < until) {
        ENetEvent ev;
        int r = enet_host_service(host_, &ev, 10);
        while (r > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                Bot* b = static_cast<Bot*>(ev.peer->data);
                ev.peer->data = nullptr;
                if (b && b->peer == ev.peer) {
                    b->peer = nullptr;
                    if (b->phase == Phase::Leaving) {
                        ++closedClean_;
                        SetPhase(*b, Phase::Closed);
                    }
                }
            }
            r = enet_host_check_events(host_, &ev);
        }
        if (r < 0) break;
    }
    // Wer nicht geantwortet hat, wird lokal verworfen (der Server merkt es
    // spätestens an seiner Zeitüberschreitung).
    for (Bot& b : bots_) {
        if (!b.peer) continue;
        if (b.phase == Phase::Leaving) ++closedForced_;
        ResetPeer(b);
        if (b.phase == Phase::Leaving) SetPhase(b, Phase::Closed);
    }
}

// ---------------------------------------------------------------------------
// Abschlussbericht
// ---------------------------------------------------------------------------
void LoadTest::PrintFinal(const std::string& endReason) {
    const Stats& p = totalPlayers_;
    const Stats& sp = totalSpectators_;
    const double secs = std::max(1e-6, measureEnd_);
    int failed = 0;
    for (const auto& f : failures_) failed += f.second;

    std::printf("\n===================== Ergebnis des Lasttests =====================\n");
    std::printf("Server             %s", addressText_.c_str());
    if (haveInfo_)
        std::printf(" \"%s\", max. %d Spieler, %s (%d Gegner)", info_.serverName.c_str(), info_.maxPlayers,
                    DifficultyName(info_.settings.difficulty), info_.settings.enemyShips);
    std::printf("\nDauer              %.1f s (%s), Rampe %d ms\n", measureEnd_, endReason.c_str(), opt_.rampMs);
    std::printf("Bots               %d Spieler + %d Zuschauer geplant, %d gestartet\n", opt_.bots, opt_.spectators,
                static_cast<int>(nextBot_));
    std::printf("  verbunden        %d (Verbindungsaufbau fehlgeschlagen: %d)", connectedEver_, connectFailures_);
    if (connectTimeCount_ > 0)
        std::printf(", Aufbau Ø %.0f ms, max %.0f ms", 1000.0 * connectTimeSum_ / connectTimeCount_,
                    1000.0 * connectTimeMax_);
    std::printf("\n  im Spiel         %d Spieler (neues Konto %d, Anmeldung mit vorhandenem Konto %d), %d Zuschauer\n",
                registered_ + loggedInViaLogin_, registered_, loggedInViaLogin_, spectating_);
    if (loginTimeCount_ > 0)
        std::printf("  Anmeldedauer     Ø %.0f ms, max %.0f ms (vom Verbinden bis Welcome, inkl. ~20 ms PBKDF2)\n",
                    1000.0 * loginTimeSum_ / loginTimeCount_, 1000.0 * loginTimeMax_);
    if (retriesTotal_ > 0) std::printf("  Wiederholungen   %d (\"bereits angemeldet\")\n", retriesTotal_);
    std::printf("  fehlgeschlagen   %d\n", failed);
    for (const auto& f : failures_) std::printf("      %4d x %s\n", f.second, f.first.c_str());
    std::printf("  getrennt         %d (Verbindung im Spiel verloren)\n", lostTotal_);
    for (const auto& l : losses_) std::printf("      %4d x %s\n", l.second, l.first.c_str());
    if (!errorCodes_.empty()) {
        std::printf("  Fehlercodes vom Server (alle empfangenen Error-Nachrichten):\n");
        for (const auto& e : errorCodes_) {
            const auto code = static_cast<net::ErrorCode>(e.first);
            std::printf("      %4d x %s%s\n", e.second, ErrorLabel(code).c_str(),
                        code == net::ErrorCode::NameTaken ? " – normal: Konto existierte, Bot meldet sich an" : "");
        }
    }

    if (p.clientSeconds > 0.0) {
        const double cs = p.clientSeconds;
        const Counter& snap = p.recv[CAT_SNAPSHOT];
        std::printf("\nEmpfang je Spieler-Bot (Mittel über %.0f Bot-Sekunden im Spiel):\n", cs);
        std::printf("  %-28s %10s %10s %12s\n", "Nachricht", "Anzahl/s", "Ø Größe", "Datenmenge");
        uint64_t allBytes = 0;
        for (int c = 0; c < CAT_COUNT; ++c) {
            const Counter& k = p.recv[c];
            allBytes += k.bytes;
            if (k.count == 0) continue;
            std::printf("  %-28s %10.2f %10s %12s\n", CATEGORY_NAMES[c], static_cast<double>(k.count) / cs,
                        Size(static_cast<double>(k.bytes) / static_cast<double>(k.count)).c_str(),
                        Rate(static_cast<double>(k.bytes) / cs).c_str());
        }
        std::printf("  %-28s %10s %10s %12s\n", "gesamt (Nutzdaten)", "", "", Rate(static_cast<double>(allBytes) / cs).c_str());
        std::printf("  Snapshots: Soll %d/s; verloren ca. %.2f %%, vertauscht %.2f %%, Ø %.1f fremde Schiffe je Snapshot\n",
                    net::TICK_RATE / net::SNAPSHOT_EVERY,
                    100.0 * SafeDiv(static_cast<double>(p.snapshotsLost), static_cast<double>(snap.count + p.snapshotsLost)),
                    100.0 * SafeDiv(static_cast<double>(p.snapshotsOld), static_cast<double>(snap.count)),
                    SafeDiv(static_cast<double>(p.shipsInSnapshots), static_cast<double>(snap.count)));
        std::printf("  Ereignisse: Ø %.1f pro Sekunde\n", static_cast<double>(p.events) / cs);
        std::printf("Senden je Spieler-Bot: %.1f Eingaben/s (Ø %s), %d Stationsaktionen insgesamt\n",
                    static_cast<double>(p.inputsSent) / cs,
                    Size(SafeDiv(static_cast<double>(p.inputBytes), static_cast<double>(p.inputsSent))).c_str(),
                    static_cast<int>(p.dockActions));
        const double lag = SafeDiv(static_cast<double>(p.ackLagSum), static_cast<double>(p.ackLagSamples));
        std::printf("Unbestätigte Eingaben beim Snapshot: Ø %.1f (ca. %.0f ms Eingabeverzögerung)\n", lag,
                    lag * 1000.0 / net::TICK_RATE);
        std::printf("Spielgeschehen: %d-mal angedockt, %d Abstürze, %d erneute Welcome (Welt-Neuaufbau)\n", docks_,
                    crashes_, rewelcomes_);
    }
    if (sp.clientSeconds > 0.0) {
        const double cs = sp.clientSeconds;
        const Counter& snap = sp.recv[CAT_SNAPSHOT];
        uint64_t allBytes = 0;
        for (const Counter& k : sp.recv) allBytes += k.bytes;
        std::printf("\nEmpfang je Zuschauer: %.2f Snapshots/s (Soll %d/s, Ø %s, Ø %.0f Schiffe), Events %s, "
                    "Deposits %s, gesamt %s\n",
                    static_cast<double>(snap.count) / cs, net::TICK_RATE / net::SPECTATOR_SNAPSHOT_EVERY,
                    Size(SafeDiv(static_cast<double>(snap.bytes), static_cast<double>(snap.count))).c_str(),
                    SafeDiv(static_cast<double>(sp.shipsInSnapshots), static_cast<double>(snap.count)),
                    Rate(static_cast<double>(sp.recv[CAT_EVENTS].bytes) / cs).c_str(),
                    Rate(static_cast<double>(sp.recv[CAT_DEPOSITS].bytes) / cs).c_str(),
                    Rate(static_cast<double>(allBytes) / cs).c_str());
    }

    std::printf("\nNetzwerk gesamt (alle Bots, ENet inkl. Protokoll-Kopf und Bestätigungen):\n");
    std::printf("  empfangen        %s (%s, %.0f Pakete/s)\n", Size(static_cast<double>(bytesIn_)).c_str(),
                Rate(static_cast<double>(bytesIn_) / secs).c_str(), static_cast<double>(packetsIn_) / secs);
    std::printf("  gesendet         %s (%s, %.0f Pakete/s)\n", Size(static_cast<double>(bytesOut_)).c_str(),
                Rate(static_cast<double>(bytesOut_) / secs).c_str(), static_cast<double>(packetsOut_) / secs);
    std::printf("  = Upload des Servers ca. %.2f Mbit/s (+ 28 Byte UDP/IP je Paket)\n",
                static_cast<double>(bytesIn_) * 8.0 / secs / 1e6);
    const uint64_t rttSamples = p.rttSamples + sp.rttSamples;
    if (rttSamples > 0)
        std::printf("Paketlaufzeit (ENet-RTT, geglättet): Ø %.1f ms, max %u ms\n",
                    static_cast<double>(p.rttSum + sp.rttSum) / static_cast<double>(rttSamples),
                    static_cast<unsigned>(std::max(p.rttMax, sp.rttMax)));
    if (p.broken + sp.broken + p.unexpected + sp.unexpected + p.recv[CAT_UNKNOWN].count + sp.recv[CAT_UNKNOWN].count > 0)
        std::printf("Auffällige Nachrichten: %d nicht dekodierbar, %d unbekannter Typ, %d unerwartet\n",
                    static_cast<int>(p.broken + sp.broken),
                    static_cast<int>(p.recv[CAT_UNKNOWN].count + sp.recv[CAT_UNKNOWN].count),
                    static_cast<int>(p.unexpected + sp.unexpected));
    else
        std::printf("Alle empfangenen Nachrichten waren gültig.\n");
    if (inputSkips_ > 0)
        std::printf("ACHTUNG: Der Lasttest-Rechner kam nicht hinterher (%d Eingabeschritte ausgelassen). Die\n"
                    "         Messwerte sind dann nicht verlässlich – weniger Bots oder einen anderen Rechner verwenden.\n",
                    static_cast<int>(inputSkips_));
    std::printf("Abmelden: %d Leave gesendet, %d Verbindungen sauber getrennt, %d ohne Antwort verworfen\n", leaveSent_,
                closedClean_, closedForced_);
    std::printf("===================================================================\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Hauptschleife
// ---------------------------------------------------------------------------
int LoadTest::Run() {
    if (!Init()) return 1;

    std::printf("Mondschürfer-Lasttest\n");
    std::printf("  Server     %s\n", addressText_.c_str());
    if (opt_.bots > 0)
        std::printf("  Spieler    %d (%s bis %s), Passwort \"%s\"\n", opt_.bots, bots_.front().name.c_str(),
                    bots_[static_cast<size_t>(opt_.bots) - 1].name.c_str(), opt_.password.c_str());
    std::printf("  Zuschauer  %d\n  Dauer      %d s, ein neuer Bot alle %d ms\n", opt_.spectators, opt_.seconds,
                opt_.rampMs);
    std::printf("  (Strg+C beendet vorzeitig; die Bots melden sich dann sauber ab)\n\n");
    std::fflush(stdout);

    std::string endReason = "Testdauer erreicht";
    int exitCode = 0;
    double lastLoop = Now();
    double nextReportAt = REPORT_INTERVAL;
    double nextRttAt = RTT_SAMPLE_INTERVAL;
    lastReportAt_ = lastLoop;

    while (true) {
        const double now = Now();
        if (g_stop.load()) {
            endReason = "vorzeitig abgebrochen mit Strg+C";
            break;
        }
        if (now >= opt_.seconds) break;

        StartDueBots(now);
        for (Bot& b : bots_) UpdateBot(b, now);
        // Eingaben sofort losschicken (sonst erst beim nächsten Service-Aufruf)
        enet_host_flush(host_);
        Service(NextTimeoutMs(now));

        const double after = Now();
        AccumulateClientTime(after - lastLoop);
        lastLoop = after;
        if (after >= nextRttAt) {
            SampleRtt();
            nextRttAt += RTT_SAMPLE_INTERVAL;
        }
        if (after >= nextReportAt) {
            PrintInterval(after);
            nextReportAt += REPORT_INTERVAL;
        }
        // Nicht erreichbar: Der erste Bot hat CONNECT_TIMEOUT lang keine
        // Antwort bekommen und noch kein einziger ist verbunden -> sinnlos,
        // weiterzumachen.
        if (!anyConnected_ && connectFailures_ > 0) {
            std::fprintf(stderr,
                         "\nFehler: Server %s nicht erreichbar (keine Antwort innerhalb von %.0f s).\n"
                         "  Läuft der Server? Stimmen Adresse und UDP-Port? Lässt eine Firewall UDP durch?\n"
                         "  (Auch möglich: alle Verbindungsplätze des Servers sind belegt.)\n",
                         addressText_.c_str(), CONNECT_TIMEOUT);
            endReason = "Server nicht erreichbar";
            exitCode = 1;
            break;
        }
        if (nextBot_ == bots_.size() && AllFinished()) {
            endReason = "alle Bots beendet oder getrennt";
            break;
        }
    }

    // Messung abschließen (das Abmelden zählt nicht mehr mit)
    measureEnd_ = Now();
    HarvestEnetTotals();
    AddStats(totalPlayers_, curPlayers_);
    AddStats(totalSpectators_, curSpectators_);
    curPlayers_ = Stats{};
    curSpectators_ = Stats{};

    if (exitCode == 0) Log("Test beendet (" + endReason + ") – Bots melden sich ab ...");
    LeaveAll();
    if (exitCode == 0) {
        PrintFinal(endReason);
        // Ohne einen einzigen angemeldeten Bot ist der Test gescheitert
        if (registered_ + loggedInViaLogin_ + spectating_ == 0) {
            std::fprintf(stderr, "Kein Bot konnte sich anmelden – siehe Fehlergründe oben.\n");
            exitCode = 1;
        }
    }
    return exitCode;
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Die Texte sind UTF-8 (Umlaute). Die Windows-Konsole erwartet sonst eine
    // alte Codepage und zeigt "Ã¼" statt "ü".
    SetConsoleOutputCP(CP_UTF8);
#endif
    Options opt;
    switch (ParseArgs(argc, argv, opt)) {
        case ParseResult::Help:
            PrintHelp();
            return 0;
        case ParseResult::Error:
            return 2;
        case ParseResult::Ok:
            break;
    }
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    LoadTest test(opt);
    return test.Run();
}
