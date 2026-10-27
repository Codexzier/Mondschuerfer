// ---------------------------------------------------------------------------
// GameServer.cpp – Netzwerk, Anmeldung, Simulation und Verteilung der
// Spielwelt (Überblick und Ablauf: siehe GameServer.hpp).
//
// Aufbau dieser Datei (in dieser Reihenfolge):
//   1. Konstanten und kleine Hilfsfunktionen (Zeit, IP-Adressen, Prozessdaten)
//   2. Lebenszyklus: Start, Hauptschleife (Run), Beenden (Shutdown)
//   3. Ein Simulationsschritt (Step) und was dazugehört
//   4. Netzwerk: Verbindungen, Nachrichten der Clients, Anmeldung
//   5. Nachrichten an die Clients bauen (Snapshot, Lagerstätten, Listen)
//   6. Einstellungen von der Webseite, Neuaufbau der Welt
//   7. Speichern, Status für die Webseite
//   8. Benchmark mit Bots
//
// Alles hier läuft im Spiel-Thread. Es gibt keine Sperren (Mutex) außer in
// SharedState, dem einzigen Berührungspunkt mit dem Web-Thread.
//
// Grundsatz Sicherheit: Jede Nachricht eines Clients kann manipuliert sein.
// Deshalb prüft der Server Zustand (darf dieser Client das gerade?), Längen
// (Protocol.cpp) und Wertebereiche, bevor er etwas an der Welt ändert. Ein
// kaputtes Paket wird verworfen, nie "repariert".
// ---------------------------------------------------------------------------
#include "GameServer.hpp"
#include "Crypto.hpp"
#include "Models.hpp"
#include "Util.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>

// ENet bindet unter Windows <winsock2.h>/<windows.h> ein. Die Windows-Header
// definieren Namen, die mit raylib.h (über World.hpp schon eingebunden)
// zusammenstoßen (Rectangle, CloseWindow, DrawText ...). Mit diesen Schaltern
// lässt Windows die betroffenen Teile (GDI, USER) weg. NOMINMAX verhindert die
// Makros min/max, die std::min/std::max kaputt machen würden.
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
#include <windows.h>   // GetProcessTimes (CPU-Zeit des Prozesses)
#undef near
#undef far
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>   // getrusage (CPU-Zeit des Prozesses)
#endif

namespace {

// ===========================================================================
// 1. Konstanten und Hilfsfunktionen
// ===========================================================================

// --- Netzwerk ---
// ENet-Plätze ("Peers"). Bewusst viel mehr, als Spieler und Zuschauer
// brauchen: ENet belegt einen Platz schon beim ersten Verbindungspaket, noch
// bevor die Verbindung steht ("halboffen"). Wer viele solche Pakete schickt,
// könnte sonst alle Plätze belegen. Wie viele wirklich mitspielen, begrenzen
// maxPlayers, MAX_SPECTATORS und MAX_LOBBY_CONNECTIONS. Ein freier Platz
// kostet rund 460 Byte, alle 1024 zusammen also etwa 0,5 MB.
constexpr size_t PEER_COUNT = 1024;
constexpr int MAX_LOBBY_CONNECTIONS = 128;        // verbunden, aber (noch) nicht angemeldet
// Halboffene Verbindungen (Handschlag nicht beendet) nach 1,5..3 s aufgeben
// statt nach ENets Standard von 30 s. Ein ehrlicher Client antwortet in
// Bruchteilen einer Sekunde (siehe GuardHalfOpenPeers).
constexpr enet_uint32 HALF_OPEN_TIMEOUT_MIN_MS = 1500;
constexpr enet_uint32 HALF_OPEN_TIMEOUT_MAX_MS = 3000;
constexpr size_t MAX_PACKET_SIZE = 64 * 1024;     // ENet-Standard wären 32 MB pro Paket (Speicherangriff)
constexpr size_t MAX_WAITING_DATA = 256 * 1024;   // unverarbeitete Daten pro Verbindung
constexpr int INPUT_QUEUE_MAX = 6;                // ältere Eingaben verwerfen (HandleInput)
// Regelung der Eingabe-Warteschlange (ApplyInputs): Liegen eine Sekunde lang
// IMMER mehr als INPUT_QUEUE_TARGET Eingaben bereit, wird eine übersprungen.
constexpr int INPUT_QUEUE_TARGET = 1;
constexpr int INPUT_QUEUE_WINDOW = net::TICK_RATE;   // Schritte (= 1 s)
constexpr int MAX_BAD_PACKETS = 50;               // danach wird getrennt
constexpr int MAX_MSGS_PER_SECOND = 240;          // normal: 30 Input/s + ein paar zuverlässige
constexpr int FLOOD_DISCONNECT = 2000;            // so viele pro Sekunde -> trennen
constexpr double LOBBY_TIMEOUT = 15.0 * 60.0;     // so lange darf man ohne Anmeldung verbunden sein
// Abmelden außerhalb einer Station: So lange bleibt das Schiff noch ohne
// Steuerung in der Welt und kann abgeschossen werden (LogoutPlayer).
constexpr double LOGOUT_GRACE = 10.0;
// Chat-Flutbremse (HandleChat): bis zu CHAT_BURST Nachrichten direkt
// hintereinander, danach eine pro 1 / CHAT_PER_SECOND Sekunden.
constexpr double CHAT_BURST = 4.0;
constexpr double CHAT_PER_SECOND = 0.5;

// --- Anmeldung (docs/Mehrspieler.md Abschnitt 8, siehe LoginBlocked) ---
// Alle Grenzen gelten für ein Zeitfenster von 10 Minuten.
constexpr int MAX_LOGIN_FAILURES_PER_CONNECTION = 5;   // dann wird getrennt
constexpr int MAX_LOGIN_FAILURES_PER_NAME_IP = 10;     // ein Konto von einer Adresse aus
constexpr int MAX_LOGIN_FAILURES_PER_NAME = 100;       // ein Konto insgesamt ("Notbremse")
constexpr int MAX_LOGIN_FAILURES_PER_IP = 30;          // eine öffentliche Adresse, alle Konten zusammen
constexpr double LOGIN_FAILURE_WINDOW = 10.0 * 60.0;
constexpr size_t MAX_TRACKED_FAILURES = 20000;    // Speicherschutz für jede der Fehlversuchslisten
constexpr size_t MAX_TRUSTED_IPS_PER_NAME = 4;    // Adressen mit erfolgreicher Anmeldung je Konto
constexpr int MAX_REGISTRATIONS_PER_IP = 10;      // neue Konten je öffentlicher IP ...
constexpr double REGISTRATION_WINDOW = 3600.0;    // ... pro Stunde

// --- Zeitplan in Simulationsschritten (30 pro Sekunde) ---
constexpr uint64_t PLAYERS_EVERY = net::TICK_RATE;              // Spielerliste 1/s
constexpr uint64_t SERVERINFO_EVERY = 2 * net::TICK_RATE;       // Lobby-Info alle 2 s
constexpr uint64_t SAVE_EVERY = 30 * net::TICK_RATE;            // speichern alle 30 s
constexpr uint64_t DEPOSITS_MIN_TICKS_PLAYER = 8;               // höchstens ~4 pro Sekunde
constexpr uint64_t DEPOSITS_MIN_TICKS_SPECTATOR = net::TICK_RATE; // höchstens 1 pro Sekunde
constexpr uint64_t UNDOCK_INTERACT_GUARD = 10;                  // ~0,33 s nach dem Abdocken

// --- Größe der Snapshots begrenzen (die nächsten zuerst) ---
constexpr size_t MAX_SNAP_SHIPS = 80;
constexpr size_t MAX_SNAP_BULLETS = 128;
constexpr size_t MAX_SNAP_WRECKS = 48;
constexpr size_t MAX_SPECTATOR_LIST = 512;
constexpr size_t MAX_EVENTS_PER_MSG = 1024;       // Client lehnt > 4096 ab (Protocol.cpp)

// --- Lastprotokoll (logs/last-JJJJ-MM-TT.csv, siehe RecordLoad) ---
// Eine Minute gilt als Lastspitze, wenn ein Schritt länger als das halbe
// Zeitbudget (33,3 ms / 2 = 16,7 ms) dauerte oder die CPU über 80 % lag.
constexpr double PEAK_TICK_MS = 1000.0 / net::TICK_RATE / 2.0;
constexpr double PEAK_CPU_PERCENT = 80.0;   // eines Kerns
// Kopfzeile der Tagesdateien. Trennzeichen ";", Dezimalkomma und die
// UTF-8-Kennung (BOM) am Anfang, damit eine deutsche Tabellenkalkulation die
// Datei direkt richtig öffnet (auch die Umlaute und "Ø").
constexpr const char* LOAD_LOG_HEADER =
    "\xEF\xBB\xBF"
    "Zeit;Unix-Zeit;Spieler;Zuschauer;Verbindungen;Schrittzeit Ø ms;Schrittzeit max ms;CPU Ø %;CPU max %;"
    "RAM MB;Empfangen Ø KB/s;Empfangen max KB/s;Gesendet Ø KB/s;Gesendet max KB/s;Lastspitze\n";

// Zahl mit Dezimalkomma für das Lastprotokoll: 3.25 -> "3,25"
std::string DecimalComma(double v, int digits) {
    std::string s = Fmt("%.*f", digits, v);
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// --- Benchmark ---
constexpr double PACKET_OVERHEAD_BYTES = 40.0;    // UDP 8 + IPv4 20 + ENet-Kopf/Befehl ~12

int64_t UnixNow() { return static_cast<int64_t>(std::time(nullptr)); }

uint32_t RandomSeed() {
    uint8_t b[4];
    crypto::RandomBytes(b, sizeof(b));
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

// ENet speichert IPv4-Adressen in "Netzwerk-Bytereihenfolge": im Speicher
// steht das erste Oktett (z. B. 192 von 192.168.1.5) zuerst.
bool IsLocalAddress(uint32_t hostNetworkOrder) {
    uint8_t b[4];
    std::memcpy(b, &hostNetworkOrder, sizeof(b));
    return b[0] == 127 ||                                // eigener Rechner
           b[0] == 10 ||                                 // privates Netz 10.0.0.0/8
           (b[0] == 172 && (b[1] & 0xF0) == 16) ||       // 172.16.0.0/12 (auch Docker)
           (b[0] == 192 && b[1] == 168);                 // 192.168.0.0/16 (Heimnetz)
}

std::string IpText(const ENetAddress& a) {
    char buf[64] = {0};
    if (enet_address_get_host_ip(&a, buf, sizeof(buf)) != 0) return "?";
    return buf;
}

// ENet-Adresse -> Lesereihenfolge wie in Ban::net (192.168.1.5 = 0xC0A80105)
uint32_t ReadableIp(uint32_t hostNetworkOrder) {
    uint8_t b[4];
    std::memcpy(b, &hostNetworkOrder, sizeof(b));
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) |
           static_cast<uint32_t>(b[3]);
}

// Dauer einer Sperre für Meldungen: "dauerhaft", "1 Tag", "7 Tage"
std::string DaysText(int days) {
    if (days <= 0) return "dauerhaft";
    return days == 1 ? std::string("1 Tag") : std::to_string(days) + " Tage";
}

// Listen von Zeitpunkten (Fehlversuche, neue Konten) je Schlüssel, z. B.
// Name oder Adresse. Die Schablone (template) funktioniert für jede
// Schlüsselart, die std::map kennt.
// Wie viele Einträge für key liegen im Zeitfenster? Ältere werden dabei entfernt.
template <typename Key>
int CountRecent(std::map<Key, std::deque<double>>& m, const Key& key, double now, double window) {
    auto it = m.find(key);
    if (it == m.end()) return 0;
    auto& dq = it->second;
    while (!dq.empty() && now - dq.front() > window) dq.pop_front();
    return static_cast<int>(dq.size());
}

// Alle Listen ausdünnen und leere Schlüssel löschen (Housekeeping, 1x pro Minute).
template <typename Key>
void PruneTimes(std::map<Key, std::deque<double>>& m, double now, double window) {
    for (auto it = m.begin(); it != m.end();) {
        auto& dq = it->second;
        while (!dq.empty() && now - dq.front() > window) dq.pop_front();
        it = dq.empty() ? m.erase(it) : std::next(it);
    }
}

// FNV-1a: schnelle Prüfsumme, um festzustellen, ob sich eine Nachricht
// gegenüber der zuletzt gesendeten geändert hat (keine Kryptografie nötig).
uint64_t Fnv1a(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : v) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string FormatDuration(double seconds) {
    const long long s = static_cast<long long>(std::max(0.0, seconds));
    return Fmt("%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
}

uint16_t ClampU16(long long v) { return static_cast<uint16_t>(std::clamp<long long>(v, 0, 65535)); }

// CPU-Zeit, die dieser Prozess bisher verbraucht hat (alle Threads, Sekunden).
// Daraus wird die CPU-Auslastung für die Webseite: Zuwachs / vergangene Zeit.
double ProcessCpuSeconds() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    auto seconds = [](const FILETIME& f) {
        ULARGE_INTEGER v;
        v.LowPart = f.dwLowDateTime;
        v.HighPart = f.dwHighDateTime;
        return static_cast<double>(v.QuadPart) / 1e7;   // Einheit: 100 ns
    };
    return seconds(kernel) + seconds(user);
#elif defined(__unix__) || defined(__APPLE__)
    // getrusage liefert dasselbe wie utime+stime aus /proc/self/stat, aber
    // ohne Text zerlegen zu müssen, und funktioniert auch unter macOS.
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    return static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) / 1e6 +
           static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
#else
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}

// Belegter Arbeitsspeicher (Linux: "VmRSS" aus /proc/self/status), sonst 0
double ProcessMemoryMB() {
#if defined(__linux__)
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream is(line.substr(6));
            double kb = 0.0;
            is >> kb;
            return kb / 1024.0;
        }
    }
#endif
    return 0.0;
}

// Ereignis der Spiellogik (Ton) -> Netzwerkereignis
net::NetEvent SoundToNet(const SoundEvent& e) {
    net::NetEvent ev;
    ev.kind = net::EventKind::Sound;
    ev.sfx = e.id;
    ev.x = e.pos.x;
    ev.y = e.pos.y;
    ev.z = e.pos.z;
    ev.volume = e.volume;
    ev.delay = e.delay;
    ev.sourceShip = e.sourceShip;
    return ev;
}

// Sortierung der Spielerliste: Runde absteigend, dann Kontostand absteigend
template <typename T>
void SortByProgress(std::vector<T>& v) {
    std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) {
        if (a.round != b.round) return a.round > b.round;
        return a.balance > b.balance;
    });
}

// Kandidat für einen Snapshot: Abstand + Index in der Weltliste
struct Candidate {
    float dist;
    size_t index;
};

// Nur die "keep" nächsten behalten. nth_element ist schneller als komplett
// sortieren: Es reicht, dass die nächsten vorne stehen.
void KeepNearest(std::vector<Candidate>& c, size_t keep) {
    if (c.size() <= keep) return;
    std::nth_element(c.begin(), c.begin() + static_cast<long>(keep), c.end(),
                     [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });
    c.resize(keep);
}

} // namespace

// ===========================================================================
// 2. Lebenszyklus
// ===========================================================================
GameServer::GameServer(const ServerConfig& config, Accounts& accounts, SharedState* shared)
    : config_(config), accounts_(accounts), shared_(shared), startTime_(std::chrono::steady_clock::now()) {}

GameServer::~GameServer() {
    if (host_) Shutdown();
    if (enetReady_) enet_deinitialize();
}

double GameServer::Now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime_).count();
}

// Zeitangabe "serverTime" in Snapshots und Welcome. Sie zählt für jeden Client
// ab seiner ersten Begrüßung bei 0 los. Warum nicht einfach die Laufzeit des
// Servers? serverTime ist eine 32-Bit-Kommazahl (float). Nach einigen Tagen
// Laufzeit wäre sie zu ungenau für die Interpolation im Client (Abstände von
// 1/15 s). Der Client rechnet seine Zeitachse deshalb aus dem ganzzahligen
// Schrittzähler (tick in Welcome und Snapshot), denn auch eine Zuschauer-
// Anzeige kann tagelang verbunden bleiben. serverTime dient nur noch der
// Kontrolle (z. B. im Lasttest).
float GameServer::ClientTime(const Client& c) const {
    return static_cast<float>(static_cast<double>(tick_ - c.timeBaseTick) * static_cast<double>(net::TICK_DT));
}

void GameServer::Log(const std::string& text) const {
    if (benchmark_) return;
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    tm = *std::localtime(&t);   // unter Windows thread-lokal und damit sicher
#else
    localtime_r(&t, &tm);       // threadsichere Variante (der Web-Thread loggt evtl. gleichzeitig)
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    // std::endl leert den Puffer: wichtig für "docker logs", sonst erscheinen
    // Meldungen erst viel später.
    std::cout << "[" << buf << "] " << text << std::endl;
}

bool GameServer::Start(std::string& error) {
    if (enet_initialize() != 0) {
        error = "ENet konnte nicht gestartet werden.";
        return false;
    }
    enetReady_ = true;

    ENetAddress address;
    address.host = ENET_HOST_ANY;   // auf allen Netzwerkkarten lauschen
    address.port = static_cast<enet_uint16>(config_.port);
    // Bandbreite 0/0 = unbegrenzt: ENet drosselt dann nicht selbst
    host_ = enet_host_create(&address, PEER_COUNT, net::CHANNEL_COUNT, 0, 0);
    if (!host_) {
        error = Fmt("UDP-Port %d kann nicht geöffnet werden (schon belegt? anderer Server läuft?).", config_.port);
        return false;
    }
    host_->maximumPacketSize = MAX_PACKET_SIZE;
    host_->maximumWaitingData = MAX_WAITING_DATA;
    // host_->duplicatePeers (ENets eigene Grenze pro Adresse) bleibt bewusst
    // unverändert: Sie gälte auch für lokale Adressen, und hinter der
    // Docker-Weiterleitung oder beim Lasttest kommen alle Clients von EINER
    // solchen Adresse. Die Grenze pro öffentlicher Adresse setzen
    // GuardHalfOpenPeers (halboffene) und OnConnect (fertige Verbindungen) um.

    InitWorld();
    lastCpuSeconds_ = ProcessCpuSeconds();
    lastStatusAt_ = Now();
    PublishStatus();   // Webseite hat sofort etwas anzuzeigen
    return true;
}

// Die Welt beim Start: gleicher Seed wie vor dem Neustart (gleiche Mondoberfläche
// und Stationen), Ausbaustufe der Stationen aus totalDelivered.
void GameServer::InitWorld() {
    settings_ = config_.settings;
    Sanitize(settings_);
    const bool haveSeed = accounts_.HasWorldSeed();
    const uint32_t seed = haveSeed ? accounts_.WorldSeed() : RandomSeed();
    world_.NewServerWorld(seed, settings_);
    world_.totalDelivered = accounts_.TotalDelivered();
    world_.lastStage = world_.StationStage();
    savedTotalDelivered_ = world_.totalDelivered;
    if (!haveSeed) {
        accounts_.SetWorldSeed(seed);
        accounts_.SaveServerData();
    }
    // Modell-Varianten: dieselbe Zuordnung wie im Client (net::ShipModel ...)
    models_ = net::DefaultModelIds();
    // Für unbekannte Namen wird der Beweis gegen einen Zufallsschlüssel geprüft.
    // So dauert die Prüfung gleich lang und verrät nicht, ob es den Namen gibt.
    crypto::RandomBytes(dummyKey_.data(), dummyKey_.size());
    UpdateMapData();
}

// Die Hauptschleife. Kein "Busy-Waiting": enet_host_service schläft im
// Betriebssystem, bis entweder ein Paket kommt oder die Wartezeit bis zum
// nächsten Schritt abgelaufen ist. Ohne Spieler braucht der Server daher fast
// keine Rechenzeit.
void GameServer::Run(const std::atomic<bool>& stop) {
    using Clock = std::chrono::steady_clock;
    const auto tickDuration = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(net::TICK_DT));
    auto next = Clock::now() + tickDuration;
    while (!stop.load()) {
        auto now = Clock::now();
        uint32_t timeoutMs = 0;
        if (now < next) {
            // Aufrunden: lieber 1 ms zu spät als in einer Schleife mit 0 ms zu kreisen
            const double ms = std::chrono::duration<double, std::milli>(next - now).count();
            timeoutMs = static_cast<uint32_t>(std::ceil(ms));
        }
        ServiceNetwork(std::min<uint32_t>(timeoutMs, 100));
        now = Clock::now();
        if (now < next) continue;

        const auto t0 = Clock::now();
        GuardHalfOpenPeers();
        Step();
        enet_host_flush(host_);   // alles sofort abschicken, nicht erst beim nächsten Warten
        tickMs_.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        if (tick_ % net::TICK_RATE == 0) PublishStatus();

        // Feste Schritte: der nächste Termin ergibt sich aus dem vorigen, nicht
        // aus "jetzt". Hängt der Rechner kurz (z. B. Speichern), holt der Server
        // die Schritte nach. Bei sehr großem Rückstand (Rechner war angehalten)
        // wird nicht nachgeholt, sonst liefe das Spiel im Zeitraffer.
        next += tickDuration;
        if (Clock::now() - next > std::chrono::milliseconds(250)) next = Clock::now() + tickDuration;
    }
}

void GameServer::Shutdown() {
    if (!host_) return;
    Log("Server wird beendet: speichere alle Spieler ...");
    // Alle Spieler wie beim Abmelden behandeln (Absturz läuft -> zählt als
    // Absturz), aber ohne die 10-s-Frist im All: Die Welt endet ja jetzt.
    UpdateDeparting(true);
    for (auto& up : clients_)
        if (up->role == Role::Player) LogoutPlayer(*up, "Server wird beendet", false, false);
    accounts_.SetTotalDelivered(world_.totalDelivered);
    accounts_.SaveAccounts();
    accounts_.SaveHighscores();
    accounts_.SaveServerData();
    if (load_.samples > 0) FinishLoadMinute();   // angefangene Minute ins Lastprotokoll
    const bool saved = accounts_.Flush();   // warten, bis wirklich alles auf der Platte ist

    // Allen Clients Bescheid sagen und die Verbindung ordentlich beenden.
    // disconnect_later: ENet schickt erst alle wartenden Pakete (unsere
    // Fehlermeldung), dann die Trennung.
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.closing || !c.peer) continue;
        SendError(c, net::ErrorCode::ServerShutdown);
        c.closing = true;
        enet_peer_disconnect_later(c.peer, 0);
    }
    enet_host_flush(host_);

    // Kurz warten (höchstens 1,5 s; Docker wartet 10 s), damit verlorene Pakete
    // wiederholt werden können und die Clients die Trennung bestätigen.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (!clients_.empty() && std::chrono::steady_clock::now() < deadline) {
        ENetEvent ev;
        if (enet_host_service(host_, &ev, 50) <= 0) continue;
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            if (Client* c = FindClient(ev.peer)) RemoveClient(*c);
        } else if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            enet_peer_reset(ev.peer);   // neue Verbindung während des Beendens: sofort verwerfen
        }
    }
    for (auto& up : clients_)
        if (up->peer) {
            up->peer->data = nullptr;
            enet_peer_reset(up->peer);
        }
    clients_.clear();
    enet_host_destroy(host_);
    host_ = nullptr;
    Log(saved ? "Server beendet. Alle Daten gespeichert." : "Server beendet. ACHTUNG: Nicht alle Daten konnten gespeichert werden (siehe Meldungen oben).");
}

// ===========================================================================
// 3. Ein Simulationsschritt
// ===========================================================================
void GameServer::Step() {
    // Einstellungen von der Webseite immer zu Beginn eines Schritts anwenden
    // (docs/Mehrspieler.md Abschnitt 12), nie mitten in der Simulation.
    ApplySettingsRequest();
    ApplyShipsRequest();
    ApplyMapCommands();
    ApplyBanCommands();

    for (auto& up : clients_)
        if (up->isBot) BotThink(*up);
    ApplyInputs();

    const int stageBefore = world_.lastStage;
    world_.Update(net::TICK_DT);
    ++tick_;
    UpdateDeparting(false);

    // world.time ist ein float und wächst endlos. Nach ~12 Tagen wäre er so
    // groß, dass "+ 1/30 s" nichts mehr ändert. Er wird nur für das leichte
    // Auf und Ab der Schiffe benutzt (sin(time * 2 + id), Periode PI). Also
    // gelegentlich ein Vielfaches der Periode abziehen: sieht niemand.
    if (world_.time > 20000.0f) world_.time -= 6000.0f * PI;

    DistributeWorldEvents(stageBefore);
    SendEvents();
    SendSnapshots();
    SendDepositUpdates();

    if (tick_ % PLAYERS_EVERY == 0) SendToAllInGame(net::Encode(MakePlayers()));
    if (highscoresChanged_) {
        SendToAllInGame(net::Encode(MakeHighscores()));
        highscoresChanged_ = false;
    }
    if (tick_ % SERVERINFO_EVERY == 0) {
        const auto info = net::Encode(MakeServerInfo());
        for (auto& up : clients_)
            if (up->role == Role::Lobby && !up->closing) Send(*up, info, true);
    }
    if (tick_ % SAVE_EVERY == 0) SaveAll();
    if (tick_ % net::TICK_RATE == 0) Housekeeping();
}

// Pro Spieler und Schritt GENAU EINE Eingabe aus der Warteschlange verbrauchen
// (docs/Mehrspieler.md Abschnitt 4). Warum eine Warteschlange und nicht einfach "die neueste"?
// Der Client sagt sein Schiff voraus, indem er jede Eingabe genau einen
// Schritt lang anwendet. Wendet der Server dieselben Eingaben in derselben
// Reihenfolge an, landet das Schiff dort, wo der Client es erwartet. Kommen
// Pakete gebündelt an (Netz-"Ruckler"), gleicht die Schlange das aus.
//
// Die Schlange darf aber nicht dauerhaft lang bleiben: Nach einem Ruckler
// kommen die zurückgehaltenen Pakete auf einmal an, und weil Client und
// Server beide genau 30 Eingaben pro Sekunde erzeugen bzw. verbrauchen,
// bliebe dieser Vorrat für immer liegen. Jede Eingabe käme dann z. B. 150 ms
// später zur Wirkung (Schüsse, Andocken, die Position, die andere sehen).
// Deshalb wird die Länge geregelt: Lagen eine Sekunde lang IMMER mehr als
// INPUT_QUEUE_TARGET Eingaben bereit, wird die älteste übersprungen. Höchstens
// eine pro Sekunde, damit es sanft geht. Tastendrücke gehen dabei nicht
// verloren (Zähler, siehe unten), ein Schuss wird an die nächste Eingabe
// weitergegeben. Der Client merkt nur eine kleine Korrektur beim Abgleich
// (ackSeq springt um zwei statt um eins).
void GameServer::ApplyInputs() {
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.role != Role::Player) continue;
        PlayerState* ps = world_.FindPlayer(c.playerId);
        if (!ps) continue;
        c.queueMinInWindow = std::min(c.queueMinInWindow, static_cast<int>(c.inputs.size()));
        if (++c.queueWindowSteps >= INPUT_QUEUE_WINDOW) {
            if (c.queueMinInWindow > INPUT_QUEUE_TARGET && c.inputs.size() >= 2) {
                const bool fire = c.inputs.front().fire;
                c.inputs.pop_front();
                if (fire) c.inputs.front().fire = true;
            }
            c.queueMinInWindow = 1 << 30;
            c.queueWindowSteps = 0;
        }
        ShipInput in;
        if (!c.inputs.empty()) {
            const net::InputRecord rec = c.inputs.front();
            c.inputs.pop_front();
            // Tastendrücke als Zähler: Der Unterschied zum zuletzt angewendeten
            // Stand sagt, ob seither gedrückt wurde. uint8_t rechnet "modulo 256",
            // daher klappt das auch beim Überlauf 255 -> 0. Werte >= 128 wären ein
            // Rückwärtsschritt (vertauschte Pakete) und zählen nicht.
            if (!c.countersKnown) {
                // Erste Eingabe dieser Sitzung: Zählerstand übernehmen, ohne
                // daraus einen Tastendruck abzuleiten.
                c.lastScanPresses = rec.scanPresses;
                c.lastInteractPresses = rec.interactPresses;
                c.countersKnown = true;
            }
            const uint8_t ds = static_cast<uint8_t>(rec.scanPresses - c.lastScanPresses);
            const uint8_t di = static_cast<uint8_t>(rec.interactPresses - c.lastInteractPresses);
            in.scanToggle = ds >= 1 && ds < 128;
            in.interact = di >= 1 && di < 128;
            c.lastScanPresses = rec.scanPresses;
            c.lastInteractPresses = rec.interactPresses;
            c.held = rec;
            c.ackSeq = rec.seq;
        }
        // Leere Schlange: die zuletzt gehaltenen Tasten weiter verwenden, aber
        // ohne Tastendrücke (sonst würde ein Druck mehrfach wirken).
        in.thrust = std::clamp(c.held.thrust, -1.0f, 1.0f);
        in.turn = std::clamp(c.held.turn, -1.0f, 1.0f);
        in.fire = c.held.fire;
        if (tick_ < c.noInteractUntilTick) in.interact = false;
        ps->input = in;
        if (Ship* s = world_.PlayerShip(*ps)) s->chatting = c.chatting;
    }
}

// Nach World::Update liegen Ereignisse in world.sounds/fx/goals und in den
// "outbox"-Listen der Spieler. Hier wird entschieden, wer was bekommt, und
// alles in die Ereignisliste des jeweiligen Clients gelegt. Danach werden die
// Listen der Welt geleert (sonst würden sie beim nächsten Schritt doppelt
// verschickt).
void GameServer::DistributeWorldEvents(int stageBefore) {
    struct View {
        Client* c;
        PlayerState* ps;
        Vector3 center;   // Mittelpunkt der Hör-/Sichtweite
        int shipId;       // eigenes Schiff oder -1
    };
    std::vector<View> players;
    std::vector<Client*> spectators;
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.closing) continue;
        if (c.role == Role::Spectator) {
            spectators.push_back(&c);
        } else if (c.role == Role::Player) {
            PlayerState* ps = world_.FindPlayer(c.playerId);
            if (!ps) continue;
            const Ship* s = world_.PlayerShip(*ps);
            if (s) c.lastPos = s->pos;   // ohne Schiff (Respawn): letzte bekannte Position
            players.push_back({&c, ps, c.lastPos, s ? s->id : -1});
        }
    }

    // --- Töne: nur für Spieler; Zuschauer hören nichts (docs/Mehrspieler.md Abschnitt 9) ---
    for (const SoundEvent& e : world_.sounds) {
        const net::NetEvent ev = SoundToNet(e);
        for (View& v : players) {
            bool send;
            if (e.forPlayer >= 0) {
                send = e.forPlayer == v.ps->id;
            } else {
                // Eigene Schüsse sagt der Client selbst voraus (sofortiger Ton
                // beim Drücken). Würde der Server sie auch schicken, hörte man
                // jeden Schuss doppelt und verspätet.
                const bool ownShot = e.id == Sfx::PlayerShot && v.shipId >= 0 && e.sourceShip == v.shipId;
                send = !ownShot && Dist2D(e.pos, v.center) <= net::AOI_EVENTS;
            }
            if (send) v.c->events.push_back(ev);
        }
    }

    // --- Sichtbare Effekte (Explosionen, Funken) ---
    for (const FxEvent& f : world_.fx) {
        net::NetEvent ev;
        ev.kind = f.type == FxEvent::Type::Explosion ? net::EventKind::Explosion : net::EventKind::Sparks;
        ev.x = f.pos.x;
        ev.y = f.pos.y;
        ev.z = f.pos.z;
        ev.size = f.size;
        ev.color = f.color;
        ev.count = static_cast<uint8_t>(std::clamp(f.count, 0, 255));
        for (View& v : players)
            if (Dist2D(f.pos, v.center) <= net::AOI_EVENTS) v.c->events.push_back(ev);
        // Zuschauer sehen die ganze Karte, aber nur Explosionen (Funken sind
        // aus der Vogelperspektive zu klein und würden nur Daten kosten)
        if (ev.kind == net::EventKind::Explosion)
            for (Client* s : spectators) s->events.push_back(ev);
    }

    // --- Meldungen ("Lagerstätte entdeckt!" ...) nur an den eigenen Spieler ---
    for (View& v : players) {
        for (const Notice& n : v.ps->outbox) {
            net::NetEvent ev;
            ev.kind = net::EventKind::Notice;
            ev.text = n.text;
            ev.color = n.color;
            v.c->events.push_back(ev);
        }
    }
    for (PlayerState& ps : world_.players) ps.outbox.clear();

    // --- Ziel erreicht: Bestenliste + Nachricht an ALLE ---
    bool anyGoal = false;
    for (const GoalEvent& g : world_.goals) {
        const PlayerState* ps = world_.FindPlayer(g.playerId);
        if (!ps) continue;
        HighscoreRecord rec;
        rec.name = ps->name;
        rec.seconds = g.seconds;
        rec.round = g.round;
        rec.when = UnixNow();
        accounts_.AddHighscore(rec);
        highscoresChanged_ = true;
        anyGoal = true;

        net::NetEvent ev;
        ev.kind = net::EventKind::Goal;
        ev.playerId = g.playerId;
        ev.text = ps->name;
        ev.seconds = g.seconds;
        ev.round = ClampU16(g.round);
        for (View& v : players) v.c->events.push_back(ev);
        for (Client* s : spectators) s->events.push_back(ev);
        for (View& v : players)
            if (v.ps->id == g.playerId) StoreProgress(*v.c);
        Log(Fmt("ZIEL ERREICHT: %s in %s (Runde %d)", ps->name.c_str(), FormatDuration(g.seconds).c_str(), g.round));
    }

    // --- Handelsstationen sind gewachsen (alle sehen es) ---
    if (world_.lastStage > stageBefore) {
        net::NetEvent ev;
        ev.kind = net::EventKind::StationGrow;
        ev.stage = static_cast<uint8_t>(std::clamp(world_.lastStage, 0, 255));
        for (View& v : players) v.c->events.push_back(ev);
        for (Client* s : spectators) s->events.push_back(ev);
        Log(Fmt("Die Handelsstationen wachsen: Ausbaustufe %d", world_.lastStage));
    }

    // --- Neues Schiff / Absturz je Spieler ---
    bool anyCrash = false;
    for (View& v : players) {
        Client& c = *v.c;
        // Wartete der Spieler auf ein Schiff (shipId -1) und hat jetzt eins?
        if (c.lastShipId < 0 && v.ps->shipId >= 0) {
            net::NetEvent ev;
            ev.kind = net::EventKind::Respawned;
            c.events.push_back(ev);
        }
        c.lastShipId = v.ps->shipId;
        // World::PlayerShipLost zählt deaths hoch, sobald das Schiff aufschlägt;
        // der Fortschritt ist dann schon zurückgesetzt -> gleich speichern.
        if (v.ps->deaths != c.lastDeaths) {
            c.lastDeaths = v.ps->deaths;
            StoreProgress(c);
            anyCrash = true;
            Log(c.name + " ist abgestürzt (Fortschritt beginnt von vorn)");
        }
    }
    if (anyGoal) accounts_.SaveHighscores();
    if (anyGoal || anyCrash) accounts_.SaveAccounts();

    world_.sounds.clear();
    world_.fx.clear();
    world_.goals.clear();
}

void GameServer::SendEvents() {
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.events.empty()) continue;
        if (c.closing || c.role == Role::Lobby) {
            c.events.clear();
            continue;
        }
        if (c.events.size() > MAX_EVENTS_PER_MSG) {
            // Sehr unwahrscheinlich, aber möglich (Riesenschlacht): wichtige
            // Ereignisse (Meldungen, Ziel, Respawn) nach vorne, Töne/Effekte kürzen
            std::stable_partition(c.events.begin(), c.events.end(), [](const net::NetEvent& e) {
                return e.kind != net::EventKind::Sound && e.kind != net::EventKind::Sparks &&
                       e.kind != net::EventKind::Explosion;
            });
            c.events.resize(MAX_EVENTS_PER_MSG);
        }
        net::EventsMsg m;
        m.events.swap(c.events);
        const std::vector<uint8_t> bytes = net::Encode(m);
        c.eventBytes += bytes.size();
        Send(c, bytes, true);
        c.events.clear();
    }
}

void GameServer::SendSnapshots() {
    const bool playerTick = tick_ % net::SNAPSHOT_EVERY == 0;
    const bool spectatorTick = tick_ % net::SPECTATOR_SNAPSHOT_EVERY == 0;
    if (!playerTick && !spectatorTick) return;
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.closing) continue;
        const bool due = (c.role == Role::Player && playerTick) || (c.role == Role::Spectator && spectatorTick);
        if (!due) continue;
        const std::vector<uint8_t> bytes = net::Encode(MakeSnapshot(c));
        c.snapshotBytes += bytes.size();
        ++c.snapshotCount;
        Send(c, bytes, false);   // unzuverlässig: ein verlorener Snapshot ist egal
    }
}

// Lagerstätten nur schicken, wenn sich für DIESEN Empfänger etwas geändert hat.
// Zwei Stufen: (1) Zähler der Welt/des Spielers haben sich bewegt? Erst dann
// lohnt es, die Liste zu bauen. (2) Prüfsumme der fertigen Nachricht anders
// als beim letzten Mal? Baut z. B. ein anderer Spieler eine Lagerstätte ab,
// die ich nie entdeckt habe, ändert sich für mich nichts -> nichts senden.
void GameServer::SendDepositUpdates() {
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.closing || c.role == Role::Lobby) continue;
        const bool player = c.role == Role::Player;
        const PlayerState* ps = player ? world_.FindPlayer(c.playerId) : nullptr;
        if (player && !ps) continue;
        const int reveal = ps ? ps->revealRevision : 0;
        if (c.depositsSent && c.sentDepositRevision == world_.depositRevision && c.sentRevealRevision == reveal) continue;
        const uint64_t minTicks = player ? DEPOSITS_MIN_TICKS_PLAYER : DEPOSITS_MIN_TICKS_SPECTATOR;
        if (c.depositsSent && tick_ - c.lastDepositsTick < minTicks) continue;   // gleich, aber noch nicht jetzt

        const std::vector<uint8_t> bytes = net::Encode(MakeDeposits(c));
        c.sentDepositRevision = world_.depositRevision;
        c.sentRevealRevision = reveal;
        const uint64_t hash = Fnv1a(bytes);
        if (c.depositsSent && hash == c.sentDepositHash) continue;
        Send(c, bytes, true);
        c.depositsSent = true;
        c.sentDepositHash = hash;
        c.lastDepositsTick = tick_;
    }
}

// Einmal pro Sekunde: Flutbremse zurücksetzen, Lobby-Zeitüberschreitung,
// alte Fehlversuche vergessen.
void GameServer::Housekeeping() {
    const double now = Now();
    for (auto& up : clients_) {
        Client& c = *up;
        c.msgsThisSecond = 0;
        if (c.role == Role::Lobby && !c.closing && c.peer && now - c.connectedAt > LOBBY_TIMEOUT) {
            SendError(c, net::ErrorCode::BadRequest, "Zu lange ohne Anmeldung – Verbindung getrennt.");
            Log(Fmt("Verbindung #%u (%s): Zeitüberschreitung in der Lobby", c.connId, c.ipText.c_str()));
            Disconnect(c);
        }
    }
    if (tick_ % (60 * net::TICK_RATE) == 0) {
        PruneTimes(failuresByNameIp_, now, LOGIN_FAILURE_WINDOW);
        PruneTimes(failuresByName_, now, LOGIN_FAILURE_WINDOW);
        PruneTimes(failuresByIp_, now, LOGIN_FAILURE_WINDOW);
        PruneTimes(registrations_, now, REGISTRATION_WINDOW);
        for (auto it = bannedLogAt_.begin(); it != bannedLogAt_.end();)
            it = now - it->second >= 60.0 ? bannedLogAt_.erase(it) : std::next(it);
        ExpireBans();

        // Für den Betreiber: Hinweise auf einen Angriff auf die Verbindungsplätze
        // (höchstens eine Meldung pro Minute, nicht eine pro Paket).
        if (host_) {
            size_t used = 0;
            for (size_t i = 0; i < host_->peerCount; ++i)
                if (host_->peers[i].state != ENET_PEER_STATE_DISCONNECTED) ++used;
            if (used * 10 >= host_->peerCount * 9)
                Log(Fmt("Warnung: %zu von %zu Verbindungsplätzen belegt (Angriff mit halboffenen Verbindungen?)", used,
                        host_->peerCount));
        }
        if (halfOpenDropped_ > 0) {
            Log(Fmt("%llu halboffene Verbindung(en) in der letzten Minute verworfen (zu viele von einer Adresse)",
                    static_cast<unsigned long long>(halfOpenDropped_)));
            halfOpenDropped_ = 0;
        }
    }
}

// ===========================================================================
// 4. Netzwerk
// ===========================================================================
void GameServer::ServiceNetwork(uint32_t timeoutMs) {
    ENetEvent ev;
    // Wartet höchstens timeoutMs auf das erste Ereignis. Weitere, die schon
    // angekommen sind, holt enet_host_check_events ohne erneutes Warten ab.
    int r = enet_host_service(host_, &ev, timeoutMs);
    if (r < 0) {
        // Fehler am Socket (sehr selten, z. B. Netzwerkkarte weg). Nicht in
        // einer Schleife mit voller CPU-Last immer wieder versuchen, sondern
        // die Wartezeit abwarten und höchstens einmal pro Minute melden.
        static double lastReport = -1e9;
        if (Now() - lastReport > 60.0) {
            Log("ENet-Fehler beim Empfangen (Netzwerk gestört?)");
            lastReport = Now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<uint32_t>(1, timeoutMs)));
        return;
    }
    int handled = 0;
    while (r > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                OnConnect(ev.peer, ev.data);
                break;
            case ENET_EVENT_TYPE_RECEIVE: {
                Client* c = FindClient(ev.peer);
                if (c && !c->closing) OnReceive(*c, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);   // Paket gehört uns: immer freigeben
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                if (Client* c = FindClient(ev.peer)) OnDisconnect(*c);
                break;
            default:
                break;
        }
        // Bei einer Paketflut nicht ewig hier hängen: der nächste Schritt hat Vorrang
        if (++handled >= 4096) break;
        r = enet_host_check_events(host_, &ev);
    }
}

// Halboffene Verbindungen. ENet legt einen Peer schon an, wenn
// das erste Verbindungspaket kommt (Zustand ACKNOWLEDGING_CONNECT). Das
// CONNECT-Ereignis (OnConnect) kommt erst, wenn der Client den Handschlag
// bestätigt hat; bis dahin weiß der Server nichts von diesem Peer. Wer nur
// Verbindungspakete schickt und nie antwortet, würde mit ENets Standardwerten
// jeden Platz 30 s lang belegen. Darum hier vor jedem Schritt:
//   1. Pro öffentlicher Adresse höchstens maxConnectionsPerIp Peers (fertige
//      und halboffene zusammen); überzählige halboffene werden sofort
//      verworfen. Lokale Adressen sind wie in OnConnect ausgenommen.
//   2. Jeder übrige halboffene Peer bekommt eine kurze Zeitgrenze: Nach
//      höchstens 3 s ohne Antwort gibt ENet ihn auf.
// Grenze des Schutzes: Bei UDP kann ein Angreifer die Absenderadresse
// fälschen, jedes Paket mit einer anderen. Dagegen hilft Punkt 1 nicht, nur
// Punkt 2: Um alle 1024 Plätze dauerhaft zu belegen, bräuchte es dann ständig
// über 300 gefälschte Pakete pro Sekunde. Vollständig ließe sich das nur mit
// einem "Cookie" im Handschlag (Änderung an ENet) oder einem VPN verhindern
// (server/README.md).
void GameServer::GuardHalfOpenPeers() {
    if (!host_) return;
    const bool limitPerIp = config_.maxConnectionsPerIp > 0;
    std::map<uint32_t, int> perIp;   // öffentliche Adresse -> belegte Plätze
    if (limitPerIp) {
        // Zuerst die fertigen (auch die gerade schließenden) Verbindungen zählen
        for (size_t i = 0; i < host_->peerCount; ++i) {
            const ENetPeer& p = host_->peers[i];
            if (p.state >= ENET_PEER_STATE_CONNECTION_SUCCEEDED && !IsLocalAddress(p.address.host)) ++perIp[p.address.host];
        }
    }
    for (size_t i = 0; i < host_->peerCount; ++i) {
        ENetPeer* p = &host_->peers[i];
        if (p->state != ENET_PEER_STATE_ACKNOWLEDGING_CONNECT) continue;
        if (limitPerIp && !IsLocalAddress(p->address.host) && ++perIp[p->address.host] > config_.maxConnectionsPerIp) {
            // Platz sofort freigeben. Der Absender erfährt nichts davon; ein
            // ehrlicher Client würde es nach einigen Sekunden erneut versuchen.
            enet_peer_reset(p);
            ++halfOpenDropped_;
            continue;
        }
        enet_peer_timeout(p, 32, HALF_OPEN_TIMEOUT_MIN_MS, HALF_OPEN_TIMEOUT_MAX_MS);
    }
}

GameServer::Client* GameServer::FindClient(ENetPeer* peer) {
    return peer ? static_cast<Client*>(peer->data) : nullptr;
}

void GameServer::RemoveClient(Client& c) {
    if (c.peer) c.peer->data = nullptr;
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [&](const std::unique_ptr<Client>& p) { return p.get() == &c; }),
                   clients_.end());
}

void GameServer::OnConnect(ENetPeer* peer, uint32_t data) {
    // Zeitüberschreitung: nach 5..15 s ohne Antwort gilt der
    // Client als weg; ENet meldet dann DISCONNECT.
    enet_peer_timeout(peer, 32, 5000, 15000);

    auto up = std::make_unique<Client>();
    Client& c = *up;
    c.connId = nextConnId_++;
    c.peer = peer;
    c.ip = peer->address.host;
    c.ipText = IpText(peer->address);
    c.connectedAt = Now();
    peer->data = &c;   // so finden wir bei jedem Paket den Client wieder
    clients_.push_back(std::move(up));

    // Der Client schickt seine Protokollversion als "Verbindungsdaten" mit.
    if (data != net::PROTOCOL_VERSION) {
        SendError(c, net::ErrorCode::VersionMismatch,
                  Fmt("Server: Protokoll %u, Spiel: Protokoll %u", static_cast<unsigned>(net::PROTOCOL_VERSION),
                      static_cast<unsigned>(data)));
        Log(Fmt("Verbindung #%u (%s) abgelehnt: falsche Protokollversion %u", c.connId, c.ipText.c_str(),
                static_cast<unsigned>(data)));
        Disconnect(c);
        return;
    }
    if (const Ban* b = FindIpBan(c.ip)) {
        SendError(c, net::ErrorCode::Banned, BanText(*b));
        // Höchstens eine Meldung pro Minute und Adresse: Wer es ständig neu
        // versucht, würde sonst das Log füllen.
        const double now = Now();
        auto it = bannedLogAt_.find(c.ip);
        if (it == bannedLogAt_.end() || now - it->second >= 60.0) {
            Log(Fmt("Verbindung #%u (%s) abgelehnt: Adresse gesperrt (%s)", c.connId, c.ipText.c_str(), b->target.c_str()));
            bannedLogAt_[c.ip] = now;
        }
        Disconnect(c);
        return;
    }
    if (config_.maxConnectionsPerIp > 0 && !IsLocalAddress(c.ip)) {
        // Auch schließende Verbindungen zählen mit: Sie belegen ihren Platz
        // noch einige Sekunden. Sonst könnte man durch ständiges Neuverbinden
        // trotz Ablehnung beliebig viele Plätze belegen.
        int same = 0;
        for (const auto& o : clients_)
            if (o->ip == c.ip) ++same;
        if (same > config_.maxConnectionsPerIp) {
            SendError(c, net::ErrorCode::ServerFull, "Zu viele Verbindungen von deiner Adresse.");
            Log(Fmt("Verbindung #%u (%s) abgelehnt: mehr als %d Verbindungen von dieser Adresse", c.connId,
                    c.ipText.c_str(), config_.maxConnectionsPerIp));
            Disconnect(c);
            return;
        }
    }
    // Wartebereich (Lobby) begrenzen: Spieler und Zuschauer haben eigene
    // Grenzen, die Lobby sonst nur die Zahl der ENet-Plätze.
    int lobby = 0;
    for (const auto& o : clients_)
        if (o->role == Role::Lobby && !o->closing) ++lobby;
    if (lobby > MAX_LOBBY_CONNECTIONS) {
        SendError(c, net::ErrorCode::ServerFull, "Der Server ist gerade ausgelastet. Bitte gleich noch einmal versuchen.");
        Log(Fmt("Verbindung #%u (%s) abgelehnt: schon %d Verbindungen ohne Anmeldung", c.connId, c.ipText.c_str(),
                MAX_LOBBY_CONNECTIONS));
        Disconnect(c);
        return;
    }
    Send(c, net::Encode(MakeServerInfo()), true);
    Log(Fmt("Verbindung #%u von %s (%d verbunden)", c.connId, c.ipText.c_str(), static_cast<int>(clients_.size())));
}

void GameServer::OnDisconnect(Client& c) {
    if (c.role == Role::Player) LogoutPlayer(c, "Verbindung getrennt", true);
    else if (c.role == Role::Spectator) Log(Fmt("Zuschauer #%u (%s) hat die Verbindung beendet", c.connId, c.ipText.c_str()));
    RemoveClient(c);   // c ist danach ungültig!
}

void GameServer::Disconnect(Client& c) {
    if (c.closing) return;
    // Ein Spieler verschwindet sofort aus der Welt (und wird gespeichert),
    // nicht erst, wenn ENet die Trennung bestätigt hat.
    if (c.role == Role::Player) LogoutPlayer(c, "vom Server getrennt", true);
    c.role = Role::Lobby;
    c.closing = true;
    if (c.peer) enet_peer_disconnect_later(c.peer, 0);
}

void GameServer::BadPacket(Client& c, const char* what) {
    if (++c.badPackets == MAX_BAD_PACKETS) {
        Log(Fmt("Verbindung #%u (%s): zu viele ungültige Nachrichten (%s) – getrennt", c.connId, c.ipText.c_str(), what));
        SendError(c, net::ErrorCode::BadRequest);
        Disconnect(c);
    }
}

void GameServer::OnReceive(Client& c, const uint8_t* data, size_t len) {
    // Flutbremse: mehr als 240 Nachrichten pro Sekunde braucht kein ehrlicher
    // Client (30 Eingaben/s). Überzählige werden einfach weggeworfen.
    if (++c.msgsThisSecond > MAX_MSGS_PER_SECOND) {
        if (c.msgsThisSecond == FLOOD_DISCONNECT) {
            Log(Fmt("Verbindung #%u (%s): Nachrichtenflut – getrennt", c.connId, c.ipText.c_str()));
            Disconnect(c);
        }
        return;
    }
    if (len == 0) {
        BadPacket(c, "leer");
        return;
    }
    net::Reader r(data, len);
    const auto type = static_cast<net::MsgType>(r.U8());
    switch (type) {
        case net::MsgType::Spectate:
            HandleSpectate(c);
            break;
        case net::MsgType::Register: {
            net::RegisterMsg m;
            if (!net::Decode(r, m)) BadPacket(c, "Register");
            else HandleRegister(c, m);
            break;
        }
        case net::MsgType::LoginStart: {
            net::LoginStartMsg m;
            if (!net::Decode(r, m)) BadPacket(c, "LoginStart");
            else HandleLoginStart(c, m);
            break;
        }
        case net::MsgType::LoginProof: {
            net::LoginProofMsg m;
            if (!net::Decode(r, m)) BadPacket(c, "LoginProof");
            else HandleLoginProof(c, m);
            break;
        }
        case net::MsgType::MapUpload: {
            net::MapUploadMsg m;
            if (!net::Decode(r, m)) BadPacket(c, "MapUpload");
            else HandleMapUpload(c, m);
            break;
        }
        case net::MsgType::Input: {
            net::InputMsg m;
            if (!net::Decode(r, m)) BadPacket(c, "Input");
            else HandleInput(c, m);
            break;
        }
        case net::MsgType::DockAction: {
            net::DockActionMsg m;
            if (!net::Decode(r, m)) BadPacket(c, "DockAction");
            else HandleDockAction(c, m);
            break;
        }
        case net::MsgType::Chat: {
            net::ChatMsg m;
            if (!net::Decode(r, m)) BadPacket(c, "Chat");
            else HandleChat(c, m);
            break;
        }
        case net::MsgType::ChatTyping: {
            net::ChatTypingMsg m;
            if (!net::Decode(r, m)) BadPacket(c, "ChatTyping");
            else HandleChatTyping(c, m);
            break;
        }
        case net::MsgType::Leave:
            HandleLeave(c);
            break;
        default:
            BadPacket(c, "unbekannter Typ");   // auch Server->Client-Typen von einem Client
            break;
    }
}

void GameServer::HandleSpectate(Client& c) {
    if (c.role != Role::Lobby) return;
    if (SpectatorsOnline() >= limits::MAX_SPECTATORS) {
        SendError(c, net::ErrorCode::SpectatorsFull);   // Verbindung bleibt, Client bleibt in der Lobby
        return;
    }
    c.role = Role::Spectator;
    c.timeBaseTick = tick_;
    c.challengePending = false;
    c.depositsSent = false;
    SendWelcome(c);
    SendDepositsNow(c);
    Send(c, net::Encode(MakePlayers()), true);
    Send(c, net::Encode(MakeHighscores()), true);
    Log(Fmt("Zuschauer #%u (%s) schaut zu (%d/%d Zuschauer)", c.connId, c.ipText.c_str(), SpectatorsOnline(),
            limits::MAX_SPECTATORS));
}

void GameServer::HandleRegister(Client& c, const net::RegisterMsg& m) {
    if (c.role != Role::Lobby) return;
    if (!settings_.allowRegistration) {
        SendError(c, net::ErrorCode::RegistrationClosed);
        return;
    }
    if (!net::ValidPlayerName(m.name)) {
        SendError(c, net::ErrorCode::NameInvalid);
        return;
    }
    if (accounts_.Find(m.name)) {
        SendError(c, net::ErrorCode::NameTaken);
        return;
    }
    if (PlayersOnline() >= settings_.maxPlayers) {
        SendError(c, net::ErrorCode::ServerFull);
        return;
    }
    const double now = Now();
    if (!RegistrationAllowed(c.ip, now)) {
        SendError(c, net::ErrorCode::TooManyAttempts, "Zu viele neue Konten von deiner Adresse. Bitte später erneut versuchen.");
        Log(Fmt("Verbindung #%u (%s): Kontoanlage abgelehnt (zu viele neue Konten von dieser Adresse)", c.connId,
                c.ipText.c_str()));
        return;
    }
    Account* acc = accounts_.Create(m.name, m.key, UnixNow());
    if (!acc) {
        SendError(c, net::ErrorCode::ServerFull, "Auf diesem Server können keine weiteren Konten angelegt werden.");
        Log("Kontoanlage abgelehnt: Obergrenze von " + std::to_string(Accounts::MAX_ACCOUNTS) + " Konten erreicht");
        return;
    }
    RecordRegistration(c.ip, now);
    accounts_.SaveAccounts();
    Log(Fmt("Neues Konto: %s (Verbindung #%u, %s)", acc->name.c_str(), c.connId, c.ipText.c_str()));
    // nach dem Anlegen direkt angemeldet (kein zweiter Anmeldeschritt)
    LoginPlayer(c, acc, acc->name);
}

void GameServer::HandleLoginStart(Client& c, const net::LoginStartMsg& m) {
    if (c.role != Role::Lobby) return;
    // Immer eine Herausforderung schicken, auch für unbekannte Namen: sonst
    // könnte man durch Ausprobieren herausfinden, welche Namen es gibt.
    // Ein neues LoginStart ersetzt eine ältere, unbenutzte Nonce.
    crypto::RandomBytes(c.nonce.data(), c.nonce.size());
    c.challengePending = true;
    c.challengeName = m.name;
    net::LoginChallengeMsg ch;
    ch.nonce = c.nonce;
    Send(c, net::Encode(ch), true);
}

void GameServer::FailLogin(Client& c, net::ErrorCode code, const std::string& text) {
    if (++c.failedLogins >= MAX_LOGIN_FAILURES_PER_CONNECTION) {
        SendError(c, net::ErrorCode::TooManyAttempts);
        Log(Fmt("Verbindung #%u (%s): %d Fehlversuche bei der Anmeldung – getrennt", c.connId, c.ipText.c_str(),
                c.failedLogins));
        Disconnect(c);
        return;
    }
    SendError(c, code, text);
}

Account* GameServer::CheckProof(Client& c, const net::Key32& proof, const char* what) {
    if (!c.challengePending) {
        // Beweis ohne vorherige Herausforderung (oder Nonce schon verbraucht)
        SendError(c, net::ErrorCode::BadRequest);
        BadPacket(c, what);
        return nullptr;
    }
    c.challengePending = false;   // eine Nonce gilt nur für EINEN Versuch
    const std::string lower = ToLowerAscii(c.challengeName);
    const bool validName = net::ValidPlayerName(c.challengeName);
    const double now = Now();

    if (LoginBlocked(lower, c.ip, now)) {
        // Zu viele Fehlversuche in den letzten 10 Minuten (siehe LoginBlocked).
        // Den Beweis gar nicht erst prüfen.
        FailLogin(c, net::ErrorCode::TooManyAttempts);
        return nullptr;
    }

    Account* acc = validName ? accounts_.Find(lower) : nullptr;
    // Für unbekannte Namen gegen einen Zufallsschlüssel rechnen: gleicher
    // Rechenweg, gleiche Antwort ("Name oder Passwort falsch").
    const net::Key32& key = acc ? acc->key : dummyKey_;
    const crypto::Digest expected = crypto::LoginProof(key, c.nonce, lower);
    const bool ok = crypto::ConstantTimeEqual(expected.data(), proof.data(), expected.size()) && acc != nullptr;
    if (!ok) {
        RecordLoginFailure(lower, acc != nullptr, c, now);
        FailLogin(c, net::ErrorCode::WrongLogin);
        return nullptr;
    }
    // Gesperrt? Erst nach dem richtigen Passwort prüfen: So erfährt nur der
    // Besitzer von der Sperre, nicht jeder, der den Namen ausprobiert.
    // Zählt wie ein Fehlversuch (höchstens 5 pro Verbindung, dann Trennung).
    if (const Ban* b = FindAccountBan(acc->name)) {
        Log(Fmt("Verbindung #%u (%s): Anmeldung als %s abgelehnt (Konto gesperrt)", c.connId, c.ipText.c_str(),
                acc->name.c_str()));
        FailLogin(c, net::ErrorCode::Banned, BanText(*b));
        return nullptr;
    }
    return acc;
}

void GameServer::HandleLoginProof(Client& c, const net::LoginProofMsg& m) {
    if (c.role != Role::Lobby) return;
    Account* acc = CheckProof(c, m.proof, "LoginProof ohne LoginStart");
    if (!acc) return;
    // Richtiges Passwort. Jetzt erst prüfen, ob der Spieler schon online ist
    // (z. B. alte Verbindung noch nicht abgelaufen) oder der Server voll ist.
    for (const auto& o : clients_) {
        if (o->role == Role::Player && o->account == acc) {
            SendError(c, net::ErrorCode::AlreadyOnline);
            return;
        }
    }
    if (PlayersOnline() >= settings_.maxPlayers) {
        SendError(c, net::ErrorCode::ServerFull);
        return;
    }
    // Ist das Schiff vom letzten Abmelden noch in der Welt (Departing)? Dann
    // übernimmt die neue Verbindung genau dieses Schiff, z. B. nach einem
    // kurzen WLAN-Aussetzer.
    for (size_t i = 0; i < departing_.size(); ++i) {
        if (departing_[i].account != acc) continue;
        const int id = departing_[i].playerId;
        departing_.erase(departing_.begin() + static_cast<long>(i));
        if (world_.FindPlayer(id)) {
            LoginPlayer(c, acc, acc->name, id);
            return;
        }
        break;
    }
    LoginPlayer(c, acc, acc->name);
}

// Karte aus dem Karteneditor ("An Server senden"). Anmeldedaten wie bei
// LoginProof prüfen, dann Berechtigung (Verwaltungsseite) und Obergrenzen
// (Accounts::StoreMap). Angemeldet wird dabei nicht: Der Client bleibt in der
// Lobby und trennt nach der Antwort selbst.
void GameServer::HandleMapUpload(Client& c, const net::MapUploadMsg& m) {
    if (c.role != Role::Lobby) return;
    Account* acc = CheckProof(c, m.proof, "MapUpload ohne LoginStart");
    if (!acc) return;
    net::MapUploadResultMsg res;
    GameMap map = m.map;
    const size_t first = map.name.find_first_not_of(' ');   // Leerzeichen am Rand weg
    map.name = first == std::string::npos ? std::string() : map.name.substr(first, map.name.find_last_not_of(' ') - first + 1);
    map.author = acc->name;
    std::string file, error;
    if (!acc->mapUpload) {
        res.text = "Du darfst auf diesem Server keine Karten hochladen. Der Betreiber kann es auf der Verwaltungsseite erlauben.";
    } else if (map.name.empty()) {
        res.text = "Die Karte braucht einen Namen.";
    } else if (!accounts_.StoreMap(map, file, error)) {
        res.text = error;
    } else {
        res.ok = true;
        res.text = "Karte \"" + map.name + "\" hochgeladen. Der Betreiber kann sie auf der Verwaltungsseite aktivieren.";
        Log(Fmt("%s hat die Karte \"%s\" hochgeladen (%d Teile, Datei %s)", acc->name.c_str(), map.name.c_str(),
                static_cast<int>(map.pieces.size()), file.c_str()));
        if (file == accounts_.ActiveMap()) {
            // Neue Fassung der gerade aktiven Karte: sofort an alle
            UpdateMapData();
            SendToAllInGame(mapData_);
        }
    }
    Send(c, net::Encode(res), true);
}

// Eingaben nur in die Warteschlange legen; angewendet werden sie in ApplyInputs.
void GameServer::HandleInput(Client& c, const net::InputMsg& m) {
    if (c.role != Role::Player) return;   // z. B. verspätetes Paket nach dem Abmelden
    for (const net::InputRecord& rec : m.records) {
        // Jede Eingabe kommt bis zu 4-mal (Schutz gegen Paketverlust). Schon
        // bekannte (seq nicht größer als die letzte) ignorieren.
        if (c.anyInput && rec.seq <= c.lastReceivedSeq) continue;
        c.inputs.push_back(rec);
        c.lastReceivedSeq = rec.seq;
        c.anyInput = true;
    }
    // Zu lange Schlange (Client schneller als der Server oder Netz-Stau):
    // älteste verwerfen. Tastendrücke gehen dabei nicht verloren, weil sie als
    // Zähler in jeder späteren Eingabe mit enthalten sind.
    while (static_cast<int>(c.inputs.size()) > INPUT_QUEUE_MAX) c.inputs.pop_front();
}

void GameServer::HandleDockAction(Client& c, const net::DockActionMsg& m) {
    if (c.role != Role::Player) return;
    PlayerState* ps = world_.FindPlayer(c.playerId);
    // Nur gültig, wenn wirklich angedockt
    if (!ps || ps->dockedAt < 0 || ps->dockedAt >= static_cast<int>(world_.stations.size())) return;
    const Station& st = world_.stations[ps->dockedAt];
    switch (m.action) {
        case net::DockActionType::Undock:
            world_.Undock(*ps);
            c.noInteractUntilTick = tick_ + UNDOCK_INTERACT_GUARD;
            break;
        case net::DockActionType::Repair:
            // wie im Einzelspieler: reparieren an der Handelsstation
            if (st.type == StationType::Trade) world_.Repair(*ps);
            break;
        case net::DockActionType::BuyUpgrade:
            // aufrüsten an der Werft; param wird in BuyUpgrade/UpgradeCost geprüft
            if (st.type == StationType::Shipyard && m.param < cfg::UP_COUNT) world_.BuyUpgrade(*ps, m.param);
            break;
        case net::DockActionType::BuyShip:
            // Schiff kaufen beim Raumschiffsverkäufer; Bit 7 = in Zahlung geben.
            // Typ und Preis prüft BuyShip.
            if (st.type == StationType::Dealer) world_.BuyShip(*ps, m.param & 0x7F, (m.param & 0x80) != 0);
            break;
        case net::DockActionType::SwitchShip:
            // Schiff wechseln an der Handelsstation (Tab "Schiff")
            if (st.type == StationType::Trade) world_.SwitchShip(*ps, m.param);
            break;
        case net::DockActionType::AcceptMission:
            // Mini-Mission annehmen an der Handelsstation (Tab "Missionen");
            // Art und freies Angebot prüft AcceptMission.
            if (st.type == StationType::Trade) world_.AcceptMission(*ps, m.param);
            break;
    }
}

// Chat-Nachricht: prüfen, bremsen und als ChatLine an alle angemeldeten
// Spieler schicken (auch an den Absender, der sie erst dann anzeigt).
// Zuschauer haben keinen Namen und kein Chatfenster.
void GameServer::HandleChat(Client& c, const net::ChatMsg& m) {
    if (c.role != Role::Player) return;
    c.chatting = false;   // gesendet = Eingabe zu (der Client schickt das auch noch)
    if (!net::ValidChatText(m.text)) {
        BadPacket(c, "Chat");
        return;
    }
    const double now = Now();
    c.chatTokens = std::min(CHAT_BURST, c.chatTokens + (now - c.chatTokensAt) * CHAT_PER_SECOND);
    c.chatTokensAt = now;
    if (c.chatTokens < 1.0) {
        if (PlayerState* ps = world_.FindPlayer(c.playerId))
            world_.Notify(*ps, "Chat: bitte etwas langsamer schreiben.", Color{255, 200, 120, 255});
        return;
    }
    c.chatTokens -= 1.0;
    net::ChatLineMsg line;
    line.playerId = c.playerId;
    line.name = c.name;
    line.text = m.text;
    const std::vector<uint8_t> bytes = net::Encode(line);
    for (auto& up : clients_)
        if (up->role == Role::Player && !up->closing) Send(*up, bytes, true);
}

void GameServer::HandleChatTyping(Client& c, const net::ChatTypingMsg& m) {
    if (c.role == Role::Player) c.chatting = m.typing;
}

void GameServer::HandleLeave(Client& c) {
    if (c.role == Role::Player) LogoutPlayer(c, "selbst abgemeldet", true);
    else if (c.role == Role::Spectator) Log(Fmt("Zuschauer #%u (%s) hat sich abgemeldet", c.connId, c.ipText.c_str()));
    Disconnect(c);
}

// Spieler in die Welt holen: neues Schiff (an einer Handelsstation oder dort,
// wo es beim letzten Abmelden war, siehe RestoreProgress) mit dem Fortschritt
// aus dem Konto, oder das noch im All verbliebene Schiff übernehmen
// (resumePlayerId, siehe LogoutPlayer). Dann Begrüßung und alle Listen
// schicken (docs/Mehrspieler.md Abschnitt 3).
void GameServer::LoginPlayer(Client& c, Account* account, const std::string& name, int resumePlayerId) {
    PlayerState* resumed = resumePlayerId >= 0 ? world_.FindPlayer(resumePlayerId) : nullptr;
    const int id = resumed ? resumePlayerId : NewPlayerId();
    PlayerState& ps = resumed ? *resumed : world_.AddPlayer(id, name);
    if (account && !resumed) RestoreProgress(ps, account->progress);
    if (account && c.peer) RememberTrustedIp(ToLowerAscii(account->name), c.ip);

    c.role = Role::Player;
    c.playerId = id;
    c.account = account;
    c.name = name;
    c.challengePending = false;
    c.sessionStart = c.loginAccountedUntil = Now();
    c.timeBaseTick = tick_;
    c.inputs.clear();
    c.anyInput = false;
    c.lastReceivedSeq = 0;
    c.ackSeq = 0;
    c.countersKnown = false;
    c.held = net::InputRecord{};
    c.queueMinInWindow = 1 << 30;
    c.queueWindowSteps = 0;
    c.lastShipId = ps.shipId;
    c.lastDeaths = ps.deaths;
    if (const Ship* s = world_.PlayerShip(ps)) c.lastPos = s->pos;
    c.depositsSent = false;

    const bool returning = account && (account->progress.balance != cfg::START_BALANCE || account->progress.round > 1 ||
                                       account->progress.upgrades[0] + account->progress.upgrades[1] +
                                               account->progress.upgrades[2] + account->progress.upgrades[3] +
                                               account->progress.upgrades[4] > 0 ||
                                       account->progress.shipType != 0 || !account->progress.hangar.empty());
    if (account) account->lastLogin = UnixNow();
    if (!c.isBot) {
        if (resumed)
            world_.Notify(ps, "Willkommen zurück, " + name + "! Dein Schiff war noch im All.", Color{200, 220, 255, 255});
        else if (returning)
            world_.Notify(ps, "Willkommen zurück, " + name + "! Dein Fortschritt wurde geladen.", Color{200, 220, 255, 255});
        else
            world_.Notify(ps, "Willkommen, " + name + "! Finde Kristalle mit dem Scanner [E].", Color{255, 200, 120, 255});
        world_.Notify(ps, "Ziel: 100.000 Ressourcenpunkte auf dem Konto.", Color{200, 220, 255, 255});
        // Funkspruch unten rechts: neues Spiel bzw. Fortsetzen (clip_newstart/clip_return)
        ps.cue = resumed || returning ? cfg::CUE_RETURN : cfg::CUE_NEWSTART;
        ps.cueT = cfg::CUE_TIME;
    }

    SendWelcome(c);
    SendDepositsNow(c);
    Send(c, net::Encode(MakePlayers()), true);
    Send(c, net::Encode(MakeHighscores()), true);
    Log(Fmt("%s ist angemeldet (%d/%d Spieler, Verbindung #%u, %s)", name.c_str(), PlayersOnline(), settings_.maxPlayers,
            c.connId, c.ipText.c_str()));
}

// Abmelden, während das Schiff abstürzt: zählt wie ein Absturz (docs/Mehrspieler.md Abschnitt 13).
// Sonst könnte man sich durch schnelles Abmelden vor dem Verlust retten.
// Rückgabe true, wenn der Fortschritt zurückgesetzt wurde.
bool GameServer::CrashOnLogout(PlayerState& ps) {
    Ship* s = world_.PlayerShip(ps);
    if (!s || !s->crashing) return false;
    ++ps.deaths;
    if (ps.lives > 1) --ps.lives;   // noch Leben übrig: Fortschritt bleibt
    else world_.ResetProgress(ps);
    s->cargo = 0;
    return true;
}

// Spieler abmelden und seinen Fortschritt sichern (auch bei Verbindungsabbruch).
// saveNow = false: der Aufrufer speichert danach selbst (Beenden: einmal für alle).
//
// Angedockt (oder ohne Schiff) verschwindet der Spieler sofort aus der Welt.
// Draußen im All bleibt sein Schiff dagegen noch LOGOUT_GRACE (10 s) ohne
// Steuerung stehen und kann abgeschossen werden (Liste departing_, siehe
// UpdateDeparting). Ohne diese Frist könnte man sich mitten im Gefecht mit
// einem Tastendruck in Sicherheit bringen, und die Regel "abgeschossen ->
// Fortschritt von vorn" wäre wirkungslos. Zusammen mit der gespeicherten
// Position (CaptureProgress) ist Abmelden auch keine Abkürzung zur Station.
void GameServer::LogoutPlayer(Client& c, const char* reason, bool saveNow, bool allowGrace) {
    bool crashed = false;
    bool lingers = false;
    if (PlayerState* ps = world_.FindPlayer(c.playerId)) {
        crashed = CrashOnLogout(*ps);
        StoreProgress(c);
        const Ship* s = world_.PlayerShip(*ps);
        if (allowGrace && c.account && s && !crashed && ps->dockedAt < 0) {
            ps->input = ShipInput{};   // keine Steuerung mehr: Schiff treibt aus
            world_.PlayerShip(*ps)->chatting = false;
            departing_.push_back({c.playerId, c.account, c.name, Now() + LOGOUT_GRACE, ps->deaths});
            lingers = true;
        } else {
            world_.RemovePlayer(c.playerId);
        }
    }
    if (saveNow) accounts_.SaveAccounts();
    Log(c.name + " abgemeldet (" + reason + ", Sitzung " + FormatDuration(Now() - c.sessionStart) + ")" +
        (crashed ? " – während des Absturzes: als Absturz gewertet" : "") +
        (lingers ? " – Schiff bleibt noch 10 s im All" : ""));
    c.role = Role::Lobby;
    c.playerId = -1;
    c.account = nullptr;
    c.inputs.clear();
    c.chatting = false;
}

// Einmal pro Schritt (und mit finishAll vor Neuaufbau und Beenden): Schiffe
// abgemeldeter Spieler entfernen, deren Frist abgelaufen ist oder die
// abgeschossen wurden. Ein abgeschossenes Schiff hat World::PlayerShipLost
// schon behandelt (Fortschritt zurückgesetzt, deaths erhöht).
void GameServer::UpdateDeparting(bool finishAll) {
    if (departing_.empty()) return;
    const double now = Now();
    for (size_t i = 0; i < departing_.size();) {
        const Departing& d = departing_[i];
        const PlayerState* ps = world_.FindPlayer(d.playerId);
        const Ship* s = ps ? world_.PlayerShip(*ps) : nullptr;
        const bool done = finishAll || now >= d.until || !ps || !s || s->crashing || ps->deaths != d.deaths;
        if (!done) {
            ++i;
            continue;
        }
        FinishDeparting(d);
        departing_.erase(departing_.begin() + static_cast<long>(i));
    }
}

void GameServer::FinishDeparting(const Departing& d) {
    PlayerState* ps = world_.FindPlayer(d.playerId);
    if (!ps) return;
    CrashOnLogout(*ps);   // Frist lief mitten im Absturz ab: zählt als Absturz
    const bool lost = ps->deaths != d.deaths;
    d.account->progress = CaptureProgress(*ps);
    world_.RemovePlayer(d.playerId);
    accounts_.SaveAccounts();
    Log(lost ? d.name + ": Schiff nach dem Abmelden abgeschossen – Fortschritt zurückgesetzt"
             : d.name + ": Schiff hat die Welt verlassen");
}

void GameServer::Send(Client& c, const std::vector<uint8_t>& bytes, bool reliable) {
    if (c.closing || bytes.empty()) return;
    c.bytesSent += bytes.size();
    ++c.packetsSent;
    if (!c.peer) return;   // Bot im Benchmark: nur zählen
    // Große unzuverlässige Pakete (Zuschauer-Snapshot) werden von ENet in
    // Stücke geteilt. UNRELIABLE_FRAGMENT sorgt dafür, dass auch die Stücke
    // unzuverlässig bleiben – sonst würde ENet sie zuverlässig wiederholen und
    // alte Snapshots würden neue aufhalten.
    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
    if (!packet) return;
    const enet_uint8 channel = static_cast<enet_uint8>(reliable ? net::CHANNEL_RELIABLE : net::CHANNEL_UNRELIABLE);
    // Bei Erfolg gehört das Paket ENet. Bei einem Fehler (z. B. Verbindung
    // gerade getrennt) müssen wir es selbst freigeben.
    if (enet_peer_send(c.peer, channel, packet) < 0 && packet->referenceCount == 0) enet_packet_destroy(packet);
}

void GameServer::SendToAllInGame(const std::vector<uint8_t>& bytes) {
    for (auto& up : clients_)
        if (up->role != Role::Lobby && !up->closing) Send(*up, bytes, true);
}

void GameServer::SendError(Client& c, net::ErrorCode code, const std::string& text) {
    net::ErrorMsg m;
    m.code = code;
    m.text = text;
    Send(c, net::Encode(m), true);
}

void GameServer::SendWelcome(Client& c) {
    net::WelcomeMsg m;
    m.role = c.role == Role::Player ? net::Role::Player : net::Role::Spectator;
    m.playerId = c.role == Role::Player ? c.playerId : -1;
    m.worldSeed = world_.seed;
    m.settings = settings_;
    m.totalDelivered = world_.totalDelivered;
    m.serverTime = ClientTime(c);
    m.tick = static_cast<uint32_t>(tick_);
    Send(c, net::Encode(m), true);
    Send(c, mapData_, true);   // Kulisse: aktive Karte (gleicher Kanal, kommt also nach dem Welcome an)
}

void GameServer::SendDepositsNow(Client& c) {
    const std::vector<uint8_t> bytes = net::Encode(MakeDeposits(c));
    Send(c, bytes, true);
    const PlayerState* ps = c.role == Role::Player ? world_.FindPlayer(c.playerId) : nullptr;
    c.depositsSent = true;
    c.sentDepositHash = Fnv1a(bytes);
    c.sentDepositRevision = world_.depositRevision;
    c.sentRevealRevision = ps ? ps->revealRevision : 0;
    c.lastDepositsTick = tick_;
}

// ===========================================================================
// 5. Nachrichten bauen
// ===========================================================================
net::ServerInfoMsg GameServer::MakeServerInfo() const {
    net::ServerInfoMsg m;
    m.version = net::PROTOCOL_VERSION;
    m.serverName = settings_.serverName;
    m.playersOnline = static_cast<uint8_t>(std::min(255, PlayersOnline()));
    m.maxPlayers = static_cast<uint8_t>(settings_.maxPlayers);
    m.spectators = static_cast<uint8_t>(std::min(255, SpectatorsOnline()));
    m.maxSpectators = static_cast<uint8_t>(limits::MAX_SPECTATORS);
    m.settings = settings_;
    m.realmSalt = accounts_.RealmSalt();
    m.totalDelivered = world_.totalDelivered;
    return m;
}

// Momentaufnahme für einen Client (docs/Mehrspieler.md Abschnitte 4 und 7). Positionen werden erst
// beim Kodieren (Protocol.cpp) auf Zentimeter quantisiert; hier stehen die
// genauen Werte aus der Welt.
net::SnapshotMsg GameServer::MakeSnapshot(const Client& c) const {
    net::SnapshotMsg m;
    m.tick = static_cast<uint32_t>(tick_);
    m.serverTime = ClientTime(c);
    m.totalDelivered = world_.totalDelivered;
    m.stationStage = static_cast<uint8_t>(world_.StationStage());

    const bool spectator = c.role == Role::Spectator;
    const PlayerState* ps = spectator ? nullptr : world_.FindPlayer(c.playerId);
    const Ship* own = ps ? world_.PlayerShip(*ps) : nullptr;
    const Vector3 center = own ? own->pos : c.lastPos;

    // --- eigener Zustand (genau, für die Vorhersage im Client) ---
    if (ps) {
        m.hasMe = true;
        net::SnapMe& e = m.me;
        e.shipId = own ? own->id : -1;
        e.ackSeq = c.ackSeq;
        e.x = center.x;
        e.y = center.y;
        e.z = center.z;
        if (own) {
            e.vx = own->vel.x;
            e.vz = own->vel.z;
            e.yaw = own->yaw;
            e.bank = own->bank;
            e.hp = own->hp;
            e.maxHp = own->maxHp;
            e.hitFlash = own->hitFlash;
            e.thrustVisual = own->thrustVisual;
            e.crashing = own->crashing;
            e.cargo = own->cargo;
            e.cargoMax = own->cargoMax;
        } else {
            // Kein Schiff (wartet auf ein neues): sinnvolle Werte statt 0/0,
            // damit der Client nicht durch 0 teilt.
            const ShipType& t = settings_.Ship(ps->shipType);
            e.maxHp = ShipArmor(t, std::clamp(ps->upgrades[cfg::UP_ARMOR], 0, cfg::UPGRADE_LEVELS));
            e.cargoMax = ShipCargo(t, std::clamp(ps->upgrades[cfg::UP_CARGO], 0, cfg::UPGRADE_LEVELS));
        }
        e.balance = ps->balance;
        for (int u = 0; u < cfg::UP_COUNT; ++u)
            e.upgrades[u] = static_cast<uint8_t>(std::clamp(ps->upgrades[u], 0, cfg::UPGRADE_LEVELS));
        e.shipType = static_cast<uint8_t>(std::clamp(ps->shipType, 0, settings_.ShipCount() - 1));
        e.hangar.clear();
        for (const OwnedShip& o : ps->hangar) {
            net::SnapMe::Parked h;
            h.type = static_cast<uint8_t>(std::clamp(o.type, 0, settings_.ShipCount() - 1));
            for (int u = 0; u < cfg::UP_COUNT; ++u) h.upgrades[u] = static_cast<uint8_t>(std::clamp(o.upgrades[u], 0, cfg::UPGRADE_MAX[u]));
            e.hangar.push_back(h);
        }
        e.scanEnergy = ps->scanEnergy;
        e.scanOn = ps->scanOn;
        e.scanPhase = std::fmod(ps->scanPulse, cfg::SCANNER_PULSE_TIME);
        e.mining = ps->mining;
        e.mineKind = static_cast<uint8_t>(std::clamp(ps->mineKind, 0, 255));
        e.mineId = ps->mineId;
        e.mineT = ps->mineT;
        e.dockedAt = static_cast<int8_t>(std::clamp(ps->dockedAt, -1, 127));
        e.lastSale = ps->lastSale;
        e.shipTime = ps->playTime;
        e.roundTime = ps->roundTime;
        e.round = ClampU16(ps->round);
        e.kills = ClampU16(ps->kills);
        e.respawnT = ps->respawnT;
        e.lives = static_cast<uint8_t>(std::clamp(ps->lives, 0, limits::MAX_LIVES));
        // Mini-Missionen: eigene Mission, offene Angebote, Markierungen, Funkspruch
        if (const Mission* mi = world_.FindMission(ps->mission)) {
            e.missionType = static_cast<int8_t>(mi->type);
            e.missionProgress = static_cast<uint8_t>(std::clamp(mi->progress, 0, 255));
            e.missionGoal = static_cast<uint8_t>(std::clamp(mi->goal, 0, 255));
            e.missionAlive = static_cast<uint8_t>(std::clamp(mi->alive, 0, 255));
        }
        for (const Mission& mi : world_.missions)
            if (mi.owner < 0 && e.offers.size() < static_cast<size_t>(limits::MAX_MISSIONS))
                e.offers.push_back(static_cast<uint8_t>(mi.type));
        for (const MissionMarker& mk : ps->markers) {
            if (e.markers.size() >= static_cast<size_t>(net::MAX_MARKERS)) break;
            e.markers.push_back({static_cast<uint8_t>(mk.kind), mk.pos.x, mk.pos.z});
        }
        e.cue = static_cast<int8_t>(ps->cueT > 0.0f ? ps->cue : -1);
        e.cueT = ps->cueT;
    }

    // --- andere Schiffe ---
    // Spieler: nur im Umkreis AOI_SHIPS (spart Daten und verrät nicht, wo
    // alle anderen sind). Zuschauer: die ganze Karte.
    std::vector<Candidate> cand;
    for (size_t i = 0; i < world_.ships.size(); ++i) {
        const Ship& s = world_.ships[i];
        if (s.dead || s.hidden) continue;           // versteckt = angedockter Händler
        if (own && s.id == own->id) continue;       // eigenes Schiff steht in "me"
        const float d = Dist2D(s.pos, center);
        if (!spectator && d > net::AOI_SHIPS) continue;
        cand.push_back({d, i});
    }
    KeepNearest(cand, spectator ? MAX_SPECTATOR_LIST : MAX_SNAP_SHIPS);
    m.ships.reserve(cand.size());
    for (const Candidate& k : cand) {
        const Ship& s = world_.ships[k.index];
        net::SnapShip ss;
        ss.id = static_cast<uint32_t>(s.id);
        ss.kind = static_cast<uint8_t>(s.kind);
        ss.variant = net::ShipVariant(models_, s.model);
        ss.x = s.pos.x;
        ss.y = s.pos.y;
        ss.z = s.pos.z;
        ss.yaw = s.yaw;
        ss.bank = s.bank;
        ss.hpFrac = s.maxHp > 0.0f ? std::clamp(s.hp / s.maxHp, 0.0f, 1.0f) : 0.0f;
        ss.thrustVisual = s.thrustVisual;
        ss.hitFlash = s.hitFlash;
        ss.crashing = s.crashing;
        ss.hostile = s.hostileToPlayer;
        ss.mining = s.mining;
        ss.docked = s.docked;
        ss.mineX = s.mineTarget.x;
        ss.mineZ = s.mineTarget.z;
        ss.playerId = static_cast<int16_t>(std::clamp(s.playerId, -1, 32767));
        ss.chatting = s.chatting;
        m.ships.push_back(ss);
    }

    // --- Geschosse (nur Spieler; Zuschauer sehen sie aus der Höhe kaum) ---
    if (!spectator) {
        cand.clear();
        for (size_t i = 0; i < world_.bullets.size(); ++i) {
            const float d = Dist2D(world_.bullets[i].pos, center);
            if (d <= net::AOI_BULLETS) cand.push_back({d, i});
        }
        KeepNearest(cand, MAX_SNAP_BULLETS);
        m.bullets.reserve(cand.size());
        for (const Candidate& k : cand) {
            const Bullet& b = world_.bullets[k.index];
            net::SnapBullet sb;
            sb.x = b.pos.x;
            sb.y = b.pos.y;
            sb.z = b.pos.z;
            sb.vx = b.vel.x;
            sb.vz = b.vel.z;
            // Farbe/Art des Geschosses im Client: 3 Turm, 0 Spieler, 1 Pirat, 2 Händler
            if (b.ownerId == -1) sb.type = 3;
            else if (b.faction == Faction::Player) sb.type = 0;
            else if (b.faction == Faction::Pirate) sb.type = 1;
            else sb.type = 2;
            m.bullets.push_back(sb);
        }
    }

    // --- Wracks ---
    cand.clear();
    for (size_t i = 0; i < world_.wrecks.size(); ++i) {
        const float d = Dist2D(world_.wrecks[i].pos, center);
        if (spectator || d <= net::AOI_SHIPS) cand.push_back({d, i});
    }
    KeepNearest(cand, spectator ? MAX_SPECTATOR_LIST : MAX_SNAP_WRECKS);
    m.wrecks.reserve(cand.size());
    for (const Candidate& k : cand) {
        const Wreck& w = world_.wrecks[k.index];
        net::SnapWreck sw;
        sw.id = static_cast<uint32_t>(w.id);
        sw.x = w.pos.x;
        sw.z = w.pos.z;
        sw.yaw = w.yaw;
        sw.tilt = w.tilt;
        sw.variant = net::ShipVariant(models_, w.model);
        sw.amount = ClampU16(w.amount);
        sw.initial = ClampU16(w.initial);
        sw.age = w.age;
        m.wrecks.push_back(sw);
    }
    return m;
}

// Spieler: nur selbst entdeckte Lagerstätten (jeder Spieler muss selbst
// scannen). Zuschauer: alle.
net::DepositsMsg GameServer::MakeDeposits(const Client& c) const {
    net::DepositsMsg m;
    const PlayerState* ps = c.role == Role::Player ? world_.FindPlayer(c.playerId) : nullptr;
    if (!ps && c.role != Role::Spectator) return m;
    for (const Deposit& d : world_.deposits) {
        if (d.amount <= 0) continue;
        if (ps && !world_.RevealedFor(d, *ps)) continue;
        net::NetDeposit nd;
        nd.id = static_cast<uint32_t>(d.id);
        nd.x = d.pos.x;
        nd.z = d.pos.z;
        nd.yaw = d.yaw;
        nd.scale = d.scale;
        nd.variant = net::DepositVariant(models_, d.model);
        nd.amount = ClampU16(d.amount);
        nd.initial = ClampU16(d.initial);
        m.deposits.push_back(nd);
    }
    return m;
}

net::PlayersMsg GameServer::MakePlayers() const {
    net::PlayersMsg m;
    m.totalDelivered = world_.totalDelivered;
    m.spectators = static_cast<uint8_t>(std::min(255, SpectatorsOnline()));
    m.maxPlayers = static_cast<uint8_t>(settings_.maxPlayers);
    m.players.reserve(world_.players.size());
    for (const PlayerState& ps : world_.players) {
        net::PlayerEntry e;
        e.id = ps.id;
        e.name = ps.name;
        e.shipId = ps.shipId;
        e.balance = ps.balance;
        e.round = ClampU16(ps.round);
        e.shipTime = ps.playTime;
        e.roundTime = ps.roundTime;
        for (int u = 0; u < cfg::UP_COUNT; ++u)
            e.upgrades[u] = static_cast<uint8_t>(std::clamp(ps.upgrades[u], 0, cfg::UPGRADE_LEVELS));
        e.lives = static_cast<uint8_t>(std::clamp(ps.lives, 0, limits::MAX_LIVES));
        e.alive = world_.PlayerAlive(ps);
        e.docked = ps.dockedAt >= 0;
        e.kills = ClampU16(ps.kills);
        e.goals = ClampU16(ps.goals);
        m.players.push_back(e);
    }
    SortByProgress(m.players);
    return m;
}

net::HighscoresMsg GameServer::MakeHighscores() const {
    net::HighscoresMsg m;
    for (const HighscoreRecord& h : accounts_.TopHighscores(Accounts::HIGHSCORES_SHOWN)) {
        net::HighscoreEntry e;
        e.name = h.name;
        e.seconds = static_cast<float>(h.seconds);
        e.round = ClampU16(h.round);
        e.when = h.when;
        m.entries.push_back(e);
    }
    return m;
}

int GameServer::PlayersOnline() const {
    int n = 0;
    for (const auto& up : clients_)
        if (up->role == Role::Player) ++n;
    return n;
}

int GameServer::SpectatorsOnline() const {
    int n = 0;
    for (const auto& up : clients_)
        if (up->role == Role::Spectator && !up->closing) ++n;
    return n;
}

// Darf dieser Anmeldeversuch überhaupt geprüft werden? (docs/Mehrspieler.md Abschnitt 8)
// Drei Zähler für Fehlversuche, jeweils über die letzten 10 Minuten:
//   1. pro (Konto, Absender-Adresse): höchstens 10. Gesperrt wird nur dieser
//      Absender für dieses Konto. Ein Fremder kann den echten Besitzer damit
//      nicht aussperren (die Namen aller Spieler sind ja öffentlich).
//   2. pro öffentlicher Adresse: höchstens 30, über alle Namen zusammen.
//      Bremst das Durchprobieren eines Passworts bei vielen Konten.
//   3. pro Konto insgesamt: höchstens 100 ("Notbremse" gegen Raten von sehr
//      vielen Adressen aus). Sie gilt nicht für Adressen, von denen aus sich
//      der Besitzer seit dem Serverstart schon erfolgreich angemeldet hat.
// Lokale Adressen (Heimnetz) zählen nur bei 1 und 3.
// Während einer Sperre wird der Beweis gar nicht erst geprüft: Sonst könnte
// ein Angreifer trotz Sperre weiterraten und an der Antwort erkennen, ob er
// richtig lag.
bool GameServer::LoginBlocked(const std::string& lowerName, uint32_t ip, double now) {
    if (CountRecent(failuresByNameIp_, {lowerName, ip}, now, LOGIN_FAILURE_WINDOW) >= MAX_LOGIN_FAILURES_PER_NAME_IP)
        return true;
    if (!IsLocalAddress(ip) && CountRecent(failuresByIp_, ip, now, LOGIN_FAILURE_WINDOW) >= MAX_LOGIN_FAILURES_PER_IP)
        return true;
    if (CountRecent(failuresByName_, lowerName, now, LOGIN_FAILURE_WINDOW) >= MAX_LOGIN_FAILURES_PER_NAME) {
        auto it = trustedIps_.find(lowerName);
        const bool trusted = it != trustedIps_.end() && std::find(it->second.begin(), it->second.end(), ip) != it->second.end();
        if (!trusted) return true;
    }
    return false;
}

// Fehlversuch merken. Zähler 1 und 3 nur für Konten, die es gibt: So bleibt
// die Liste klein (höchstens so viele Einträge wie Konten bzw. 30 pro
// Adresse), und niemand kann sie mit erfundenen Namen füllen. Läuft eine
// Liste trotzdem voll, bleiben die anderen Zähler wirksam.
// Erreicht ein Zähler gerade seine Grenze, steht das einmal im Log, damit
// der Betreiber einen Angriff bemerkt (nicht bei jedem weiteren Paket).
void GameServer::RecordLoginFailure(const std::string& lowerName, bool accountExists, const Client& c, double now) {
    auto record = [&](auto& map, const auto& key, int limit, const std::string& message) {
        if (map.size() >= MAX_TRACKED_FAILURES && !map.count(key)) {
            if (now - lastTableFullLog_ > 60.0) {
                Log("Warnung: Liste der Fehlversuche ist voll (sehr viele Anmeldeversuche?)");
                lastTableFullLog_ = now;
            }
            return;
        }
        auto& dq = map[key];
        dq.push_back(now);
        while (static_cast<int>(dq.size()) > limit) dq.pop_front();   // mehr braucht die Prüfung nicht
        if (static_cast<int>(dq.size()) == limit && now - dq.front() <= LOGIN_FAILURE_WINDOW) Log(message);
    };
    if (!IsLocalAddress(c.ip))
        record(failuresByIp_, c.ip, MAX_LOGIN_FAILURES_PER_IP,
               Fmt("Adresse %s: %d fehlgeschlagene Anmeldungen in 10 min – weitere Versuche von dort vorerst gesperrt",
                   c.ipText.c_str(), MAX_LOGIN_FAILURES_PER_IP));
    if (!accountExists) return;
    record(failuresByNameIp_, std::make_pair(lowerName, c.ip), MAX_LOGIN_FAILURES_PER_NAME_IP,
           Fmt("Anmeldung für \"%s\" von %s gesperrt (%d Fehlversuche in 10 min)", lowerName.c_str(), c.ipText.c_str(),
               MAX_LOGIN_FAILURES_PER_NAME_IP));
    record(failuresByName_, lowerName, MAX_LOGIN_FAILURES_PER_NAME,
           Fmt("Konto \"%s\": %d Fehlversuche in 10 min von verschiedenen Adressen – Anmeldung nur noch von bekannten "
               "Adressen",
               lowerName.c_str(), MAX_LOGIN_FAILURES_PER_NAME));
}

// Nach einer erfolgreichen Anmeldung: Diese Adresse gilt für das Konto als
// bekannt (Ausnahme von der Notbremse in LoginBlocked). Nur im Speicher, die
// letzten 4 Adressen je Konto.
void GameServer::RememberTrustedIp(const std::string& lowerName, uint32_t ip) {
    auto& v = trustedIps_[lowerName];
    v.erase(std::remove(v.begin(), v.end(), ip), v.end());
    v.push_back(ip);
    if (v.size() > MAX_TRUSTED_IPS_PER_NAME) v.erase(v.begin());
}

bool GameServer::RegistrationAllowed(uint32_t ip, double now) {
    if (IsLocalAddress(ip)) return true;
    return CountRecent(registrations_, ip, now, REGISTRATION_WINDOW) < MAX_REGISTRATIONS_PER_IP;
}

void GameServer::RecordRegistration(uint32_t ip, double now) {
    if (IsLocalAddress(ip)) return;
    registrations_[ip].push_back(now);
}

// ===========================================================================
// 6. Welt: Einstellungen, Neuaufbau, Spieler-IDs, Fortschritt
// ===========================================================================
int GameServer::NewPlayerId() {
    // IDs laufen 1..30000 (passt in int16 der Snapshots) und werden nie
    // gleichzeitig doppelt vergeben.
    for (int tries = 0; tries < 30001; ++tries) {
        const int id = nextPlayerId_;
        nextPlayerId_ = nextPlayerId_ >= 30000 ? 1 : nextPlayerId_ + 1;
        if (!world_.FindPlayer(id)) return id;
    }
    return nextPlayerId_;
}

void GameServer::RestoreProgress(PlayerState& ps, const PlayerProgress& p) {
    // Werte aus der Datei nochmals begrenzen: die Datei könnte von Hand
    // bearbeitet worden sein, und Upgrade-Stufen dienen als Array-Index.
    ps.balance = std::max(0LL, p.balance);
    ps.delivered = std::max(0LL, p.delivered);
    for (int u = 0; u < cfg::UP_COUNT; ++u) ps.upgrades[u] = std::clamp(p.upgrades[u], 0, cfg::UPGRADE_MAX[u]);
    ps.lives = p.lives > 0 ? std::clamp(p.lives, 1, limits::MAX_LIVES) : settings_.lives;
    // Schiffstyp und geparkte Schiffe; je Typ höchstens ein Schiff
    ps.shipType = std::clamp(p.shipType, 0, settings_.ShipCount() - 1);
    ps.hangar.clear();
    for (const auto& h : p.hangar) {
        const int t = std::clamp(h.type, 0, settings_.ShipCount() - 1);
        if (world_.OwnsShipType(ps, t)) continue;
        OwnedShip o;
        o.type = t;
        for (int u = 0; u < cfg::UP_COUNT; ++u) o.upgrades[u] = std::clamp(h.upgrades[u], 0, cfg::UPGRADE_MAX[u]);
        o.hp = h.hp;
        ps.hangar.push_back(o);
    }
    ps.playTime = static_cast<float>(p.shipTime);
    ps.roundTime = static_cast<float>(p.roundTime);
    ps.round = std::max(1, p.round);
    ps.kills = std::max(0, p.kills);
    ps.deaths = std::max(0, p.deaths);
    ps.goals = std::max(0, p.goals);
    world_.ApplyUpgrades(ps, true);   // Geschwindigkeit, Schild, Waffe, Lager aus den Stufen
    if (Ship* s = world_.PlayerShip(ps)) {
        s->cargo = std::clamp(p.cargo, 0, s->cargoMax);
        if (p.hp > 0.0f) s->hp = std::clamp(p.hp, 1.0f, s->maxHp);
        // Dort weitermachen, wo man sich abgemeldet hat, statt an der
        // Handelsstation (SpawnPlayerShip). Nur in derselben Welt (Seed) und
        // innerhalb der Karte; sonst bleibt es beim Platz an der Station.
        if (p.hasPos && p.posSeed == world_.seed && std::isfinite(p.posX) && std::isfinite(p.posZ) &&
            std::isfinite(p.yaw) && std::fabs(p.posX) <= cfg::WORLD_HALF && std::fabs(p.posZ) <= cfg::WORLD_HALF) {
            s->pos = {p.posX, cfg::SHIP_ALTITUDE, p.posZ};
            s->yaw = p.yaw;
            s->vel = {0, 0, 0};
        }
    }
}

PlayerProgress GameServer::CaptureProgress(const PlayerState& ps) const {
    PlayerProgress p;
    p.balance = ps.balance;
    p.delivered = ps.delivered;
    for (int u = 0; u < cfg::UP_COUNT; ++u) p.upgrades[u] = ps.upgrades[u];
    p.shipType = ps.shipType;
    for (const OwnedShip& o : ps.hangar) {
        PlayerProgress::Parked h;
        h.type = o.type;
        for (int u = 0; u < cfg::UP_COUNT; ++u) h.upgrades[u] = o.upgrades[u];
        h.hp = o.hp;
        p.hangar.push_back(h);
    }
    p.lives = ps.lives;
    const Ship* s = world_.PlayerShip(ps);
    if (s && !s->crashing) {
        p.cargo = s->cargo;
        p.hp = s->hp;
        p.hasPos = true;   // auch angedockt: das Schiff steht dann neben der Station
        p.posX = s->pos.x;
        p.posZ = s->pos.z;
        p.yaw = s->yaw;
        p.posSeed = world_.seed;
    } else {
        p.cargo = 0;     // kein Schiff / Absturz: Ladung ist verloren
        p.hp = -1.0f;    // nächstes Schiff hat vollen Schild
    }
    p.shipTime = ps.playTime;
    p.roundTime = ps.roundTime;
    p.round = ps.round;
    p.kills = ps.kills;
    p.deaths = ps.deaths;
    p.goals = ps.goals;
    return p;
}

void GameServer::ApplySettingsRequest() {
    if (!shared_) return;
    GameSettings s;
    bool reset = false;
    if (!shared_->TakeSettingsRequest(s, reset)) return;
    if (reset) {
        // Zurück auf docker-compose.yml: die Datei der Webseite löschen, damit
        // auch nach einem Neustart wieder die Umgebungsvariablen gelten.
        s = config_.defaults;
        accounts_.RemoveSettings();
    }
    // Die Schiffe ändert nur der eigene Bereich der Webseite (ApplyShipsRequest);
    // auch "Auf Standard zurücksetzen" lässt sie stehen.
    s.ships = settings_.ships;
    Sanitize(s);   // nie ungeprüft übernehmen, auch wenn WebServer schon geprüft hat
    if (!reset) accounts_.SaveSettings(s);
    ApplySettings(s);
}

void GameServer::ApplySettings(const GameSettings& s) {
    const GameSettings old = settings_;
    settings_ = s;
    const bool rebuild = NeedsWorldRebuild(old, s);
    Log(Fmt("Einstellungen geändert: \"%s\", %s (%d Gegner), %d Handelsstation(en), %d Werft(en), Mond %s, max. %d Spieler, %d Leben, %d Missionen, neue Konten %s%s",
            s.serverName.c_str(), DifficultyName(s.difficulty), s.enemyShips, s.tradeStations, s.shipyards,
            MoonColorName(s.moon), s.maxPlayers, s.lives, s.missions, s.allowRegistration ? "erlaubt" : "nicht erlaubt",
            rebuild ? " – Karte wird neu aufgebaut" : ""));
    if (rebuild) {
        RebuildWorld();
    } else {
        // Ohne Neuaufbau: Die Welt liest settings.enemyShips in jedem Schritt
        // (Piraten nachschieben). Bei weniger Gegnern sofort aufräumen.
        world_.settings = s;
        RemoveSurplusPirates();
        BroadcastSettings();
    }
}

// Schiffe von der Webseite (Bereich "Schiffe"): sofort übernehmen, auch für
// die Schiffe, die gerade fliegen. Die Welt muss dafür nicht neu aufgebaut werden.
void GameServer::ApplyShipsRequest() {
    if (!shared_) return;
    GameSettings s = settings_;
    if (!shared_->TakeShipsRequest(s.ships)) return;
    Sanitize(s);
    accounts_.SaveShips(s.ships);
    settings_.ships = s.ships;
    world_.settings.ships = s.ships;
    for (PlayerState& ps : world_.players) {
        world_.ApplyUpgrades(ps, false);
        if (Ship* sp = world_.PlayerShip(ps)) sp->cargo = std::min(sp->cargo, sp->cargoMax);
    }
    Log(Fmt("Schiffe geändert: %d Schiffstyp(en)", settings_.ShipCount()));
    BroadcastSettings();
}

// Bereich "Karten" der Webseite. Eine Karte ist nur Kulisse (keine
// Kollision), deshalb ändert sich an der Welt des Servers nichts: Die Clients
// bekommen die neue Karte (MapData) und tauschen ihre Deko aus.
void GameServer::ApplyMapCommands() {
    std::vector<MapCommand> cmds;
    if (!shared_ || !shared_->TakeMapCommands(cmds)) return;
    for (const MapCommand& cmd : cmds) {
        switch (cmd.kind) {
            case MapCommand::Kind::Activate:
            case MapCommand::Kind::Delete: {
                const ServerMap* sm = cmd.arg.empty() ? nullptr : accounts_.FindMap(cmd.arg);
                if (!cmd.arg.empty() && !sm) {
                    MapNotice("Diese Karte gibt es nicht mehr.", true);
                    break;
                }
                const std::string name = sm ? sm->map.name : std::string("Standardkarte");
                const bool wasActive = cmd.arg == accounts_.ActiveMap();
                if (cmd.kind == MapCommand::Kind::Delete) {
                    if (!sm) break;   // die Standardkarte lässt sich nicht löschen
                    accounts_.RemoveMap(cmd.arg);
                    MapNotice("Karte \"" + name + "\" gelöscht." + (wasActive ? " Es gilt wieder die Standardkarte." : ""));
                    if (!wasActive) break;
                    accounts_.SetActiveMap(std::string());
                } else {
                    if (wasActive) break;
                    accounts_.SetActiveMap(cmd.arg);
                    MapNotice("Karte \"" + name + "\" ist jetzt aktiv.");
                }
                accounts_.SaveServerData();
                UpdateMapData();
                SendToAllInGame(mapData_);
                break;
            }
            case MapCommand::Kind::Allow:
            case MapCommand::Kind::Deny: {
                Account* acc = accounts_.Find(cmd.arg);
                if (!acc) {
                    MapNotice("Es gibt kein Konto \"" + cmd.arg + "\".", true);
                    break;
                }
                acc->mapUpload = cmd.kind == MapCommand::Kind::Allow;
                accounts_.SaveAccounts();
                MapNotice(acc->name + (acc->mapUpload ? " darf jetzt Karten hochladen." : " darf keine Karten mehr hochladen."));
                break;
            }
        }
    }
}

void GameServer::UpdateMapData() {
    net::MapDataMsg m;
    if (const ServerMap* sm = accounts_.FindMap(accounts_.ActiveMap())) {
        m.custom = true;
        m.map = sm->map;
        m.map.author.clear();
    }
    mapData_ = net::Encode(m);
}

void GameServer::MapNotice(const std::string& text, bool error) {
    mapNotice_ = text;
    mapNoticeError_ = error;
    ++mapNoticeSeq_;
    Log("Karten: " + text);
}

// Bereich "Sperren" der Webseite. Eine neue Sperre wirkt sofort: Wer
// betroffen und verbunden ist, wird getrennt. Der Web-Thread hat Namen und
// Adressen schon geprüft; hier wird trotzdem nichts ungeprüft übernommen.
void GameServer::ApplyBanCommands() {
    std::vector<BanCommand> cmds;
    if (!shared_ || !shared_->TakeBanCommands(cmds)) return;
    const int64_t now = UnixNow();
    const std::string full = "Es gibt schon " + std::to_string(Accounts::MAX_BANS) + " Sperren. Bitte zuerst alte entfernen.";

    // Alle Verbindungen aus dem Bereich einer IP-Sperre trennen; Anzahl zurück
    auto removeInRange = [&](const Ban& b) {
        int n = 0;
        for (auto& up : clients_) {
            Client& c = *up;
            if (c.closing || !c.peer || !IpInRange(ReadableIp(c.ip), b.net, b.prefix)) continue;
            RemoveByOperator(c, net::ErrorCode::Banned, BanText(b), "Adresse gesperrt");
            ++n;
        }
        return n;
    };

    for (const BanCommand& cmd : cmds) {
        if (!cmd.reason.empty() && !net::ValidChatText(cmd.reason)) {   // wird im Spiel angezeigt
            BanNotice("Der Grund enthält Zeichen, die das Spiel nicht anzeigen kann.", true);
            continue;
        }
        switch (cmd.kind) {
            case BanCommand::Kind::BanAccount: {
                const Account* acc = accounts_.Find(cmd.target);
                if (!acc) {
                    BanNotice("Es gibt kein Konto \"" + cmd.target + "\".", true);
                    break;
                }
                Ban b;
                b.target = acc->name;
                b.reason = cmd.reason;
                b.created = now;
                b.until = cmd.days > 0 ? now + static_cast<int64_t>(cmd.days) * 86400 : 0;
                if (!accounts_.AddBan(b)) {
                    BanNotice(full, true);
                    break;
                }
                std::string text = acc->name + " ist gesperrt (" + DaysText(cmd.days) + ").";
                Client* online = nullptr;
                for (auto& up : clients_)
                    if (!up->closing && up->role == Role::Player && up->account == acc) online = up.get();
                // Auf Wunsch auch die Adresse der aktuellen Verbindung sperren
                Ban ipBan;
                bool ipBanned = false;
                if (cmd.withIp) {
                    if (!online || !online->peer) {
                        text += " Die Adresse wurde nicht gesperrt, weil " + acc->name + " gerade nicht online ist.";
                    } else {
                        ipBan.isIp = true;
                        ipBan.net = ReadableIp(online->ip);
                        ipBan.target = IpRangeText(ipBan.net, ipBan.prefix);
                        ipBan.reason = cmd.reason;
                        ipBan.created = now;
                        ipBan.until = b.until;
                        if (IpRangeProtected(ipBan.net, ipBan.prefix)) {
                            text += " Die Adresse " + ipBan.target + " wurde nicht gesperrt (Docker-Netz oder dieser Rechner).";
                        } else if (!accounts_.AddBan(ipBan)) {
                            text += " Die Adresse wurde nicht gesperrt: " + full;
                        } else {
                            text += " Adresse " + ipBan.target + " ebenfalls gesperrt.";
                            ipBanned = true;
                        }
                    }
                }
                if (online) {
                    RemoveByOperator(*online, net::ErrorCode::Banned, BanText(b), "Konto gesperrt");
                    text += " Die Verbindung wurde getrennt.";
                }
                // Weitere Verbindungen von der eben gesperrten Adresse (z. B. ein Zweitkonto)
                if (ipBanned) removeInRange(ipBan);
                accounts_.SaveBans();
                BanNotice(text);
                break;
            }
            case BanCommand::Kind::BanIp: {
                Ban b;
                b.isIp = true;
                if (!ParseIpRange(cmd.target, b.net, b.prefix) || IpRangeProtected(b.net, b.prefix)) {
                    BanNotice("Diese Adresse kann nicht gesperrt werden.", true);
                    break;
                }
                b.target = IpRangeText(b.net, b.prefix);
                b.reason = cmd.reason;
                b.created = now;
                b.until = cmd.days > 0 ? now + static_cast<int64_t>(cmd.days) * 86400 : 0;
                if (!accounts_.AddBan(b)) {
                    BanNotice(full, true);
                    break;
                }
                accounts_.SaveBans();
                const int n = removeInRange(b);
                BanNotice(b.target + " ist gesperrt (" + DaysText(cmd.days) + ")." +
                          (n > 0 ? " " + std::to_string(n) + " Verbindung(en) getrennt." : std::string()));
                break;
            }
            case BanCommand::Kind::Unban:
                if (accounts_.RemoveBan(cmd.isIp, cmd.target)) {
                    accounts_.SaveBans();
                    BanNotice(cmd.target + " ist wieder freigegeben.");
                } else {
                    BanNotice("Diese Sperre gibt es nicht mehr.", true);
                }
                break;
            case BanCommand::Kind::Kick: {
                const std::string lower = ToLowerAscii(cmd.target);
                Client* found = nullptr;
                for (auto& up : clients_)
                    if (!up->closing && up->role == Role::Player && ToLowerAscii(up->name) == lower) found = up.get();
                if (!found) {
                    BanNotice(cmd.target + " ist nicht online.", true);
                    break;
                }
                const std::string name = found->name;
                RemoveByOperator(*found, net::ErrorCode::Kicked, std::string(), "vom Betreiber getrennt");
                BanNotice(name + " wurde vom Server getrennt.");
                break;
            }
        }
    }
}

void GameServer::BanNotice(const std::string& text, bool error) {
    banNotice_ = text;
    banNoticeError_ = error;
    ++banNoticeSeq_;
    Log("Sperren: " + text);
}

void GameServer::RemoveByOperator(Client& c, net::ErrorCode code, const std::string& text, const char* reason) {
    if (c.closing) return;
    if (c.role == Role::Player) LogoutPlayer(c, reason, true, false);
    else if (c.role == Role::Spectator) Log(Fmt("Zuschauer #%u (%s): %s", c.connId, c.ipText.c_str(), reason));
    SendError(c, code, text);
    Disconnect(c);
}

const Ban* GameServer::FindIpBan(uint32_t ip) const {
    const uint32_t readable = ReadableIp(ip);
    const int64_t now = UnixNow();
    for (const Ban& b : accounts_.Bans())
        if (b.isIp && (b.until == 0 || b.until > now) && IpInRange(readable, b.net, b.prefix)) return &b;
    return nullptr;
}

const Ban* GameServer::FindAccountBan(const std::string& name) const {
    const std::string lower = ToLowerAscii(name);
    const int64_t now = UnixNow();
    for (const Ban& b : accounts_.Bans())
        if (!b.isIp && (b.until == 0 || b.until > now) && ToLowerAscii(b.target) == lower) return &b;
    return nullptr;
}

// Text für den Spieler, z. B. "Grund: Beleidigungen im Chat, noch 7 Tage".
// Nur Zeichen, die die Schrift des Spiels kennt (kein Gedankenstrich).
std::string GameServer::BanText(const Ban& b) const {
    std::string text = b.reason.empty() ? std::string() : "Grund: " + b.reason + ", ";
    if (b.until == 0) return text + "dauerhaft";
    const int64_t left = std::max<int64_t>(1, b.until - UnixNow());
    if (left >= 2 * 86400) return text + "noch " + std::to_string((left + 86399) / 86400) + " Tage";
    if (left >= 2 * 3600) return text + "noch " + std::to_string((left + 3599) / 3600) + " Stunden";
    const int64_t minutes = (left + 59) / 60;
    return text + "noch " + std::to_string(minutes) + (minutes == 1 ? " Minute" : " Minuten");
}

void GameServer::ExpireBans() {
    std::vector<Ban> removed;
    accounts_.RemoveExpiredBans(UnixNow(), removed);
    if (removed.empty()) return;
    for (const Ban& b : removed) Log("Sperren: Sperre für " + b.target + " ist abgelaufen.");
    accounts_.SaveBans();
}

void GameServer::BroadcastSettings() {
    net::SettingsMsg m;
    m.settings = settings_;
    const std::vector<uint8_t> bytes = net::Encode(m);
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.closing || c.role == Role::Lobby) continue;
        Send(c, bytes, true);
    }
}

// Weniger Gegner eingestellt: überzählige Piraten entfernen, die am weitesten
// von allen Spielern entfernten zuerst (die verschwinden, ohne dass es jemand sieht).
// Missionspiraten zählen nicht mit und bleiben (World::Update zählt sie auch nicht).
void GameServer::RemoveSurplusPirates() {
    std::vector<Candidate> pirates;   // dist = Abstand zum nächsten Spieler
    int alive = 0;
    for (size_t i = 0; i < world_.ships.size(); ++i) {
        const Ship& s = world_.ships[i];
        if (s.dead || s.faction != Faction::Pirate || s.mission >= 0) continue;
        ++alive;
        if (s.crashing) continue;   // stürzt sowieso gleich ab
        float nearest = 1e9f;
        for (const PlayerState& ps : world_.players)
            if (const Ship* p = world_.PlayerShip(ps)) nearest = std::min(nearest, Dist2D(p->pos, s.pos));
        pirates.push_back({nearest, i});
    }
    int surplus = alive - world_.settings.enemyShips;
    if (surplus <= 0) return;
    std::sort(pirates.begin(), pirates.end(), [](const Candidate& a, const Candidate& b) { return a.dist > b.dist; });
    std::vector<int> removeIds;
    for (const Candidate& k : pirates) {
        if (surplus-- <= 0) break;
        removeIds.push_back(world_.ships[k.index].id);
    }
    if (removeIds.empty()) return;   // alle überzähligen stürzen gerade ohnehin ab
    // Direkt aus der Liste löschen. KI-Ziele und Geschosse verweisen nur über
    // die ID auf Schiffe; FindShip liefert für entfernte Schiffe nullptr.
    world_.ships.erase(std::remove_if(world_.ships.begin(), world_.ships.end(),
                                      [&](const Ship& s) {
                                          return std::find(removeIds.begin(), removeIds.end(), s.id) != removeIds.end();
                                      }),
                       world_.ships.end());
    Log(Fmt("%d Pirat(en) entfernt (jetzt höchstens %d)", static_cast<int>(removeIds.size()), world_.settings.enemyShips));
}

// Stationen oder Mondfarbe geändert (docs/Mehrspieler.md Abschnitt 12): neue Welt mit neuem Seed.
// Alle Spieler behalten ihren Fortschritt und starten an einer Handelsstation.
void GameServer::RebuildWorld() {
    UpdateDeparting(true);   // Schiffe abgemeldeter Spieler gehören zur alten Welt
    struct Keep {
        Client* c;
        PlayerProgress progress;
    };
    std::vector<Keep> keep;
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.role != Role::Player) continue;
        PlayerState* ps = world_.FindPlayer(c.playerId);
        if (!ps) continue;
        // wie beim Abmelden: ein laufender Absturz wird nicht "weggezaubert"
        CrashOnLogout(*ps);
        StoreProgress(c);
        keep.push_back({&c, CaptureProgress(*ps)});
    }

    const long long delivered = world_.totalDelivered;
    const int stage = world_.lastStage;
    const uint32_t seed = RandomSeed();
    world_.NewServerWorld(seed, settings_);
    world_.totalDelivered = delivered;   // Ausbaustufe bleibt erhalten
    world_.lastStage = stage;
    accounts_.SetWorldSeed(seed);
    accounts_.SetTotalDelivered(delivered);
    accounts_.SaveServerData();
    accounts_.SaveAccounts();

    for (Keep& k : keep) {
        Client& c = *k.c;
        PlayerState& ps = world_.AddPlayer(c.playerId, c.name);   // gleiche ID wie vorher
        RestoreProgress(ps, k.progress);   // gespeicherte Position gilt nicht (anderer Seed)
        c.inputs.clear();
        c.queueMinInWindow = 1 << 30;
        c.queueWindowSteps = 0;
        c.lastShipId = ps.shipId;
        c.lastDeaths = ps.deaths;
        if (const Ship* s = world_.PlayerShip(ps)) c.lastPos = s->pos;
        if (!c.isBot)
            world_.Notify(ps, "Der Server hat die Karte neu aufgebaut. Dein Fortschritt bleibt erhalten.",
                          Color{200, 220, 255, 255});
    }
    // Alle Clients bauen mit dem neuen Welcome ihre Kulisse neu auf
    const std::vector<uint8_t> players = net::Encode(MakePlayers());
    for (auto& up : clients_) {
        Client& c = *up;
        if (c.closing || c.role == Role::Lobby) continue;
        SendWelcome(c);
        SendDepositsNow(c);
        Send(c, players, true);
    }
}

// ===========================================================================
// 7. Speichern und Status
// ===========================================================================

// Fortschritt und Anmeldezeit eines Spielers ins Konto übernehmen (nur im
// Speicher; auf die Platte schreibt SaveAccounts).
void GameServer::StoreProgress(Client& c) {
    if (!c.account) return;
    const double now = Now();
    c.account->totalLoginSeconds += std::max(0.0, now - c.loginAccountedUntil);
    c.loginAccountedUntil = now;
    if (const PlayerState* ps = world_.FindPlayer(c.playerId)) c.account->progress = CaptureProgress(*ps);
}

// Alle 30 Sekunden: Falls der Server abstürzt oder der Strom ausfällt, gehen
// höchstens 30 Sekunden Fortschritt verloren.
void GameServer::SaveAll() {
    int online = 0;
    for (auto& up : clients_) {
        if (up->role != Role::Player) continue;
        StoreProgress(*up);
        ++online;
    }
    if (online > 0) accounts_.SaveAccounts();
    if (world_.totalDelivered != savedTotalDelivered_) {
        accounts_.SetTotalDelivered(world_.totalDelivered);
        accounts_.SaveServerData();
        savedTotalDelivered_ = world_.totalDelivered;
    }
}

// Jede Sekunde eine Kopie des Zustands für die Webseite (SharedState).
void GameServer::PublishStatus() {
    if (!shared_) {
        tickMs_.clear();   // ohne Webseite niemand, der die Messwerte abholt
        return;
    }
    const double now = Now();
    const double wall = std::max(1e-3, now - lastStatusAt_);
    lastStatusAt_ = now;

    ServerStatus st;
    st.serverName = settings_.serverName;
    st.uptimeSeconds = now;
    st.tickRate = net::TICK_RATE;
    double sum = 0.0, mx = 0.0;
    for (double t : tickMs_) {
        sum += t;
        mx = std::max(mx, t);
    }
    st.avgTickMs = tickMs_.empty() ? 0.0 : sum / static_cast<double>(tickMs_.size());
    st.maxTickMs = mx;
    st.loadPercent = st.avgTickMs / (1000.0 / net::TICK_RATE) * 100.0;
    tickMs_.clear();

    const double cpu = ProcessCpuSeconds();
    st.cpuPercent = std::max(0.0, (cpu - lastCpuSeconds_) / wall * 100.0);
    lastCpuSeconds_ = cpu;
    st.memoryMB = ProcessMemoryMB();
    st.cpuCores = static_cast<int>(std::thread::hardware_concurrency());

    st.playersOnline = PlayersOnline();
    st.maxPlayers = settings_.maxPlayers;
    st.maxPlayersLimit = limits::MAX_PLAYERS_LIMIT;
    st.spectators = SpectatorsOnline();
    st.maxSpectators = limits::MAX_SPECTATORS;
    if (host_) {
        // ENet zählt gesendete/empfangene Bytes (inkl. ENet-Kopf). Wir setzen
        // die Zähler nach jedem Ablesen zurück (32 Bit würden sonst überlaufen).
        st.netOutKBs = host_->totalSentData / 1024.0 / wall;
        st.netInKBs = host_->totalReceivedData / 1024.0 / wall;
        host_->totalSentData = 0;
        host_->totalReceivedData = 0;
    }
    st.totalDelivered = world_.totalDelivered;
    st.stationStage = world_.StationStage();
    for (const Ship& s : world_.ships)
        if (s.faction == Faction::Pirate && !s.dead && !s.crashing) ++st.enemiesAlive;
    st.registeredAccounts = accounts_.Count();
    st.gamePort = config_.port;

    for (const auto& up : clients_) {
        const Client& c = *up;
        if (c.role != Role::Player) continue;
        const PlayerState* ps = world_.FindPlayer(c.playerId);
        if (!ps) continue;
        PlayerStatus p;
        p.id = ps->id;
        p.name = ps->name;
        p.progressPercent = std::clamp(static_cast<double>(ps->balance) * 100.0 / cfg::GOAL_POINTS, 0.0, 100.0);
        p.balance = ps->balance;
        p.round = ps->round;
        for (int u = 0; u < cfg::UP_COUNT; ++u) p.upgrades[u] = ps->upgrades[u];
        p.lives = ps->lives;
        p.shipTime = ps->playTime;
        p.sessionSeconds = now - c.sessionStart;
        // Gesamtzeit = im Konto verbucht + seit der letzten Verbuchung
        p.totalLoginSeconds = (c.account ? c.account->totalLoginSeconds : 0.0) + (now - c.loginAccountedUntil);
        p.kills = ps->kills;
        p.deaths = ps->deaths;
        p.goals = ps->goals;
        p.alive = world_.PlayerAlive(*ps);
        p.docked = ps->dockedAt >= 0;
        p.pingMs = c.peer ? static_cast<int>(c.peer->roundTripTime) : 0;
        p.ip = c.peer ? c.ipText : std::string();
        st.players.push_back(p);
    }
    SortByProgress(st.players);

    for (const HighscoreRecord& h : accounts_.TopHighscores(Accounts::HIGHSCORES_SHOWN)) {
        HighscoreStatus hs;
        hs.name = h.name;
        hs.seconds = h.seconds;
        hs.round = h.round;
        hs.when = h.when;
        st.highscores.push_back(hs);
    }
    st.settings = settings_;
    st.defaults = config_.defaults;
    st.shipModels = ShipModelNames();   // Auswahl im Bereich "Schiffe" (der Web-Thread kennt Models.hpp nicht)
    for (const ServerMap& sm : accounts_.Maps())
        st.maps.push_back({sm.file, sm.map.name, sm.map.author, static_cast<int>(sm.map.pieces.size()), sm.loops});
    st.activeMap = accounts_.ActiveMap();
    st.mapUploaders = accounts_.MapUploaders();
    st.mapNotice = mapNotice_;
    st.mapNoticeSeq = mapNoticeSeq_;
    st.mapNoticeError = mapNoticeError_;
    const int64_t unixNow = UnixNow();
    for (const Ban& b : accounts_.Bans()) {
        if (b.until > 0 && b.until <= unixNow) continue;   // abgelaufen, wird beim nächsten ExpireBans entfernt
        BanStatus bs;
        bs.isIp = b.isIp;
        bs.target = b.target;
        bs.reason = b.reason;
        bs.created = b.created;
        bs.until = b.until;
        st.bans.push_back(bs);
    }
    st.banNotice = banNotice_;
    st.banNoticeSeq = banNoticeSeq_;
    st.banNoticeError = banNoticeError_;
    RecordLoad(st);
    shared_->PublishStatus(st);
}

void GameServer::RecordLoad(const ServerStatus& st) {
    const int64_t minute = UnixNow() / 60;
    if (minute != load_.minute) {
        if (load_.samples > 0) FinishLoadMinute();
        load_ = LoadMinute{};
        load_.minute = minute;
    }
    LoadMinute& m = load_;
    ++m.samples;
    m.tickSum += st.avgTickMs;
    m.tickMax = std::max(m.tickMax, st.maxTickMs);
    m.cpuSum += st.cpuPercent;
    m.cpuMax = std::max(m.cpuMax, st.cpuPercent);
    m.memMax = std::max(m.memMax, st.memoryMB);
    m.inSum += st.netInKBs;
    m.inMax = std::max(m.inMax, st.netInKBs);
    m.outSum += st.netOutKBs;
    m.outMax = std::max(m.outMax, st.netOutKBs);
    m.players = std::max(m.players, st.playersOnline);
    m.spectators = std::max(m.spectators, st.spectators);
    int connections = 0;
    for (const auto& up : clients_)
        if (up->peer) ++connections;
    m.connections = std::max(m.connections, connections);
}

// Eine Zeile je Minute (Durchschnitt und Höchstwert der Sekundenwerte). Bei
// einer Lastspitze zusätzlich eine Meldung im Log, mit dem Client, an den in
// dieser Minute die meisten Daten gingen.
void GameServer::FinishLoadMinute() {
    const LoadMinute& m = load_;
    const double n = static_cast<double>(std::max(1, m.samples));
    const bool peak = m.tickMax > PEAK_TICK_MS || m.cpuMax > PEAK_CPU_PERCENT;

    const std::time_t t = static_cast<std::time_t>(m.minute * 60);
    const std::time_t oldest = t - static_cast<std::time_t>(Accounts::LOAD_LOG_DAYS) * 86400;
    std::tm tm{}, tmOld{};
#if defined(_WIN32)
    tm = *std::localtime(&t);
    tmOld = *std::localtime(&oldest);
#else
    localtime_r(&t, &tm);
    localtime_r(&oldest, &tmOld);
#endif
    char day[16], oldestDay[16], clock[8];
    std::strftime(day, sizeof(day), "%Y-%m-%d", &tm);
    std::strftime(oldestDay, sizeof(oldestDay), "%Y-%m-%d", &tmOld);
    std::strftime(clock, sizeof(clock), "%H:%M", &tm);

    const std::string line = std::string(day) + " " + clock + ";" + std::to_string(static_cast<long long>(t)) + ";" +
                             std::to_string(m.players) + ";" + std::to_string(m.spectators) + ";" +
                             std::to_string(m.connections) + ";" + DecimalComma(m.tickSum / n, 2) + ";" +
                             DecimalComma(m.tickMax, 2) + ";" + DecimalComma(m.cpuSum / n, 1) + ";" +
                             DecimalComma(m.cpuMax, 1) + ";" + DecimalComma(m.memMax, 1) + ";" +
                             DecimalComma(m.inSum / n, 1) + ";" + DecimalComma(m.inMax, 1) + ";" +
                             DecimalComma(m.outSum / n, 1) + ";" + DecimalComma(m.outMax, 1) + ";" +
                             (peak ? "ja" : "") + "\n";
    accounts_.AppendLoadLog(day, oldestDay, LOAD_LOG_HEADER, line);

    if (peak) {
        const Client* top = nullptr;
        uint64_t topBytes = 0;
        for (const auto& up : clients_) {
            const uint64_t sent = up->bytesSent - up->bytesSentMinuteStart;
            if (up->peer && sent > topBytes) {
                top = up.get();
                topBytes = sent;
            }
        }
        std::string who = "–";
        if (top) {
            const char* role = top->role == Role::Player ? top->name.c_str()
                               : top->role == Role::Spectator ? "Zuschauer" : "ohne Anmeldung";
            who = Fmt("%s (%s, %.0f KB)", role, top->ipText.c_str(), static_cast<double>(topBytes) / 1024.0);
        }
        Log(Fmt("Lastspitze um %s: Schritt max. %.1f ms (Ø %.2f ms), CPU max. %.0f %%, %d Spieler, %d Zuschauer, "
                "%d Verbindungen; meiste Daten an: %s",
                clock, m.tickMax, m.tickSum / n, m.cpuMax, m.players, m.spectators, m.connections, who.c_str()));
    }
    for (auto& up : clients_) up->bytesSentMinuteStart = up->bytesSent;
}

// ===========================================================================
// 8. Benchmark
// ===========================================================================
void GameServer::AddBot(int index) {
    auto up = std::make_unique<Client>();
    Client& c = *up;
    c.connId = nextConnId_++;
    c.isBot = true;
    c.ipText = "bot";
    c.bot.rng.seed(static_cast<unsigned>(1000 + index));   // fest: Messungen sind vergleichbar
    clients_.push_back(std::move(up));
    LoginPlayer(c, nullptr, Fmt("Bot%03d", index + 1));
}

// Bot-"Gehirn": erzeugt Eingaben wie ein Mensch, der herumfliegt, kämpft,
// scannt und abbaut. Die Eingaben gehen über HandleInput in die
// Warteschlange – derselbe Weg wie bei echten Netzwerk-Paketen.
void GameServer::BotThink(Client& c) {
    BotBrain& b = c.bot;
    PlayerState* ps = world_.FindPlayer(c.playerId);
    if (!ps) return;
    const float dt = net::TICK_DT;
    auto rnd = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(b.rng); };

    // Angedockt: wie im Stationsfenster etwas kaufen/reparieren, dann abdocken
    if (ps->dockedAt >= 0) {
        b.dockedT += dt;
        if (b.dockedT > 1.5f) {
            net::DockActionMsg a;
            const bool trade = world_.stations[ps->dockedAt].type == StationType::Trade;
            a.action = trade ? net::DockActionType::Repair : net::DockActionType::BuyUpgrade;
            a.param = static_cast<uint8_t>(b.rng() % cfg::UP_COUNT);
            HandleDockAction(c, a);
            a.action = net::DockActionType::Undock;
            HandleDockAction(c, a);
            b.dockedT = 0.0f;
        }
    }

    b.changeT -= dt;
    if (b.changeT <= 0.0f) {
        const float r = rnd(0.0f, 1.0f);
        b.thrust = r < 0.75f ? 1.0f : (r < 0.9f ? 0.0f : -0.5f);   // meistens Vollgas
        b.turn = rnd(0.0f, 1.0f) < 0.35f ? 0.0f : rnd(-1.0f, 1.0f);
        b.fire = rnd(0.0f, 1.0f) < 0.6f;
        b.changeT = rnd(0.4f, 2.5f);
    }
    float turn = b.turn;
    if (const Ship* s = world_.PlayerShip(*ps)) {
        // Nicht am Kartenrand kleben bleiben: zur Mitte zurückdrehen
        const float edge = cfg::WORLD_HALF - 25.0f;
        if (std::fabs(s->pos.x) > edge || std::fabs(s->pos.z) > edge) {
            const float want = YawTowards(s->pos, Vector3{0.0f, 0.0f, 0.0f});
            turn = std::clamp(AngleDiff(s->yaw, want) * 2.0f, -1.0f, 1.0f);
        }
    }
    b.scanT -= dt;
    if (b.scanT <= 0.0f) {
        ++b.scanPresses;
        b.scanT = rnd(3.0f, 9.0f);
    }
    b.interactT -= dt;
    if (b.interactT <= 0.0f) {
        ++b.interactPresses;
        b.interactT = rnd(1.5f, 5.0f);
    }

    net::InputRecord rec;
    rec.seq = ++b.seq;
    rec.thrust = b.thrust;
    rec.turn = turn;
    rec.fire = b.fire;
    rec.scanPresses = b.scanPresses;
    rec.interactPresses = b.interactPresses;
    b.recent.push_back(rec);
    while (b.recent.size() > static_cast<size_t>(net::MAX_INPUTS_PER_MSG)) b.recent.pop_front();
    net::InputMsg msg;
    msg.records.assign(b.recent.begin(), b.recent.end());
    HandleInput(c, msg);
}

int GameServer::RunBenchmark(int players, double seconds, const GameSettings& settings) {
    players = std::clamp(players, 1, 1000);
    seconds = std::clamp(seconds, 1.0, 3600.0);

    Accounts accounts("");                 // nur im Speicher, nichts auf der Platte
    accounts.SetWorldSeed(20251011u);      // fester Seed: Ergebnisse verschiedener Rechner vergleichbar
    ServerConfig cfgServer;
    cfgServer.settings = settings;
    cfgServer.defaults = settings;
    GameServer gs(cfgServer, accounts, nullptr);
    gs.benchmark_ = true;
    gs.InitWorld();
    for (int i = 0; i < players; ++i) gs.AddBot(i);
    // Begrüßungsnachrichten nicht mitzählen, nur den laufenden Betrieb
    for (auto& up : gs.clients_) up->bytesSent = up->packetsSent = up->snapshotBytes = up->snapshotCount = up->eventBytes = 0;

    const GameSettings& s = gs.settings_;
    std::cout << "Mondschürfer-Benchmark (ohne Netzwerk)\n"
              << "  " << players << " Bot-Spieler, " << seconds << " s Spielzeit, Schwierigkeit "
              << DifficultyName(s.difficulty) << " (" << s.enemyShips << " feindliche Schiffe), " << s.tradeStations
              << " Handelsstation(en), " << s.shipyards << " Werft(en)\n"
              << "  Rechner: " << std::thread::hardware_concurrency() << " CPU-Kerne (der Spiel-Thread nutzt einen)\n"
              << "  läuft ..." << std::endl;
    if (players > limits::MAX_PLAYERS_LIMIT)
        std::cout << "  Hinweis: Im echten Betrieb sind höchstens " << limits::MAX_PLAYERS_LIMIT << " Spieler erlaubt.\n";

    const int steps = std::max(1, static_cast<int>(std::lround(seconds * net::TICK_RATE)));
    std::vector<double> ms;
    ms.reserve(static_cast<size_t>(steps));
    size_t shipsSum = 0, bulletsSum = 0;
    const auto wallStart = std::chrono::steady_clock::now();
    for (int i = 0; i < steps; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        gs.Step();
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        shipsSum += gs.world_.ships.size();
        bulletsSum += gs.world_.bullets.size();
    }
    const double wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();

    // --- Auswertung Rechenzeit ---
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double v : ms) sum += v;
    const double avg = sum / static_cast<double>(ms.size());
    const double p95 = sorted[std::min(sorted.size() - 1, static_cast<size_t>(0.95 * static_cast<double>(sorted.size())))];
    const double mx = sorted.back();
    const double budget = 1000.0 / net::TICK_RATE;

    // --- Auswertung Datenmenge (Upload des Servers) ---
    const double simSeconds = steps * static_cast<double>(net::TICK_DT);
    double bytes = 0.0, packets = 0.0, snapBytes = 0.0, snaps = 0.0, eventBytes = 0.0;
    for (auto& up : gs.clients_) {
        eventBytes += static_cast<double>(up->eventBytes);
        bytes += static_cast<double>(up->bytesSent);
        packets += static_cast<double>(up->packetsSent);
        snapBytes += static_cast<double>(up->snapshotBytes);
        snaps += static_cast<double>(up->snapshotCount);
    }
    const double perPlayerBps = (bytes + packets * PACKET_OVERHEAD_BYTES) / simSeconds / players;   // Byte/s
    const double totalBps = perPlayerBps * players;
    const double totalMbit = totalBps * 8.0 / 1e6;
    const double perPlayerMbit = perPlayerBps * 8.0 / 1e6;
    // Download (Eingaben der Clients): 30 Input-Pakete/s mit bis zu 4 Einträgen à 9 Byte
    const double inputBps = net::TICK_RATE * (2.0 + 4 * 9.0 + PACKET_OVERHEAD_BYTES);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\nRechenzeit pro Schritt (Simulation + alle Nachrichten bauen und kodieren):\n"
              << "  Durchschnitt   " << avg << " ms\n"
              << "  95 %           " << p95 << " ms\n"
              << "  Maximum        " << mx << " ms\n"
              << "  Auslastung     " << avg / budget * 100.0 << " % (95 %: " << p95 / budget * 100.0
              << " %) des Zeitbudgets von " << budget << " ms\n"
              << "  (" << steps << " Schritte in " << wallSeconds << " s gerechnet; Ø "
              << static_cast<double>(shipsSum) / steps << " Schiffe, " << static_cast<double>(bulletsSum) / steps
              << " Geschosse)\n";
    std::cout << std::setprecision(1);
    std::cout << "\nDaten pro Spieler (Server -> Client):\n"
              << "  Snapshot Ø          " << (snaps > 0 ? snapBytes / snaps : 0.0) << " Byte (" << net::TICK_RATE / net::SNAPSHOT_EVERY
              << " pro Sekunde)\n"
              << "  Upload Ø            " << perPlayerBps / 1024.0 << " KB/s pro Spieler (" << perPlayerMbit * 1000.0
              << " kbit/s, inkl. Ereignisse, Lagerstätten, Spielerliste 1/s und ca. " << PACKET_OVERHEAD_BYTES
              << " Byte UDP/IP/ENet je Paket)\n"
              << "  Upload gesamt       " << totalBps / 1024.0 << " KB/s = " << std::setprecision(2) << totalMbit
              << " Mbit/s für " << players << " Spieler\n"
              << std::setprecision(1) << "  Aufteilung          Snapshots " << snapBytes / std::max(1.0, bytes) * 100.0
              << " %, Ereignisse " << eventBytes / std::max(1.0, bytes) * 100.0 << " %, Rest (Lagerstätten, Listen) "
              << (bytes - snapBytes - eventBytes) / std::max(1.0, bytes) * 100.0 << " % (ohne Paket-Overhead)\n"
              << "  Download (Eingaben) ca. " << inputBps / 1024.0 << " KB/s pro Spieler\n";

    // --- Einschätzung ---
    const double load95 = p95 / budget * 100.0;
    std::cout << "\nEinschätzung:\n";
    if (load95 < 25.0)
        std::cout << "  Rechenzeit: unkritisch – dieser Rechner schafft " << players << " Spieler mit viel Reserve.\n";
    else if (load95 < 60.0)
        std::cout << "  Rechenzeit: ausreichend für " << players << " Spieler, aber mit wenig Reserve für Kampfspitzen.\n";
    else
        std::cout << "  Rechenzeit: zu knapp für " << players
                  << " Spieler – MAX_PLAYERS verringern oder einen schnelleren Rechner verwenden.\n";
    // Nicht über die gemessene Spielerzahl hinaus hochrechnen: Mit mehr
    // Spielern sieht jeder auch mehr Schiffe, die Daten pro Spieler steigen.
    auto fit = [&](double mbit) {
        const int n = static_cast<int>(mbit * 0.7 / std::max(1e-9, perPlayerMbit));
        return n >= players ? std::string("reicht für alle ") + std::to_string(players) + " Spieler"
                            : "reicht für ca. " + std::to_string(n) + " Spieler";
    };
    std::cout << "  Netzwerk: " << std::setprecision(2) << totalMbit << " Mbit/s Upload. Daumenregel (70 % der Leitung):\n"
              << "    Internet mit 10 Mbit/s Upload  -> " << fit(10.0) << "\n"
              << "    Internet mit 40 Mbit/s Upload  -> " << fit(40.0) << "\n"
              << "    LAN (100 Mbit/s, Kabel)        -> " << fit(100.0) << "\n"
              << "    WLAN: eher die Hälfte rechnen; Raspberry Pi bevorzugt per Kabel anschließen.\n"
              << "  Hinweis: Die Bots fliegen dicht beieinander und schießen oft; echte Spieler verteilen\n"
              << "  sich meist mehr. Die Werte sind daher eher eine Obergrenze.\n"
              << "  Messen auf dem Zielsystem: mondschuerfer-server --benchmark " << players << "\n";
    return 0;
}
