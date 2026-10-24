// ---------------------------------------------------------------------------
// NetClient.cpp – Netzwerk-Client des Spiels (Schnittstelle: NetClient.hpp).
//
// Wofür ist die Datei da?
//   Sie verbindet das Spiel über ENet (UDP) mit dem Mehrspieler-Server und
//   übersetzt dessen Nachrichten (shared/Protocol.hpp) in eine "Spiegelwelt"
//   (World mit isMirror = true). Render.cpp und Hud.cpp zeichnen diese Welt
//   genauso wie im Einzelspieler; sie merken nicht, dass die Daten vom Server
//   kommen.
//
// Wer benutzt sie?
//   src/Multiplayer.cpp (Menüs, Anzeige). Es ruft jedes Frame Update() auf
//   und holt danach Töne, Zielmeldungen usw. mit den Take...()-Funktionen ab.
//
// Abhängigkeiten:
//   - ENet (third_party/enet): Verbindung, zuverlässige/unzuverlässige Kanäle.
//   - shared/Protocol.hpp: Nachrichten kodieren/dekodieren.
//   - shared/Crypto.hpp: Anmeldeschlüssel (PBKDF2) und Challenge-Antwort (HMAC).
//   - shared/World.hpp: Spiegelwelt, Schiffsphysik für die Vorhersage,
//     Partikel (Explosion/Funken) und Meldungen.
//   - Keine raylib-FUNKTIONEN (nur Datentypen wie Vector3/Color und die
//     Inline-Rechenfunktionen aus raymath.h). Dadurch lässt sich der Client
//     ohne Fenster testen. Die Zeit kommt aus std::chrono::steady_clock.
//
// Threads:
//   Alles läuft im Spiel-Thread (dem Thread, der Update() aufruft). ENet wird
//   nur von dort benutzt; es gibt keinen Hintergrund-Thread. Einzige Ausnahme
//   ist der Referenzzähler für enet_initialize/enet_deinitialize, der mit
//   einem Mutex geschützt ist (mehrere NetClient-Objekte sind erlaubt, z. B.
//   in Tests).
//
// Ablauf (Zustände siehe NetState in NetClient.hpp):
//   Connect()          -> Connecting: ENet baut die Verbindung auf (max. 5 s)
//   ServerInfo kommt   -> Lobby: Name, Spielerzahl, Salz für den Schlüssel
//   Register()/Login() -> LoggingIn: Schlüssel berechnen, senden
//   Spectate()         -> LoggingIn
//   UploadMap()        -> LoggingIn, nach MapUploadResult wieder Lobby
//   Welcome kommt      -> Playing bzw. Spectating: Kulisse mit dem Seed des
//                         Servers aufbauen (World::BuildMirror)
//   jedes Frame (Update):
//     1. Netzwerkpakete abarbeiten (Snapshots, Ereignisse, Listen ...)
//     2. Spieler: in festen Schritten von 1/30 s Eingaben senden und das
//        eigene Schiff sofort lokal bewegen ("Vorhersage")
//     3. fremde Schiffe zwischen zwei Snapshots interpolieren, Geschosse
//        weiterrechnen, eigenes Schiff mit weicher Korrektur darstellen
//     4. World::UpdateCosmetics: Rauch, Partikel, Meldungen ablaufen lassen
//   Disconnect()       -> Leave senden, trennen, Disconnected
//
// Die beiden wichtigsten Techniken (ausführlich bei den Funktionen erklärt):
//   - Vorhersage + Abgleich ("Client-side prediction and reconciliation") für
//     das eigene Schiff: StepOwnShip() und ApplyMe().
//   - Interpolation für fremde Schiffe: UpdateRemoteShips().
// ---------------------------------------------------------------------------
#include "NetClient.hpp"
#include "Config.hpp"
#include "Crypto.hpp"
#include "Models.hpp"
#include "Util.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

// ENet bindet unter Windows <winsock2.h>/<windows.h> ein. Die Windows-Header
// definieren Namen, die mit raylib.h (über NetClient.hpp schon eingebunden)
// zusammenstoßen (Rectangle, CloseWindow, DrawText, ShowCursor ...). Mit
// diesen Schaltern lässt Windows die betroffenen Teile (GDI, USER) weg.
// NOMINMAX verhindert die Makros min/max, die std::min/std::max kaputt machen.
// (Gleiches Vorgehen wie in server/src/GameServer.cpp.)
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
#undef near
#undef far
#endif

namespace {

using Clock = std::chrono::steady_clock;

// ===========================================================================
// Einstellungen des Clients
// ===========================================================================
constexpr double CONNECT_TIMEOUT = 5.0;       // s bis "Server nicht erreichbar"
constexpr double LOGIN_TIMEOUT = 10.0;        // s ohne Antwort auf Register/Login/Spectate
constexpr int DISCONNECT_WAIT_MS = 300;       // so lange auf die Bestätigung der Trennung warten
constexpr double SERVER_ERROR_GRACE = 5.0;    // s: Trennung kurz nach einem Error gehört zu diesem Error
constexpr float MAX_FRAME_DT = 0.25f;         // längere Frames (Fenster verschoben ...) werden gekürzt
constexpr float MAX_VIEW_DT = 1.0f / 20.0f;   // Darstellung (Partikel, Korrektur) wie im Einzelspieler
constexpr size_t MAX_PENDING_INPUTS = 90;     // höchstens 3 s unbestätigte Eingaben aufheben
constexpr float CORRECTION_RATE = 10.0f;      // Korrekturversatz *= exp(-10 * dt) (docs/Mehrspieler.md 6.2)
constexpr float CORRECTION_SNAP = 8.0f;       // m: größere Abweichung wird nicht geblendet, sondern gesprungen
constexpr float REMOTE_TELEPORT = 15.0f;      // m zwischen zwei Snapshots: nicht interpolieren
constexpr size_t MAX_FRAMES = 32;             // gepufferte Snapshots für die Interpolation (~2 s)
constexpr double MAX_EXTRAPOLATION = 0.25;    // s: so lange fremde Schiffe bei fehlenden Daten weiterrechnen
constexpr float BULLET_MAX_AGE = 0.35f;       // s: ältere Geschossdaten (Netz hängt) verwerfen
constexpr size_t MAX_QUEUED_SOUNDS = 256;     // falls niemand TakeSounds() aufruft
constexpr size_t MAX_QUEUED_GOALS = 32;
constexpr size_t MAX_QUEUED_CHAT = 64;
constexpr size_t CLIENT_MAX_PACKET = 1024 * 1024;   // Schutz vor riesigen Paketen (ENet-Standard: 32 MB)
constexpr float TIME_SYNC_SMOOTHING = 0.05f;  // Anteil, mit dem jede neue Messung die Zeitschätzung verschiebt
constexpr double TIME_SYNC_RESET = 1.0;       // s Abweichung: Schätzung neu beginnen statt glätten

// ===========================================================================
// ENet starten/beenden mit Referenzzähler
// ===========================================================================
// enet_initialize/enet_deinitialize gelten für das ganze Programm (unter
// Windows: WSAStartup/WSACleanup). Gibt es mehrere NetClient-Objekte (z. B.
// im Test ein Spieler und ein Zuschauer), darf das letzte erst beim Trennen
// deinitialisieren. Daher zählt g_enetUsers, wie viele Verbindungen ENet
// gerade brauchen. Der Mutex schützt nur diesen Zähler.
std::mutex g_enetLock;
int g_enetUsers = 0;

bool AcquireEnet() {
    std::lock_guard<std::mutex> guard(g_enetLock);
    if (g_enetUsers == 0 && enet_initialize() != 0) return false;
    ++g_enetUsers;
    return true;
}

void ReleaseEnet() {
    std::lock_guard<std::mutex> guard(g_enetLock);
    if (g_enetUsers <= 0) return;
    if (--g_enetUsers == 0) enet_deinitialize();
}

// ===========================================================================
// Kleine Hilfsfunktionen
// ===========================================================================
std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

// "host" oder "host:port" zerlegen und prüfen. ENet 1.3 kann nur IPv4; eine
// IPv6-Adresse (mehrere Doppelpunkte) wird deshalb mit einem klaren Hinweis
// abgelehnt statt mit einer unverständlichen Fehlermeldung.
bool ParseAddress(const std::string& text, std::string& host, uint16_t& port, std::string& error) {
    const std::string s = Trim(text);
    if (s.empty()) {
        error = "Bitte eine Serveradresse eingeben (z. B. 192.168.1.20 oder 192.168.1.20:7777).";
        return false;
    }
    if (s.find("://") != std::string::npos) {
        error = "Bitte nur Adresse und Port angeben, ohne \"http://\" (z. B. 192.168.1.20:7777).";
        return false;
    }
    const size_t colon = s.find(':');
    if (colon != std::string::npos && s.find(':', colon + 1) != std::string::npos) {
        error = "IPv6-Adressen werden nicht unterstützt. Bitte eine IPv4-Adresse oder einen Hostnamen angeben.";
        return false;
    }
    host = colon == std::string::npos ? s : Trim(s.substr(0, colon));
    port = net::DEFAULT_PORT;
    if (colon != std::string::npos) {
        const std::string p = Trim(s.substr(colon + 1));
        // Nur Ziffern, höchstens 5 Stück: so kann die Umrechnung nicht überlaufen
        bool digits = !p.empty() && p.size() <= 5;
        for (char c : p)
            if (c < '0' || c > '9') digits = false;
        const long value = digits ? std::stol(p) : 0;
        if (!digits || value < 1 || value > 65535) {
            error = "Ungültiger Port \"" + p + "\" (erlaubt: 1 bis 65535).";
            return false;
        }
        port = static_cast<uint16_t>(value);
    }
    if (host.empty()) {
        error = "Bitte eine Serveradresse vor dem Doppelpunkt angeben.";
        return false;
    }
    // Hostnamen und IPv4-Adressen bestehen nur aus diesen Zeichen. Alles
    // andere (Leerzeichen, "/", "http://" ...) ist ein Tippfehler.
    for (char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '-' || c == '_';
        if (!ok) {
            error = "Ungültige Serveradresse \"" + host + "\".";
            return false;
        }
    }
    return true;
}

// Zeit für Meldungen: "12:34 Min." bzw. "1:02:03 Std."
std::string FormatDuration(float seconds) {
    if (!(seconds >= 0.0f)) seconds = 0.0f;   // fängt auch NaN ab
    const long total = std::lround(seconds);
    const long h = total / 3600;
    const long m = (total / 60) % 60;
    const long s = total % 60;
    if (h > 0) return Fmt("%ld:%02ld:%02ld Std.", h, m, s);
    return Fmt("%ld:%02ld Min.", m, s);
}

// Winkel in den Bereich -PI..PI bringen
float WrapAngle(float a) {
    if (!std::isfinite(a)) return 0.0f;
    a = std::fmod(a + PI, 2.0f * PI);
    if (a < 0.0f) a += 2.0f * PI;
    return a - PI;
}

// Welcher Fraktion gehört ein Schiff dieser Art? (Wie World::MakeShip)
Faction FactionOf(ShipKind k) {
    switch (k) {
        case ShipKind::Player:
        case ShipKind::Convoy: return Faction::Player;
        case ShipKind::Pirate:
        case ShipKind::Elite: return Faction::Pirate;
        case ShipKind::Miner: return Faction::Neutral;
    }
    return Faction::Neutral;
}

// Eine Netzwerk-Eingabe als Eingabe für die Schiffsphysik. Tastendrücke
// (Scanner, Interaktion) spielen für die Bewegung keine Rolle; sie wertet
// nur der Server aus.
ShipInput PhysicsInput(const net::InputRecord& r) {
    ShipInput in;
    in.thrust = r.thrust;
    in.turn = r.turn;
    in.fire = r.fire;
    return in;
}

} // namespace

// ===========================================================================
// Impl: alle Daten und Helfer des Clients
// ===========================================================================
// Warum eine Impl-Struktur ("Pimpl")? Der Header NetClient.hpp muss dann
// weder ENet noch die vielen internen Puffer kennen. Wer NetClient.hpp
// einbindet (Multiplayer.cpp), bekommt kein <winsock2.h> mit seinen
// Namenskonflikten zu raylib.
struct NetClient::Impl {
    Impl();

    // --- Verbindung ----------------------------------------------------------
    ENetHost* host = nullptr;          // unser ENet-"Host" (Client-Seite, 1 Peer)
    ENetPeer* peer = nullptr;          // die Verbindung zum Server
    bool enetAcquired = false;         // hält diese Verbindung einen ENet-Referenzzähler?
    bool connected = false;            // ENet hat CONNECT gemeldet
    NetState state = NetState::Disconnected;
    double connectStart = 0.0;         // Now() beim Connect()
    std::string error;                 // für TakeError()
    bool hasInfo = false;
    net::ServerInfoMsg info;

    // Letzter Error vom Server. Trennt der Server kurz danach (z. B. nach
    // "Zu viele Versuche" oder "Server wird beendet"), zeigen wir diesen Grund
    // statt eines allgemeinen "Verbindung verloren".
    net::ErrorCode lastServerError = net::ErrorCode::None;
    std::string lastServerErrorText;
    double lastServerErrorAt = -1e9;

    // Fehler, die beim Abarbeiten eines Pakets zur Trennung führen, werden erst
    // nach dem Freigeben des Pakets ausgeführt (sonst würde der Host zerstört,
    // während wir noch in seiner Ereignisschleife stecken).
    bool closeRequested = false;
    std::string closeError;
    double serviceErrorSince = -1.0;   // Now() beim ersten Socketfehler in Folge, -1 = keiner

    // --- Anmeldung -------------------------------------------------------------
    enum class LoginKind { None, Register, Login, Spectate, Upload };
    LoginKind loginKind = LoginKind::None;
    double loginStart = 0.0;
    std::string loginName;             // so, wie der Spieler ihn eingegeben hat (geprüft)
    net::Key32 loginKey{};             // nur zwischen Login() und LoginChallenge gespeichert
    bool haveKey = false;
    GameMap uploadMap;                 // UploadMap(): wird mit der Challenge-Antwort geschickt
    bool uploadDone = false;           // Antwort da (TakeUploadResult)
    bool uploadOk = false;
    std::string uploadText;

    // --- Welt -------------------------------------------------------------------
    World world;
    bool worldReady = false;
    bool worldRebuilt = false;         // für TakeWorldRebuilt()
    net::WelcomeMsg welcome;
    uint32_t welcomeTick = 0;          // Snapshots bis einschließlich dieses Schritts gehören zur alten Welt
    uint32_t tickBase = 0;             // Schritt des ersten Welcome dieser Verbindung = Zeit 0 (TickTime)
    bool haveTickBase = false;
    int myId = -1;                     // eigene Spieler-ID, -1 = Zuschauer/keine
    std::string myName;
    net::ModelIds models{};            // Variantennummer (Netz) -> Modell-ID
    std::vector<Deco> baseDecos;       // Zufalls-Deko aus BuildMirror (für den Wechsel zurück zur Standardkarte)

    // --- Listen ---------------------------------------------------------------------
    std::vector<net::PlayerEntry> players;
    std::vector<net::HighscoreEntry> highscores;
    long long totalDelivered = 0;
    int spectators = 0;

    // --- Ereignisse zum Abholen ---------------------------------------------------------
    std::vector<SoundEvent> sounds;
    std::vector<NetClient::GoalNotice> goals;
    bool respawned = false;
    std::vector<net::ChatLineMsg> chat;

    // --- Zeit ---------------------------------------------------------------------------
    Clock::time_point epoch;           // Nullpunkt für Now()
    bool timeSynced = false;
    double timeOffset = 0.0;           // geschätzte Serverzeit = Now() + timeOffset
    double lastRenderTime = -1e9;      // Darstellungszeit läuft nie rückwärts
    uint32_t lastTick = 0;             // neuester Snapshot (Schutz gegen vertauschte Pakete)
    bool anyTick = false;

    // --- Eigenes Schiff: Vorhersage ------------------------------------------------------
    float accumulator = 0.0f;          // noch nicht verbrauchte Zeit (< TICK_DT)
    uint32_t seq = 0;                  // Nummer der letzten erzeugten Eingabe
    uint8_t scanPresses = 0;           // Tastendruck-Zähler (laufen über, siehe Protocol.hpp)
    uint8_t interactPresses = 0;
    std::deque<net::InputRecord> pending;   // gesendet, vom Server noch nicht bestätigt (älteste zuerst)
    bool ownShipActive = false;        // haben wir gerade ein eigenes Schiff?
    Ship pred;                         // vorhergesagter Zustand (Physik), NICHT der dargestellte
    Vector3 prevPos{};                 // Zustand vor dem letzten Schritt: für die Darstellung
    float prevYaw = 0.0f;              // zwischen zwei Schritten (Bildrate > 30/s)
    float prevBank = 0.0f;
    Vector3 corrOffset{};              // Korrekturversatz (weich ausgeblendet)
    float corrYaw = 0.0f;
    float fireCd = 0.0f;               // lokale Abklingzeit der Waffe (nur für den Schusston)
    float lastHp = -1.0f;              // für "Treffer erkannt" -> Bildschirm wackelt

    // --- Fremde Schiffe: Interpolation ------------------------------------------------------
    struct Frame {
        double serverTime = 0.0;           // aus dem Schrittzähler (TickTime)
        std::vector<net::SnapShip> ships;
    };
    std::deque<Frame> frames;                         // gepufferte Snapshots, älteste zuerst
    std::unordered_map<uint32_t, size_t> scratchIndex; // Schiffs-ID -> Index (wiederverwendet)

    // --- Geschosse --------------------------------------------------------------------------
    std::vector<net::SnapBullet> bullets;   // aus dem neuesten Snapshot
    double bulletsAt = 0.0;                 // Now() beim Empfang

    // --- Helfer -------------------------------------------------------------------------------
    double Now() const { return std::chrono::duration<double>(Clock::now() - epoch).count(); }
    void ResetPlaceholderWorld();
    void ResetSession();
    void ResetPrediction();
    void ForgetOwnShip();
    void WipeKey();
    void Send(const std::vector<uint8_t>& bytes, bool reliable);
    void RequestClose(const std::string& err);
    void CloseConnection(bool graceful);
    void BackToLobby(const std::string& err);

    void Service();
    void CheckTimeouts();
    void OnConnect();
    void OnDisconnectEvent();
    void OnPacket(const uint8_t* data, size_t len);

    void HandleServerInfo(const net::ServerInfoMsg& m);
    void HandleError(const net::ErrorMsg& m);
    void HandleChallenge(const net::LoginChallengeMsg& m);
    void HandleWelcome(const net::WelcomeMsg& m);
    void HandleSnapshot(net::SnapshotMsg& m);
    void HandleDeposits(const net::DepositsMsg& m);
    void HandleEvents(const net::EventsMsg& m);
    void HandlePlayers(const net::PlayersMsg& m);
    void HandleHighscores(const net::HighscoresMsg& m);
    void HandleSettings(const net::SettingsMsg& m);
    void HandleMapUploadResult(const net::MapUploadResultMsg& m);
    void HandleMapData(const net::MapDataMsg& m);

    void SyncTime(double serverTime);
    // Serverzeit in Sekunden aus dem Schrittzähler, gezählt ab dem ersten
    // Welcome dieser Verbindung. Ganze Zahlen statt float: bleibt auch nach
    // Tagen auf die Mikrosekunde genau (ein float hätte nach 12 Tagen nur
    // noch eine Auflösung von 1/8 s). "tick - tickBase" rechnet modulo 2^32
    // und ist deshalb auch beim Überlauf des Zählers richtig.
    double TickTime(uint32_t tick) const {
        return static_cast<double>(static_cast<uint32_t>(tick - tickBase)) / net::TICK_RATE;
    }
    void ApplyMe(const net::SnapMe& me);
    void StepOwnShip(const ShipInput& held);
    void UpdateOwnDisplay(float dt);
    void UpdateRemoteShips();
    void UpdateBullets();
    void PushSound(const SoundEvent& e);
};

NetClient::Impl::Impl() : epoch(Clock::now()) {
    models = net::DefaultModelIds();
    ResetPlaceholderWorld();
}

// Vor dem ersten Welcome gibt es noch keine Kulisse. Damit GetWorld().Me()
// und GetWorld().Player() trotzdem gültig sind (players[0], ships[0]), legen
// wir dieselben Platzhalter an wie World::BuildMirror.
void NetClient::Impl::ResetPlaceholderWorld() {
    world = World{};
    world.isMirror = true;
    world.players.push_back(PlayerState{});
    Ship dummy;
    dummy.id = -1;
    dummy.model = models.player;
    dummy.hidden = true;
    dummy.dead = true;
    world.ships.push_back(dummy);
}

// Alles, was nur für EINE Verbindung gilt, auf Anfang setzen (bei Connect).
// Die Welt selbst bleibt stehen, bis ein neues Welcome kommt (WorldReady()
// ist aber false), damit die Anzeige z. B. als Hintergrund weiterzeichnen kann.
void NetClient::Impl::ResetSession() {
    connected = false;
    hasInfo = false;
    info = net::ServerInfoMsg{};
    lastServerError = net::ErrorCode::None;
    lastServerErrorText.clear();
    lastServerErrorAt = -1e9;
    closeRequested = false;
    closeError.clear();
    serviceErrorSince = -1.0;
    loginKind = LoginKind::None;
    loginName.clear();
    WipeKey();
    uploadMap = GameMap{};
    uploadDone = false;
    worldReady = false;
    worldRebuilt = false;
    welcome = net::WelcomeMsg{};
    welcomeTick = 0;
    tickBase = 0;
    haveTickBase = false;
    myId = -1;
    myName.clear();
    players.clear();
    highscores.clear();
    totalDelivered = 0;
    spectators = 0;
    sounds.clear();
    goals.clear();
    respawned = false;
    chat.clear();
    timeSynced = false;
    timeOffset = 0.0;
    lastRenderTime = -1e9;
    lastTick = 0;
    anyTick = false;
    // Neue Verbindung = neue Sitzung auf dem Server: Eingabenummern und
    // Tastenzähler dürfen wieder bei 0 beginnen (der Server setzt seine
    // Gegenstücke bei der Anmeldung ebenfalls zurück).
    seq = 0;
    scanPresses = 0;
    interactPresses = 0;
    ResetPrediction();
    frames.clear();
    bullets.clear();
}

// Vorhersage komplett verwerfen (neue Verbindung, neue Welt): auch die
// unbestätigten Eingaben. Nach einem Welt-Neuaufbau leert der Server seine
// Eingabe-Warteschlange ebenfalls; noch unterwegs befindliche Eingaben würden
// nie bestätigt und dürfen deshalb nicht erneut angewendet werden.
void NetClient::Impl::ResetPrediction() {
    accumulator = 0.0f;
    pending.clear();
    ForgetOwnShip();
}

// Nur das vorhergesagte Schiff vergessen (z. B. neues Schiff nach Absturz).
// Das nächste SnapMe richtet es frisch ein. Die unbestätigten Eingaben
// bleiben: Der Server wendet sie auf das neue Schiff an.
void NetClient::Impl::ForgetOwnShip() {
    ownShipActive = false;
    pred = Ship{};
    corrOffset = Vector3{0, 0, 0};
    corrYaw = 0.0f;
    fireCd = 0.0f;
    lastHp = -1.0f;
}

// Der Anmeldeschlüssel ist so viel wert wie das Passwort (wer ihn kennt, kann
// sich anmelden). Deshalb nach Gebrauch überschreiben.
void NetClient::Impl::WipeKey() {
    loginKey.fill(0);
    haveKey = false;
}

void NetClient::Impl::Send(const std::vector<uint8_t>& bytes, bool reliable) {
    if (!peer || !connected || bytes.empty()) return;
    // Eingaben sind klein (< 40 Byte) und brauchen keine Teilung; flags = 0
    // heißt "unzuverlässig, in Reihenfolge" auf dem unzuverlässigen Kanal.
    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
    if (!packet) return;
    const enet_uint8 channel = static_cast<enet_uint8>(reliable ? net::CHANNEL_RELIABLE : net::CHANNEL_UNRELIABLE);
    // Bei Erfolg gehört das Paket ENet. Schlägt das Senden fehl (Verbindung
    // gerade weg), müssen wir es selbst freigeben.
    if (enet_peer_send(peer, channel, packet) < 0 && packet->referenceCount == 0) enet_packet_destroy(packet);
}

void NetClient::Impl::RequestClose(const std::string& err) {
    closeRequested = true;
    closeError = err;
}

// Verbindung beenden und ENet aufräumen.
//   graceful = true: Disconnect() des Spielers. "Leave" schicken (der Server
//     speichert dann sofort), ENet-Trennung einleiten und kurz auf die
//     Bestätigung warten. So weiß der Server sofort Bescheid und muss nicht
//     erst auf eine Zeitüberschreitung (5–15 s) warten.
//   graceful = false: Fehlerfall. Dem Server nur noch kurz "tschüss" sagen
//     (disconnect_now, ohne Warten) bzw. gar nichts, wenn keine Verbindung bestand.
void NetClient::Impl::CloseConnection(bool graceful) {
    if (host) {
        if (peer) {
            if (connected && graceful) {
                Send(net::EncodeSimple(net::MsgType::Leave), true);
                // Wichtig: disconnect_LATER. Das einfache enet_peer_disconnect
                // leert sofort die Sendewarteschlange und würde das gerade
                // eingereihte Leave wegwerfen. disconnect_later trennt erst,
                // wenn der Server alle zuverlässigen Pakete (also Leave)
                // bestätigt hat. Der Server trennt nach Leave ohnehin selbst.
                enet_peer_disconnect_later(peer, 0);
                enet_host_flush(host);
                // Wartet höchstens DISCONNECT_WAIT_MS. Pakete, die jetzt noch
                // kommen, interessieren nicht mehr und werden nur freigegeben.
                const auto deadline = Clock::now() + std::chrono::milliseconds(DISCONNECT_WAIT_MS);
                bool done = false;
                while (!done && Clock::now() < deadline) {
                    ENetEvent ev;
                    const int r = enet_host_service(host, &ev, 20);
                    if (r < 0) break;
                    if (r == 0) continue;
                    if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
                    else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) done = true;
                }
                if (!done) enet_peer_reset(peer);   // keine Bestätigung: Verbindung hart verwerfen
            } else if (connected) {
                enet_peer_disconnect_now(peer, 0);
            } else {
                enet_peer_reset(peer);
            }
        }
        enet_host_destroy(host);
    }
    host = nullptr;
    peer = nullptr;
    connected = false;
    if (enetAcquired) {
        ReleaseEnet();
        enetAcquired = false;
    }
    state = NetState::Disconnected;
    worldReady = false;
    loginKind = LoginKind::None;
    WipeKey();
    pending.clear();
}

// Anmeldung/Zuschauen hat nicht geklappt: Die Verbindung bleibt, der Spieler
// kann es in der Lobby erneut versuchen (docs/Mehrspieler.md Abschnitt 8).
void NetClient::Impl::BackToLobby(const std::string& err) {
    error = err;
    loginKind = LoginKind::None;
    WipeKey();
    if (state == NetState::LoggingIn) state = NetState::Lobby;
}

// ===========================================================================
// Netzwerk: Ereignisse von ENet abarbeiten
// ===========================================================================
// enet_host_service mit Wartezeit 0 liefert sofort das nächste Ereignis oder
// 0, wenn keins mehr da ist. Nebenbei verschickt es wartende Pakete und
// wiederholt verlorene zuverlässige Pakete.
void NetClient::Impl::Service() {
    while (host) {
        ENetEvent ev;
        const int r = enet_host_service(host, &ev, 0);
        if (r < 0) {
            // Socketfehler (z. B. Netzwerkkarte weg). Einzelne Fehler: im
            // nächsten Frame erneut versuchen. Hält der Zustand an, kann ENet
            // auch keine Zeitüberschreitung mehr feststellen -> selbst aufgeben.
            if (serviceErrorSince < 0.0) serviceErrorSince = Now();
            else if (Now() - serviceErrorSince > CONNECT_TIMEOUT) {
                error = "Netzwerkfehler: Verbindung zum Server unterbrochen";
                CloseConnection(false);
            }
            break;
        }
        serviceErrorSince = -1.0;
        if (r == 0) break;   // nichts mehr da
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                OnConnect();
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                OnPacket(ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);   // empfangene Pakete gehören uns
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                OnDisconnectEvent();   // zerstört den Host -> Schleife endet
                break;
            default:
                break;
        }
        if (closeRequested) {
            closeRequested = false;
            error = closeError;
            CloseConnection(false);
        }
    }
}

void NetClient::Impl::CheckTimeouts() {
    const double now = Now();
    // Verbindungsaufbau: Bei UDP gibt es keine "Verbindung abgelehnt"-Meldung.
    // Ist der Port zu oder die Adresse falsch, kommt einfach nie eine Antwort.
    // Wir warten deshalb höchstens 5 s (bis ServerInfo da ist).
    if (state == NetState::Connecting && now - connectStart > CONNECT_TIMEOUT) {
        error = "Server nicht erreichbar (Adresse/Port prüfen)";
        CloseConnection(false);
        return;
    }
    if (state == NetState::LoggingIn && now - loginStart > LOGIN_TIMEOUT)
        BackToLobby("Keine Antwort vom Server. Bitte erneut versuchen.");
}

void NetClient::Impl::OnConnect() {
    connected = true;
    // Zeitüberschreitung wie auf dem Server (GameServer::OnConnect): Antwortet die
    // Gegenseite 5–15 s lang nicht, meldet ENet DISCONNECT.
    if (peer) enet_peer_timeout(peer, 32, 5000, 15000);
    // Der Zustand bleibt Connecting, bis ServerInfo kommt (erst dann wissen
    // wir, dass dort wirklich ein Mondschürfer-Server läuft).
}

void NetClient::Impl::OnDisconnectEvent() {
    // ENet hat den Peer bei diesem Ereignis schon zurückgesetzt.
    peer = nullptr;
    const bool wasConnected = connected;
    connected = false;
    if (!wasConnected) {
        // Der Verbindungsaufbau ist gescheitert (ENet hat aufgegeben).
        error = "Server nicht erreichbar (Adresse/Port prüfen)";
    } else if (lastServerError != net::ErrorCode::None && Now() - lastServerErrorAt < SERVER_ERROR_GRACE) {
        // Erwartete Trennung nach einem Error (zu viele Versuche, falsche
        // Version, Server wird beendet ...): dessen Text ist der eigentliche Grund.
        error = lastServerErrorText;
    } else {
        error = "Verbindung zum Server verloren";
    }
    CloseConnection(false);
}

// Ein empfangenes Paket: erstes Byte = Nachrichtentyp, danach die Daten.
// Kaputte Pakete (Decode liefert false) werden einfach verworfen.
void NetClient::Impl::OnPacket(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    net::Reader r(data, len);
    const auto type = static_cast<net::MsgType>(r.U8());
    switch (type) {
        case net::MsgType::ServerInfo: {
            net::ServerInfoMsg m;
            if (net::Decode(r, m)) HandleServerInfo(m);
            break;
        }
        case net::MsgType::Error: {
            net::ErrorMsg m;
            if (net::Decode(r, m)) HandleError(m);
            break;
        }
        case net::MsgType::LoginChallenge: {
            net::LoginChallengeMsg m;
            if (net::Decode(r, m)) HandleChallenge(m);
            break;
        }
        case net::MsgType::Welcome: {
            net::WelcomeMsg m;
            if (net::Decode(r, m)) HandleWelcome(m);
            break;
        }
        case net::MsgType::Snapshot: {
            net::SnapshotMsg m;
            if (net::Decode(r, m)) HandleSnapshot(m);
            break;
        }
        case net::MsgType::Deposits: {
            net::DepositsMsg m;
            if (net::Decode(r, m)) HandleDeposits(m);
            break;
        }
        case net::MsgType::Events: {
            net::EventsMsg m;
            if (net::Decode(r, m)) HandleEvents(m);
            break;
        }
        case net::MsgType::Players: {
            net::PlayersMsg m;
            if (net::Decode(r, m)) HandlePlayers(m);
            break;
        }
        case net::MsgType::Highscores: {
            net::HighscoresMsg m;
            if (net::Decode(r, m)) HandleHighscores(m);
            break;
        }
        case net::MsgType::Settings: {
            net::SettingsMsg m;
            if (net::Decode(r, m)) HandleSettings(m);
            break;
        }
        case net::MsgType::MapUploadResult: {
            net::MapUploadResultMsg m;
            if (net::Decode(r, m)) HandleMapUploadResult(m);
            break;
        }
        case net::MsgType::MapData: {
            net::MapDataMsg m;
            if (net::Decode(r, m)) HandleMapData(m);
            break;
        }
        case net::MsgType::ChatLine: {
            net::ChatLineMsg m;
            if (net::Decode(r, m) && state == NetState::Playing) {
                if (chat.size() >= MAX_QUEUED_CHAT) chat.erase(chat.begin());
                chat.push_back(m);
            }
            break;
        }
        default:
            break;   // unbekannt (z. B. neuere Serverversion): ignorieren
    }
}

// ===========================================================================
// Nachrichten vom Server
// ===========================================================================
void NetClient::Impl::HandleServerInfo(const net::ServerInfoMsg& m) {
    if (m.version != net::PROTOCOL_VERSION) {
        // Eigentlich lehnt der Server eine falsche Version schon beim
        // Verbinden ab. Sicher ist sicher: mit einem anderen Protokoll würden
        // wir alle weiteren Nachrichten falsch verstehen.
        RequestClose(std::string("Verbindung abgelehnt: ") + net::ErrorText(net::ErrorCode::VersionMismatch) +
                     Fmt(" (Server: Protokoll %u, Spiel: Protokoll %u)", static_cast<unsigned>(m.version),
                         static_cast<unsigned>(net::PROTOCOL_VERSION)));
        return;
    }
    info = m;
    hasInfo = true;
    if (!worldReady) totalDelivered = m.totalDelivered;
    if (state == NetState::Connecting) state = NetState::Lobby;
}

void NetClient::Impl::HandleError(const net::ErrorMsg& m) {
    std::string text = net::ErrorText(m.code);
    if (!m.text.empty()) text += " (" + m.text + ")";

    // Bei diesen Fehlern trennt der Server die Verbindung (falsche Version:
    // docs/Mehrspieler.md Abschnitt 3; Server wird beendet: GameServer::Shutdown). Also gar nicht
    // erst in der Lobby bleiben, sondern selbst sauber aufräumen.
    if (m.code == net::ErrorCode::VersionMismatch) text = "Verbindung abgelehnt: " + text;
    if (m.code == net::ErrorCode::ServerShutdown) text = "Verbindung beendet: " + text;

    // Kontext: Wobei ist der Fehler passiert?
    if (state == NetState::LoggingIn) {
        switch (loginKind) {
            case LoginKind::Register: text = "Konto anlegen fehlgeschlagen: " + text; break;
            case LoginKind::Login: text = "Anmeldung fehlgeschlagen: " + text; break;
            case LoginKind::Spectate: text = "Zuschauen nicht möglich: " + text; break;
            case LoginKind::Upload: text = "Hochladen fehlgeschlagen: " + text; break;
            case LoginKind::None: break;
        }
    }
    lastServerError = m.code;
    lastServerErrorText = text;
    lastServerErrorAt = Now();

    if (m.code == net::ErrorCode::VersionMismatch || m.code == net::ErrorCode::ServerShutdown) {
        RequestClose(text);
        return;
    }
    if (state == NetState::LoggingIn) {
        // NameInvalid/NameTaken/ServerFull/WrongLogin/AlreadyOnline/
        // TooManyAttempts/SpectatorsFull: zurück in die Lobby. Trennt der
        // Server danach (5 Fehlversuche), meldet OnDisconnectEvent denselben Text.
        BackToLobby(text);
        return;
    }
    error = text;   // z. B. BadRequest im Spiel: nur anzeigen
}

// Anmelden, Schritt 2: Der Server schickt eine Zufallszahl (Nonce). Wir
// beweisen, dass wir den Schlüssel kennen, ohne ihn zu schicken:
// proof = HMAC(Schlüssel, Nonce + Name). Ein Mithörer kann mit dem Beweis
// nichts anfangen, weil die nächste Anmeldung eine andere Nonce bekommt.
void NetClient::Impl::HandleChallenge(const net::LoginChallengeMsg& m) {
    if (state != NetState::LoggingIn || !haveKey) return;
    if (loginKind == LoginKind::Login) {
        net::LoginProofMsg proof;
        proof.proof = crypto::LoginProof(loginKey, m.nonce, ToLowerAscii(loginName));
        Send(net::Encode(proof), true);
    } else if (loginKind == LoginKind::Upload) {
        // Karte hochladen: derselbe Beweis, aber zusammen mit der Karte
        net::MapUploadMsg up;
        up.proof = crypto::LoginProof(loginKey, m.nonce, ToLowerAscii(loginName));
        up.map = uploadMap;
        Send(net::Encode(up), true);
        uploadMap = GameMap{};
    } else {
        return;
    }
    WipeKey();   // wird nicht mehr gebraucht (eine Nonce gilt nur für einen Versuch)
}

// Antwort auf UploadMap: zurück in die Lobby, das Ergebnis holt der Editor ab.
void NetClient::Impl::HandleMapUploadResult(const net::MapUploadResultMsg& m) {
    if (state != NetState::LoggingIn || loginKind != LoginKind::Upload) return;
    uploadDone = true;
    uploadOk = m.ok;
    uploadText = m.text;
    loginKind = LoginKind::None;
    state = NetState::Lobby;
}

// Aktive Karte des Servers: kommt nach jedem Welcome und wenn der Betreiber
// die Karte wechselt. Erst die Zufalls-Deko aus BuildMirror zurückholen, dann
// die Karte darüberlegen (World::ApplyMap); die Standardkarte ist nur die
// Zufalls-Deko.
void NetClient::Impl::HandleMapData(const net::MapDataMsg& m) {
    if (!worldReady) return;
    world.decos = baseDecos;
    world.monorails.clear();
    if (m.custom) world.ApplyMap(m.map);
}

// Begrüßung: Anmeldung/Zuschauen hat geklappt, oder der Server hat die Welt
// neu aufgebaut (geänderte Einstellungen). In beiden Fällen bauen wir die
// Kulisse mit Seed und Einstellungen des Servers auf. Weil World::BuildMirror
// dieselben Funktionen in derselben Reihenfolge aufruft wie der Server,
// entstehen dieselben Stationen und dieselbe Mondoberfläche.
void NetClient::Impl::HandleWelcome(const net::WelcomeMsg& m) {
    if (state != NetState::LoggingIn && state != NetState::Playing && state != NetState::Spectating) return;
    const bool rebuild = worldReady;
    const float keepDrawRange = world.drawRange;   // gehört der Anzeige (Zuschauer: ganze Karte)

    welcome = m;
    world.BuildMirror(m.worldSeed, m.settings);
    baseDecos = world.decos;
    if (rebuild) world.drawRange = keepDrawRange;
    world.totalDelivered = m.totalDelivered;
    world.lastStage = world.StationStage();
    world.stageAnim = 0.0f;
    totalDelivered = m.totalDelivered;

    const bool player = m.role == net::Role::Player;
    myId = player ? m.playerId : -1;
    if (player && myName.empty()) myName = loginName;
    PlayerState& me = world.Me();
    me.id = myId;
    me.name = player ? myName : std::string();
    me.shipId = -1;

    state = player ? NetState::Playing : NetState::Spectating;
    loginKind = LoginKind::None;
    WipeKey();
    worldReady = true;
    worldRebuilt = true;

    // Alles, was sich auf die alte Welt bezog, verwerfen. Schiffs-IDs der
    // neuen Welt beginnen wieder bei 1 und dürfen nicht mit alten Snapshots
    // vermischt werden. Eingabenummer und Tastenzähler laufen dagegen weiter:
    // Der Server merkt sich deren Stand auch über den Neuaufbau hinweg.
    // Die Zeitachse beginnt beim ersten Welcome und läuft über spätere
    // Welcomes (Neuaufbau der Karte) hinweg weiter. Würde sie neu bei 0
    // beginnen, liefe die Darstellungszeit scheinbar rückwärts.
    if (!haveTickBase) {
        tickBase = m.tick;
        haveTickBase = true;
    }
    welcomeTick = m.tick;
    frames.clear();
    bullets.clear();
    ResetPrediction();
    SyncTime(TickTime(m.tick));
}

// Zeitabgleich mit dem Server. Jeder Snapshot trägt die Serverzeit, zu der er
// entstanden ist. "Serverzeit - lokale Empfangszeit" ist eine Messung des
// Versatzes zwischen beiden Uhren (inklusive Laufzeit im Netz). Einzelne
// Messungen schwanken (Netz-Jitter), daher wird geglättet. Springt die
// Messung sehr weit (Neuverbindung, langer Hänger), fangen wir neu an.
void NetClient::Impl::SyncTime(double serverTime) {
    const double sample = serverTime - Now();
    if (!timeSynced || std::fabs(sample - timeOffset) > TIME_SYNC_RESET) {
        timeOffset = sample;
        timeSynced = true;
        lastRenderTime = -1e9;
    } else {
        timeOffset += (sample - timeOffset) * TIME_SYNC_SMOOTHING;
    }
}

void NetClient::Impl::HandleSnapshot(net::SnapshotMsg& m) {
    if (!worldReady) return;
    // Snapshots, die vor dem (erneuten) Welcome verschickt wurden, beschreiben
    // noch die alte Welt. Sie tragen höchstens den Schritt des Welcome (der
    // Server zählt tick erst nach dem Welcome weiter, bevor er den nächsten
    // Snapshot baut). Vergleich über die Differenz: auch beim Überlauf richtig.
    if (static_cast<int32_t>(m.tick - welcomeTick) <= 0) return;
    // Unzuverlässiger Kanal: Pakete können vertauscht oder doppelt ankommen.
    // Ein älterer Snapshot als der zuletzt verarbeitete bringt nichts Neues.
    if (anyTick && m.tick <= lastTick) return;
    lastTick = m.tick;
    anyTick = true;

    const double serverTime = TickTime(m.tick);
    SyncTime(serverTime);
    world.totalDelivered = m.totalDelivered;
    totalDelivered = m.totalDelivered;

    // Fremde Schiffe in den Puffer (gezeichnet werden sie zeitversetzt, siehe
    // UpdateRemoteShips)
    Frame f;
    f.serverTime = serverTime;
    f.ships = std::move(m.ships);
    frames.push_back(std::move(f));
    while (frames.size() > MAX_FRAMES) frames.pop_front();

    // Geschosse: neuester Stand + Empfangszeit (weitergerechnet in UpdateBullets)
    bullets = m.bullets;
    bulletsAt = Now();

    // Wracks: einfach den neuesten Stand übernehmen (sie bewegen sich nicht)
    world.wrecks.clear();
    world.wrecks.reserve(m.wrecks.size());
    for (const net::SnapWreck& k : m.wrecks) {
        Wreck w;
        w.id = static_cast<int>(k.id);
        w.pos = Vector3{k.x, 0.15f, k.z};   // Höhe wie beim Server (World::Update, Absturz)
        w.yaw = k.yaw;
        w.tilt = k.tilt;
        w.model = net::ShipModel(models, k.variant);
        w.amount = k.amount;
        w.initial = k.initial;
        w.age = k.age;
        world.wrecks.push_back(w);
    }

    if (m.hasMe && state == NetState::Playing) ApplyMe(m.me);
}

// Eigener Zustand vom Server: Konto, Scanner, Abbau ... direkt übernehmen;
// das Schiff selbst mit "Reconciliation" (Abgleich der Vorhersage).
void NetClient::Impl::ApplyMe(const net::SnapMe& me) {
    PlayerState& ps = world.Me();
    ps.id = myId;
    ps.balance = me.balance;
    for (int u = 0; u < cfg::UP_COUNT; ++u)
        ps.upgrades[u] = std::min<int>(me.upgrades[u], cfg::UPGRADE_LEVELS);   // Schutz für Tabellenzugriffe
    ps.lives = me.lives;
    ps.shipType = std::min<int>(me.shipType, world.settings.ShipCount() - 1);
    ps.hangar.clear();
    for (const auto& h : me.hangar) {
        OwnedShip o;
        o.type = std::min<int>(h.type, world.settings.ShipCount() - 1);
        for (int u = 0; u < cfg::UP_COUNT; ++u) o.upgrades[u] = std::min<int>(h.upgrades[u], cfg::UPGRADE_MAX[u]);
        ps.hangar.push_back(o);
    }
    ps.scanEnergy = me.scanEnergy;
    // Scannerring: Die lokale Phase läuft jedes Frame weiter (UpdateOwnDisplay),
    // damit der Ring flüssig wächst. Die Serverphase kommt um die Laufzeit
    // verspätet an; ein hartes Übernehmen würde den Ring bei jedem Snapshot
    // ein Stück zurückspringen lassen. Deshalb nur sanft nachziehen.
    if (!ps.scanOn || !me.scanOn) {
        ps.scanPulse = me.scanPhase;
    } else {
        const float period = cfg::SCANNER_PULSE_TIME;
        float diff = std::fmod(me.scanPhase - ps.scanPulse, period);
        if (diff > period * 0.5f) diff -= period;
        if (diff < -period * 0.5f) diff += period;
        if (std::fabs(diff) > 0.3f) ps.scanPulse = me.scanPhase;
        else ps.scanPulse += diff * 0.25f;
    }
    ps.scanOn = me.scanOn;
    ps.mining = me.mining;
    ps.mineKind = me.mineKind;
    ps.mineId = me.mineId;
    ps.mineT = me.mineT;
    // Stationsindex prüfen: Hud.cpp greift damit direkt in world.stations
    ps.dockedAt = (me.dockedAt >= 0 && me.dockedAt < static_cast<int>(world.stations.size())) ? me.dockedAt : -1;
    ps.lastSale = me.lastSale;
    ps.playTime = me.shipTime;
    ps.roundTime = me.roundTime;
    ps.round = me.round;
    ps.kills = me.kills;
    ps.respawnT = me.respawnT;

    // Mini-Missionen: Angebote und eigene Mission als World::missions
    // nachbauen (IDs nur hier im Client gültig: 1 = eigene Mission), Personen
    // aus den Markierungen (nur Anzeige; eingesammelt wird auf dem Server).
    world.missions.clear();
    ps.mission = -1;
    if (me.missionType >= 0 && me.missionType < cfg::MISSION_TYPE_COUNT) {
        Mission m;
        m.id = 1;
        m.type = static_cast<MissionType>(me.missionType);
        m.owner = ps.id;
        m.progress = me.missionProgress;
        m.goal = me.missionGoal;
        m.alive = me.missionAlive;
        world.missions.push_back(m);
        ps.mission = m.id;
    }
    for (size_t i = 0; i < me.offers.size(); ++i) {
        if (me.offers[i] >= cfg::MISSION_TYPE_COUNT) continue;
        Mission m;
        m.id = static_cast<int>(i) + 2;
        m.type = static_cast<MissionType>(me.offers[i]);
        world.missions.push_back(m);
    }
    ps.markers.clear();
    world.persons.clear();
    for (const auto& mk : me.markers) {
        ps.markers.push_back({mk.kind, Vector3{mk.x, 0.0f, mk.z}});
        if (mk.kind != 0) continue;
        Person pe;
        pe.id = static_cast<int>(world.persons.size()) + 1;
        pe.mission = ps.mission;
        pe.owner = ps.id;
        pe.pos = Vector3{mk.x, 0.0f, mk.z};
        pe.yaw = static_cast<float>(pe.id) * 2.1f;   // fester Blickwinkel je Person
        pe.model = pe.id % 2 == 1 ? world.mdl.astronautA : world.mdl.astronautB;
        world.persons.push_back(pe);
    }
    ps.cue = me.cue;
    ps.cueT = me.cue >= 0 ? me.cueT : 0.0f;

    // Bestätigte Eingaben hat der Server angewendet: Sie stecken schon in
    // seiner Position und werden nicht mehr gebraucht.
    while (!pending.empty() && pending.front().seq <= me.ackSeq) pending.pop_front();

    Ship& shown = world.ships[0];
    if (me.shipId < 0) {
        // Kein Schiff (abgestürzt, wartet auf ein neues): Platzhalter
        // verstecken, aber die letzte bekannte Position behalten (die Kamera
        // bleibt dort, wo das Schiff aufgeschlagen ist).
        ps.shipId = -1;
        ownShipActive = false;
        corrOffset = Vector3{0, 0, 0};
        corrYaw = 0.0f;
        lastHp = -1.0f;
        shown.id = -1;
        shown.hidden = true;
        shown.dead = true;
        shown.crashing = false;
        shown.pos = Vector3{me.x, me.y, me.z};
        return;
    }

    // Erstes Schiff bzw. neues Schiff nach einem Absturz: frisch einrichten
    // (Werte wie World::MakeShip für ShipKind::Player).
    const bool fresh = !ownShipActive || pred.id != me.shipId;
    if (fresh) {
        pred = Ship{};
        pred.id = me.shipId;
        pred.kind = ShipKind::Player;
        pred.faction = Faction::Player;
        pred.model = models.player;
        pred.playerId = myId;
        pred.turnRate = cfg::PLAYER_TURN_RATE;   // wie World::MakeShip
        ownShipActive = true;
        fireCd = 0.0f;
        lastHp = -1.0f;
    }
    ps.shipId = me.shipId;

    // Werte aus Schiffstyp und Aufrüstungen (wie World::ApplyUpgrades).
    // maxSpeed ist für die Vorhersage wichtig: Mit einem anderen Wert als der
    // Server würde das Schiff ständig zurückkorrigiert.
    const ShipType& type = world.settings.Ship(ps.shipType);
    pred.model = ModelId(type.model);
    pred.maxSpeed = ShipSpeed(type, ps.upgrades[cfg::UP_SPEED]);
    pred.damage = ShipDamage(type, ps.upgrades[cfg::UP_WEAPON]);
    pred.fireInterval = cfg::WEAPON_RATE[ps.upgrades[cfg::UP_WEAPON]];
    pred.maxHp = me.maxHp > 0.0f ? me.maxHp : ShipArmor(type, ps.upgrades[cfg::UP_ARMOR]);
    pred.hp = me.hp;
    pred.cargo = me.cargo;
    pred.cargoMax = me.cargoMax;
    pred.hitFlash = me.hitFlash;
    pred.crashing = me.crashing;
    pred.docked = ps.dockedAt >= 0;
    pred.mining = false;   // eigener Abbaustrahl wird aus Me().mining gezeichnet

    // Treffer erkannt (Schild gesunken) -> Bildschirm wackeln lassen, wie
    // World::DamageShip es im Einzelspieler macht. Reparatur/Aufrüstung
    // erhöhen hp und lösen nichts aus.
    if (!fresh && lastHp >= 0.0f && me.hp < lastHp - 0.01f) world.shake = std::min(1.0f, world.shake + 0.45f);
    lastHp = me.hp;

    // Absturz, angedockt oder ganz neues Schiff: Hier gibt es nichts
    // vorherzusagen (der Server bewegt das abstürzende Schiff, angedockt
    // steht es still). Serverwerte direkt übernehmen, auch die Höhe.
    if (fresh || pred.crashing || pred.docked) {
        pred.pos = Vector3{me.x, me.y, me.z};
        pred.vel = Vector3{me.vx, 0.0f, me.vz};
        pred.yaw = me.yaw;
        pred.bank = me.bank;
        pred.thrustVisual = me.thrustVisual;
        // Ein frisches Schiff fliegt evtl. schon: noch unbestätigte Eingaben
        // gleich anwenden, damit es nicht im nächsten Snapshot zurückspringt.
        if (fresh && !pred.crashing && !pred.docked)
            for (const net::InputRecord& rec : pending) world.UpdateShipPhysics(pred, net::TICK_DT, PhysicsInput(rec));
        prevPos = pred.pos;
        prevYaw = pred.yaw;
        prevBank = pred.bank;
        corrOffset = Vector3{0, 0, 0};
        corrYaw = 0.0f;
        return;
    }

    // --- Reconciliation ------------------------------------------------------
    // 1. Auf den Serverzustand zurücksetzen. Der beschreibt das Schiff NACH
    //    der Eingabe ackSeq.
    // 2. Alle Eingaben danach (noch unterwegs zum Server) erneut anwenden.
    //    Ergebnis: unsere beste Schätzung, wo der Server das Schiff gleich
    //    sehen wird. Hat der Server dasselbe gerechnet wie wir, ändert sich
    //    praktisch nichts.
    // 3. Weicht das Ergebnis vom bisher Gezeigten ab (Kollision mit einem
    //    anderen Schiff, verlorenes Paket ...), springt das Bild nicht:
    //    Die Differenz wandert in den Korrekturversatz, der in
    //    UpdateOwnDisplay in Sekundenbruchteilen ausgeblendet wird.
    const Vector3 oldPos = pred.pos;
    const float oldYaw = pred.yaw;
    const float oldBank = pred.bank;
    // Die Höhe ist nur das leichte Auf und Ab (sin(time)); sie hängt an der
    // lokalen Uhr und wird nicht korrigiert, sonst würde das Schiff zittern.
    pred.pos = Vector3{me.x, oldPos.y, me.z};
    pred.vel = Vector3{me.vx, 0.0f, me.vz};
    pred.yaw = me.yaw;
    pred.bank = me.bank;
    pred.thrustVisual = me.thrustVisual;
    for (const net::InputRecord& rec : pending) world.UpdateShipPhysics(pred, net::TICK_DT, PhysicsInput(rec));

    Vector3 shift = Vector3Subtract(pred.pos, oldPos);
    shift.y = 0.0f;
    const float yawShift = AngleDiff(oldYaw, pred.yaw);
    if (Vector3Length(shift) > CORRECTION_SNAP) {
        // Sehr große Abweichung (z. B. lange Netzstörung): lieber einmal
        // springen als sekundenlang durch die Gegend gleiten.
        corrOffset = Vector3{0, 0, 0};
        corrYaw = 0.0f;
        prevPos = pred.pos;
        prevYaw = pred.yaw;
        prevBank = pred.bank;
    } else {
        // Gezeigt wird Lerp(prev, pred) + Versatz. prev und pred verschieben
        // sich um denselben Betrag, der Versatz um das Gegenteil: Das Bild
        // bleibt in diesem Moment exakt gleich.
        corrOffset = Vector3Subtract(corrOffset, shift);
        corrYaw -= yawShift;
        prevPos = Vector3Add(prevPos, shift);
        prevYaw += yawShift;
        prevBank += pred.bank - oldBank;
    }
}

// Lagerstätten: Die Liste ersetzt die alte vollständig (docs/Mehrspieler.md Abschnitt 4). Bekannte
// behalten ihren Einblend-Fortschritt, neue wachsen aus dem Boden (revealT = 0).
void NetClient::Impl::HandleDeposits(const net::DepositsMsg& m) {
    if (!worldReady) return;
    std::unordered_map<int, float> oldReveal;
    oldReveal.reserve(world.deposits.size());
    for (const Deposit& d : world.deposits) oldReveal[d.id] = d.revealT;
    world.deposits.clear();
    world.deposits.reserve(m.deposits.size());
    for (const net::NetDeposit& nd : m.deposits) {
        Deposit d;
        d.id = static_cast<int>(nd.id);
        d.pos = Vector3{nd.x, 0.0f, nd.z};
        d.yaw = nd.yaw;
        d.scale = nd.scale;
        d.model = net::DepositModel(models, nd.variant);
        d.amount = nd.amount;
        d.initial = nd.initial;
        d.revealed = true;   // der Server schickt nur, was wir sehen dürfen
        const auto it = oldReveal.find(d.id);
        d.revealT = it != oldReveal.end() ? it->second : 0.0f;
        world.deposits.push_back(d);
    }
}

void NetClient::Impl::PushSound(const SoundEvent& e) {
    if (sounds.size() >= MAX_QUEUED_SOUNDS) sounds.erase(sounds.begin());   // ältesten verwerfen
    sounds.push_back(e);
}

void NetClient::Impl::HandleEvents(const net::EventsMsg& m) {
    if (!worldReady) return;
    for (const net::NetEvent& e : m.events) {
        switch (e.kind) {
            case net::EventKind::Sound: {
                // Eigene Schüsse sagen wir selbst voraus (StepOwnShip). Der
                // Server lässt sie deshalb weg; falls doch einer kommt, nicht
                // doppelt abspielen.
                if (e.sfx == Sfx::PlayerShot && ownShipActive && e.sourceShip == pred.id) break;
                SoundEvent s;
                s.id = e.sfx;
                s.pos = Vector3{e.x, 0.0f, e.z};   // Audio rechnet nur mit x/z
                s.volume = e.volume;
                s.delay = e.delay;
                s.forPlayer = -1;
                s.sourceShip = e.sourceShip;
                PushSound(s);
                break;
            }
            case net::EventKind::Explosion:
                world.Explosion(Vector3{e.x, e.y, e.z}, e.size);   // erzeugt hier Partikel (kein Server)
                break;
            case net::EventKind::Sparks:
                world.SpawnSparks(Vector3{e.x, e.y, e.z}, e.color, e.count);
                break;
            case net::EventKind::Notice:
                world.Notify(world.Me(), e.text, e.color);
                break;
            case net::EventKind::Goal: {
                NetClient::GoalNotice g;
                g.mine = myId >= 0 && e.playerId == myId;
                g.name = e.text;
                g.seconds = e.seconds;
                g.round = e.round;
                if (goals.size() >= MAX_QUEUED_GOALS) goals.erase(goals.begin());
                goals.push_back(g);
                // Die eigene Erfolgsmeldung schickt der Server selbst als
                // Notice ("ZIEL ERREICHT! ..."). Für andere Spieler eine Zeile:
                if (!g.mine)
                    world.Notify(world.Me(),
                                 Fmt("%s hat das Ziel erreicht (%s, Runde %d)", g.name.c_str(),
                                     FormatDuration(g.seconds).c_str(), g.round),
                                 Color{255, 220, 120, 255});
                break;
            }
            case net::EventKind::StationGrow: {
                const int stage = std::clamp<int>(e.stage, 0, cfg::STATION_STAGES - 1);
                world.lastStage = stage;
                world.stageAnim = 1.0f;   // Render.cpp lässt die neuen Teile "aufwachsen"
                // Ereignisse (zuverlässig) können vor dem Snapshot (unzuverlässig)
                // mit der neuen Liefermenge ankommen. Damit die neue Stufe schon
                // gezeichnet wird, die Menge mindestens auf die Schwelle setzen.
                world.totalDelivered = std::max<long long>(world.totalDelivered, cfg::STATION_THRESHOLDS[stage]);
                totalDelivered = std::max(totalDelivered, world.totalDelivered);
                break;
            }
            case net::EventKind::Respawned:
                respawned = true;
                // Alte Vorhersage gehört zum zerstörten Schiff; das nächste
                // SnapMe mit der neuen Schiffs-ID richtet das neue ein.
                ForgetOwnShip();
                break;
        }
    }
}

void NetClient::Impl::HandlePlayers(const net::PlayersMsg& m) {
    players = m.players;
    totalDelivered = m.totalDelivered;
    if (worldReady) world.totalDelivered = std::max<long long>(world.totalDelivered, m.totalDelivered);
    spectators = m.spectators;
    if (hasInfo) {
        // Info() aktuell halten (z. B. für eine Anzeige "3/16 Spieler")
        info.playersOnline = static_cast<uint8_t>(std::min<size_t>(m.players.size(), 255));
        info.maxPlayers = m.maxPlayers;
        info.spectators = m.spectators;
    }
    if (myId < 0) return;
    for (const net::PlayerEntry& p : players) {
        if (p.id != myId) continue;
        // Der Server kennt die Schreibweise aus der Kontoanlage ("Anna"),
        // auch wenn beim Anmelden "anna" eingetippt wurde.
        myName = p.name;
        if (worldReady) {
            world.Me().name = p.name;
            world.Me().goals = p.goals;
        }
        break;
    }
}

void NetClient::Impl::HandleHighscores(const net::HighscoresMsg& m) { highscores = m.entries; }

// Der Betreiber hat Einstellungen ohne Neuaufbau der Welt geändert (z. B. die
// Schiffe). Übernehmen; die eigenen Schiffswerte folgen mit dem nächsten
// Snapshot (ApplyMe rechnet mit world.settings). Einstellungen, für die die
// Kulisse neu gebaut werden müsste, kommen nur mit einem Welcome.
void NetClient::Impl::HandleSettings(const net::SettingsMsg& m) {
    if (!worldReady || NeedsWorldRebuild(world.settings, m.settings)) return;
    world.settings = m.settings;
    welcome.settings = m.settings;
}

// ===========================================================================
// Eigenes Schiff: Vorhersage
// ===========================================================================
// Ein Simulationsschritt (1/30 s), genau wie auf dem Server:
//   1. Eingabe aus den gehaltenen Tasten und den Tastenzählern bauen,
//      fortlaufend nummerieren und merken (bis der Server sie bestätigt).
//   2. Das eigene Schiff sofort damit bewegen (dieselbe Funktion
//      World::UpdateShipPhysics wie der Server). Dadurch reagiert das Schiff
//      ohne Verzögerung, obwohl die Eingabe erst unterwegs ist.
//   3. Die letzten bis zu 4 unbestätigten Eingaben schicken (unzuverlässig).
//      Geht ein Paket verloren, steckt dieselbe Eingabe im nächsten noch drin.
void NetClient::Impl::StepOwnShip(const ShipInput& held) {
    net::InputRecord rec;
    rec.seq = ++seq;
    rec.thrust = std::isfinite(held.thrust) ? std::clamp(held.thrust, -1.0f, 1.0f) : 0.0f;
    rec.turn = std::isfinite(held.turn) ? std::clamp(held.turn, -1.0f, 1.0f) : 0.0f;
    // Auf die Prozent-Stufen runden, in denen die Eingabe übertragen wird
    // (Protocol.cpp, Encode(InputMsg)). So rechnet die Vorhersage mit genau
    // den Werten, die beim Server ankommen (wichtig bei Gamepad-Sticks;
    // Tasten liefern ohnehin nur -1, 0 oder 1).
    rec.thrust = static_cast<float>(std::lround(rec.thrust * 100.0f)) / 100.0f;
    rec.turn = static_cast<float>(std::lround(rec.turn * 100.0f)) / 100.0f;
    rec.fire = held.fire;
    rec.scanPresses = scanPresses;
    rec.interactPresses = interactPresses;
    pending.push_back(rec);
    while (pending.size() > MAX_PENDING_INPUTS) pending.pop_front();

    prevPos = pred.pos;
    prevYaw = pred.yaw;
    prevBank = pred.bank;
    if (ownShipActive && !pred.crashing && !pred.docked) {
        world.UpdateShipPhysics(pred, net::TICK_DT, PhysicsInput(rec));
        // Schusston sofort selbst erzeugen (wie World::UpdatePlayer: erst
        // Abklingzeit verringern, dann schießen, wenn bereit). Das Geschoss
        // selbst kommt mit dem nächsten Snapshot vom Server.
        fireCd -= net::TICK_DT;
        if (rec.fire && fireCd <= 0.0f) {
            fireCd = pred.fireInterval;
            SoundEvent s;
            s.id = Sfx::PlayerShot;
            s.pos = pred.pos;
            s.forPlayer = myId;
            s.sourceShip = pred.id;
            PushSound(s);
        }
    }

    net::InputMsg msg;
    const size_t n = std::min<size_t>(pending.size(), net::MAX_INPUTS_PER_MSG);
    msg.records.assign(pending.end() - static_cast<std::ptrdiff_t>(n), pending.end());
    Send(net::Encode(msg), false);
}

// Jedes Frame: Was wird vom eigenen Schiff gezeigt?
//   Physik läuft in 1/30-s-Schritten, das Bild meist mit 60 Bildern/s oder
//   mehr. Damit das Schiff nicht ruckelt, wird zwischen dem Zustand vor und
//   nach dem letzten Schritt gemischt (Anteil = übrige Zeit im Akkumulator).
//   Dazu kommt der Korrekturversatz aus der Reconciliation, der exponentiell
//   kleiner wird: nach 0,1 s ist noch etwa ein Drittel übrig, nach 0,3 s fast nichts.
void NetClient::Impl::UpdateOwnDisplay(float dt) {
    const float decay = std::exp(-CORRECTION_RATE * dt);
    corrOffset = Vector3Scale(corrOffset, decay);
    corrYaw *= decay;

    PlayerState& ps = world.Me();
    // Zeiten zwischen zwei Snapshots weiterlaufen lassen (Anzeige im HUD);
    // der nächste Snapshot setzt die genauen Werte.
    if (ps.scanOn) ps.scanPulse = std::fmod(ps.scanPulse + dt, cfg::SCANNER_PULSE_TIME);
    if (ps.mining) ps.mineT = std::min(cfg::MINING_TIME, ps.mineT + dt);
    if (ownShipActive && !pred.crashing) {
        ps.playTime += dt;
        ps.roundTime += dt;
    }
    if (!ownShipActive && ps.respawnT > 0.0f) ps.respawnT = std::max(0.0f, ps.respawnT - dt);

    if (!ownShipActive) return;   // Platzhalter bleibt versteckt (ApplyMe)
    Ship& shown = world.ships[0];
    const float a = std::clamp(accumulator / net::TICK_DT, 0.0f, 1.0f);
    shown = pred;
    shown.pos = Vector3Add(Vector3Lerp(prevPos, pred.pos, a), corrOffset);
    shown.yaw = WrapAngle(prevYaw + AngleDiff(prevYaw, pred.yaw) * a + corrYaw);
    shown.bank = Lerp(prevBank, pred.bank, a);
    shown.hidden = false;
    shown.dead = false;
}

// ===========================================================================
// Fremde Schiffe: Interpolation
// ===========================================================================
// Snapshots kommen 15-mal pro Sekunde (Zuschauer 10-mal), unregelmäßig und
// manchmal gar nicht. Würden wir fremde Schiffe einfach auf die neueste
// Position setzen, sprängen sie sichtbar. Stattdessen zeigen wir sie
// INTERP_DELAY (120 ms) "in der Vergangenheit": Für diese Zeit liegen fast
// immer schon zwei Snapshots vor (davor und danach), zwischen denen wir
// gleichmäßig überblenden.
//
//   Darstellungszeit = geschätzte Serverzeit (Now() + timeOffset) - INTERP_DELAY
//   A = letzter Snapshot mit Zeit <= Darstellungszeit, B = der nächste danach
//   Schiff = A + (B - A) * Anteil      (Drehwinkel über AngleDiff, damit der
//                                       Übergang -PI/+PI keinen Salto ergibt)
//
// Welche Schiffe gezeigt werden, bestimmt B: Neue IDs erscheinen (ohne
// Partner in A: direkt an ihrer Position), verschwundene fallen weg.
// Fehlen Daten (Darstellungszeit schon hinter dem neuesten Snapshot), wird
// kurz mit der letzten Geschwindigkeit weitergerechnet (Extrapolation).
void NetClient::Impl::UpdateRemoteShips() {
    world.ships.resize(1);   // ships[0] = eigenes Schiff bzw. Platzhalter
    if (frames.empty() || !timeSynced) return;

    double renderTime = Now() + timeOffset - net::INTERP_DELAY;
    // Die Zeitschätzung wird ständig leicht nachgeführt. Würde die
    // Darstellungszeit dabei zurücklaufen, liefen Schiffe kurz rückwärts.
    if (renderTime < lastRenderTime) renderTime = lastRenderTime;
    lastRenderTime = renderTime;

    // Neuesten Snapshot mit Zeit <= renderTime suchen
    size_t ia = frames.size();
    for (size_t i = frames.size(); i-- > 0;) {
        if (frames[i].serverTime <= renderTime) {
            ia = i;
            break;
        }
    }
    // Ältere Snapshots werden nicht mehr gebraucht. Einer VOR A bleibt
    // stehen: Falls A der neueste ist, rechnen wir mit ihm weiter (s. u.).
    if (ia != frames.size() && ia > 1) {
        frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(ia - 1));
        ia = 1;
    }
    const Frame* fa;
    const Frame* fb;
    float alpha = 0.0f;
    if (ia == frames.size()) {
        fa = fb = &frames.front();   // Darstellungszeit liegt vor allen Daten (gerade erst begonnen)
    } else if (ia + 1 < frames.size()) {
        // Normalfall: A und B umschließen die Darstellungszeit
        fa = &frames[ia];
        fb = &frames[ia + 1];
        const double span = fb->serverTime - fa->serverTime;
        alpha = span > 1e-5 ? std::clamp(static_cast<float>((renderTime - fa->serverTime) / span), 0.0f, 1.0f) : 1.0f;
    } else if (frames.size() >= 2) {
        // Darstellungszeit liegt schon NACH dem neuesten Snapshot: Es fehlen
        // Daten (meist zwei verlorene Snapshots hintereinander). Würden die
        // Schiffe jetzt stehen bleiben, sprängen sie beim nächsten Snapshot
        // ruckartig nach vorn. Deshalb kurz mit der letzten Geschwindigkeit
        // weiterrechnen (Extrapolation, alpha > 1), höchstens
        // MAX_EXTRAPOLATION Sekunden; danach bleiben sie doch stehen.
        fa = &frames[frames.size() - 2];
        fb = &frames.back();
        const double span = fb->serverTime - fa->serverTime;
        const double ahead = std::min<double>(renderTime - fb->serverTime, MAX_EXTRAPOLATION);
        alpha = span > 1e-5 ? 1.0f + static_cast<float>(ahead / span) : 1.0f;
    } else {
        fa = fb = &frames.back();    // nur ein Snapshot bekannt: dort zeigen
    }

    scratchIndex.clear();
    if (fa != fb)
        for (size_t i = 0; i < fa->ships.size(); ++i) scratchIndex[fa->ships[i].id] = i;
    const float span = static_cast<float>(fb->serverTime - fa->serverTime);   // höchstens ~1 s: float genügt

    world.ships.reserve(1 + fb->ships.size());
    for (const net::SnapShip& b : fb->ships) {
        // Das eigene Schiff steht in ships[0] (der Server lässt es im
        // Snapshot ohnehin weg; doppelt hält besser).
        if (ownShipActive && static_cast<int>(b.id) == pred.id) continue;
        const net::SnapShip* a = nullptr;
        if (fa != fb) {
            const auto it = scratchIndex.find(b.id);
            if (it != scratchIndex.end()) a = &fa->ships[it->second];
        }
        Ship s;
        s.id = static_cast<int>(b.id);
        s.kind = b.kind <= static_cast<uint8_t>(ShipKind::Convoy) ? static_cast<ShipKind>(b.kind) : ShipKind::Pirate;
        s.faction = FactionOf(s.kind);
        s.model = net::ShipModel(models, b.variant);
        s.playerId = b.playerId;
        s.crashing = b.crashing;
        s.hostileToPlayer = b.hostile;
        s.mining = b.mining;
        s.mineTarget = Vector3{b.mineX, 0.0f, b.mineZ};
        s.docked = b.docked;
        s.chatting = b.chatting;
        const Vector3 pb{b.x, b.y, b.z};
        float hpFrac = b.hpFrac;
        if (a && Vector2Distance({a->x, a->z}, {b.x, b.z}) < REMOTE_TELEPORT) {
            // Position und Winkel dürfen über B hinaus weitergerechnet werden
            // (alpha > 1, siehe oben), Anzeigewerte wie Schild nicht.
            const float k = std::min(alpha, 1.0f);
            const Vector3 pa{a->x, a->y, a->z};
            s.pos = Vector3Lerp(pa, pb, alpha);
            s.pos.y = std::max(0.3f, s.pos.y);   // abstürzende Schiffe nicht unter den Boden rechnen
            s.yaw = WrapAngle(a->yaw + AngleDiff(a->yaw, b.yaw) * alpha);
            s.bank = WrapAngle(a->bank + AngleDiff(a->bank, b.bank) * k);
            s.thrustVisual = Lerp(a->thrustVisual, b.thrustVisual, k);
            s.hitFlash = Lerp(a->hitFlash, b.hitFlash, k);
            hpFrac = Lerp(a->hpFrac, b.hpFrac, k);
            if (span > 1e-5f) s.vel = Vector3Scale(Vector3Subtract(pb, pa), 1.0f / span);
            s.vel.y = 0.0f;
        } else {
            s.pos = pb;
            s.yaw = b.yaw;
            s.bank = b.bank;
            s.thrustVisual = b.thrustVisual;
            s.hitFlash = b.hitFlash;
        }
        // Der Snapshot enthält nur den Anteil des Schilds (für Balken über
        // fremden Schiffen genügt das)
        s.maxHp = 100.0f;
        s.hp = hpFrac * 100.0f;
        world.ships.push_back(s);
    }
}

// Geschosse fliegen geradeaus mit fester Geschwindigkeit. Zwischen zwei
// Snapshots rechnen wir sie einfach weiter: Position + vel * Alter.
// Typ -> Fraktion wie auf dem Server: 0 Spieler, 1 Pirat, 2 Händler,
// 3 Geschützturm (ownerId -1, so erkennt Render.cpp die Turmgeschosse).
void NetClient::Impl::UpdateBullets() {
    world.bullets.clear();
    if (bullets.empty()) return;
    const float age = static_cast<float>(Now() - bulletsAt);
    if (age > BULLET_MAX_AGE) {
        // Seit einer Weile kein Snapshot mehr: lieber keine Geschosse als
        // welche, die endlos geradeaus fliegen.
        bullets.clear();
        return;
    }
    world.bullets.reserve(bullets.size());
    for (const net::SnapBullet& sb : bullets) {
        Bullet b;
        b.pos = Vector3{sb.x + sb.vx * age, sb.y, sb.z + sb.vz * age};
        b.vel = Vector3{sb.vx, 0.0f, sb.vz};
        b.life = cfg::BULLET_LIFE;
        switch (sb.type) {
            case 0: b.faction = Faction::Player; b.ownerId = 0; break;
            case 1: b.faction = Faction::Pirate; b.ownerId = 0; break;
            case 3: b.faction = Faction::Neutral; b.ownerId = -1; break;
            default: b.faction = Faction::Neutral; b.ownerId = 0; break;
        }
        world.bullets.push_back(b);
    }
}

// ===========================================================================
// NetClient: öffentliche Schnittstelle
// ===========================================================================
NetClient::NetClient() : impl_(std::make_unique<Impl>()) {}

NetClient::~NetClient() { Disconnect(); }

bool NetClient::Connect(const std::string& address) {
    Impl& d = *impl_;
    if (d.host) Disconnect();   // alte Verbindung sauber beenden
    d.ResetSession();
    d.error.clear();

    std::string host;
    uint16_t port = 0;
    if (!ParseAddress(address, host, port, d.error)) return false;

    if (!AcquireEnet()) {
        d.error = "Netzwerk konnte nicht gestartet werden (ENet).";
        return false;
    }
    d.enetAcquired = true;

    // Namensauflösung (DNS) bzw. Umwandlung "192.168.1.20" -> Adresse.
    // Bei Hostnamen kann das kurz blockieren; passiert nur einmal beim Verbinden.
    ENetAddress addr{};
    addr.port = port;
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        d.error = "Server \"" + host + "\" nicht gefunden (Adresse prüfen).";
        d.CloseConnection(false);
        return false;
    }
    // Client-Host: keine eigene Adresse (beliebiger freier Port), genau ein
    // Peer (der Server), zwei Kanäle, Bandbreite unbegrenzt.
    d.host = enet_host_create(nullptr, 1, net::CHANNEL_COUNT, 0, 0);
    if (!d.host) {
        d.error = "Netzwerk-Socket konnte nicht geöffnet werden.";
        d.CloseConnection(false);
        return false;
    }
    // Ein Server (oder jemand, der sich als einer ausgibt) soll uns nicht mit
    // riesigen Paketen den Speicher füllen. Größter echter Snapshot: ~25 KB.
    d.host->maximumPacketSize = CLIENT_MAX_PACKET;
    // Verbindungsdaten = Protokollversion: Der Server lehnt eine falsche ab.
    d.peer = enet_host_connect(d.host, &addr, net::CHANNEL_COUNT, net::PROTOCOL_VERSION);
    if (!d.peer) {
        d.error = "Verbindung konnte nicht gestartet werden.";
        d.CloseConnection(false);
        return false;
    }
    enet_peer_timeout(d.peer, 32, 5000, 15000);
    d.state = NetState::Connecting;
    d.connectStart = d.Now();
    enet_host_flush(d.host);   // Verbindungsanfrage sofort losschicken
    return true;
}

void NetClient::Disconnect() {
    Impl& d = *impl_;
    if (!d.host && !d.enetAcquired) {
        d.state = NetState::Disconnected;
        d.worldReady = false;
        return;
    }
    d.CloseConnection(true);
}

NetState NetClient::State() const { return impl_->state; }

std::string NetClient::TakeError() {
    std::string e;
    e.swap(impl_->error);
    return e;
}

bool NetClient::HasInfo() const { return impl_->hasInfo; }
const net::ServerInfoMsg& NetClient::Info() const { return impl_->info; }

float NetClient::PingMs() const {
    const Impl& d = *impl_;
    if (!d.peer || !d.connected) return 0.0f;
    return static_cast<float>(d.peer->roundTripTime);   // von ENet gemittelt, in ms
}

// Konto anlegen. Prüft Name und Passwortlänge sofort (dann wird nichts
// gesendet), berechnet den Schlüssel und schickt Register. Der Server meldet
// den Spieler bei Erfolg direkt an (Welcome).
void NetClient::Register(const std::string& name, const std::string& password) {
    Impl& d = *impl_;
    if (d.state != NetState::Lobby || !d.hasInfo) {
        d.error = "Nicht mit einem Server verbunden.";
        return;
    }
    const std::string n = Trim(name);
    if (!net::ValidPlayerName(n)) {
        d.error = net::ErrorText(net::ErrorCode::NameInvalid);
        return;
    }
    if (static_cast<int>(password.size()) < net::MIN_PASSWORD) {
        d.error = Fmt("Das Passwort muss mindestens %d Zeichen lang sein.", net::MIN_PASSWORD);
        return;
    }
    // PBKDF2 mit 20 000 Runden: dauert einige Millisekunden und macht das
    // Durchprobieren von Passwörtern teuer. Das Passwort selbst wird weder
    // gespeichert noch gesendet.
    net::RegisterMsg m;
    m.name = n;
    m.key = crypto::DeriveLoginKey(password, ToLowerAscii(n), d.info.realmSalt, net::KDF_ITERATIONS);
    d.Send(net::Encode(m), true);
    m.key.fill(0);
    d.loginKind = Impl::LoginKind::Register;
    d.loginName = n;
    d.myName = n;
    d.loginStart = d.Now();
    d.state = NetState::LoggingIn;
}

// Anmelden, Schritt 1: Schlüssel berechnen (bleibt nur bis zur Challenge im
// Speicher) und den Namen schicken. Schritt 2 in HandleChallenge.
void NetClient::Login(const std::string& name, const std::string& password) {
    Impl& d = *impl_;
    if (d.state != NetState::Lobby || !d.hasInfo) {
        d.error = "Nicht mit einem Server verbunden.";
        return;
    }
    const std::string n = Trim(name);
    if (!net::ValidPlayerName(n)) {
        d.error = net::ErrorText(net::ErrorCode::NameInvalid);
        return;
    }
    if (static_cast<int>(password.size()) < net::MIN_PASSWORD) {
        d.error = Fmt("Das Passwort muss mindestens %d Zeichen lang sein.", net::MIN_PASSWORD);
        return;
    }
    d.loginKey = crypto::DeriveLoginKey(password, ToLowerAscii(n), d.info.realmSalt, net::KDF_ITERATIONS);
    d.haveKey = true;
    net::LoginStartMsg m;
    m.name = n;
    d.Send(net::Encode(m), true);
    d.loginKind = Impl::LoginKind::Login;
    d.loginName = n;
    d.myName = n;   // vorläufig; die Players-Liste liefert die Schreibweise des Kontos
    d.loginStart = d.Now();
    d.state = NetState::LoggingIn;
}

void NetClient::Spectate() {
    Impl& d = *impl_;
    if (d.state != NetState::Lobby || !d.hasInfo) {
        d.error = "Nicht mit einem Server verbunden.";
        return;
    }
    d.Send(net::EncodeSimple(net::MsgType::Spectate), true);
    d.loginKind = Impl::LoginKind::Spectate;
    d.loginName.clear();
    d.myName.clear();
    d.loginStart = d.Now();
    d.state = NetState::LoggingIn;
}

// Wie Login, nur mit LoginKind::Upload: HandleChallenge schickt dann die
// Karte (MapUpload) statt LoginProof.
void NetClient::UploadMap(const std::string& name, const std::string& password, const GameMap& map) {
    Impl& d = *impl_;
    Login(name, password);
    if (d.state != NetState::LoggingIn) return;   // Eingabe ungültig (Grund in TakeError)
    d.loginKind = Impl::LoginKind::Upload;
    d.uploadMap = map;
    d.uploadDone = false;
}

bool NetClient::TakeUploadResult(bool& ok, std::string& text) {
    Impl& d = *impl_;
    if (!d.uploadDone) return false;
    d.uploadDone = false;
    ok = d.uploadOk;
    text = d.uploadText;
    return true;
}

void NetClient::Update(float dt, const ShipInput& held, bool scanPressed, bool interactPressed) {
    Impl& d = *impl_;
    if (!(dt > 0.0f)) dt = 0.0f;   // auch NaN
    dt = std::min(dt, MAX_FRAME_DT);

    // 1. Netzwerk: alles abarbeiten, was seit dem letzten Frame angekommen ist
    d.Service();
    d.CheckTimeouts();

    // 2. Spieler: Eingaben in festen Schritten (30/s, wie der Server)
    if (d.host && d.state == NetState::Playing && d.worldReady) {
        // Tastendrücke als Zähler (siehe Protocol.hpp). Angedockt NICHT
        // zählen: F/ESC im Stationsfenster gehen als DockAction an den
        // Server; ein zusätzlicher Interaktions-Druck würde nach dem Abdocken
        // sofort wieder andocken (docs/Mehrspieler.md Abschnitt 4).
        if (scanPressed) ++d.scanPresses;
        if (interactPressed && d.world.Me().dockedAt < 0) ++d.interactPresses;
        d.accumulator += dt;
        while (d.accumulator >= net::TICK_DT) {
            d.accumulator -= net::TICK_DT;
            d.StepOwnShip(held);
        }
    }

    // 3. Darstellung der Spiegelwelt. Hier mit gekürzter Zeit wie im
    // Einzelspieler: Ein langer Frame soll Partikel nicht auf einen Schlag
    // weit springen lassen.
    if (d.worldReady) {
        const float viewDt = std::min(dt, MAX_VIEW_DT);
        d.world.time += viewDt;
        d.UpdateRemoteShips();
        d.UpdateBullets();
        if (d.state == NetState::Playing) d.UpdateOwnDisplay(viewDt);
        d.world.UpdateCosmetics(viewDt);
    }

    // Eingaben sofort verschicken, nicht erst beim nächsten Frame
    if (d.host) enet_host_flush(d.host);
}

void NetClient::SendDockAction(net::DockActionType action, int param) {
    Impl& d = *impl_;
    if (d.state != NetState::Playing) return;
    net::DockActionMsg m;
    m.action = action;
    m.param = static_cast<uint8_t>(std::clamp(param, 0, 255));
    d.Send(net::Encode(m), true);
    if (d.host) enet_host_flush(d.host);
}

void NetClient::SendChat(const std::string& text) {
    Impl& d = *impl_;
    if (d.state != NetState::Playing) return;
    net::ChatMsg m;
    m.text = text;
    d.Send(net::Encode(m), true);
    if (d.host) enet_host_flush(d.host);
}

void NetClient::SendChatTyping(bool typing) {
    Impl& d = *impl_;
    if (d.state != NetState::Playing) return;
    net::ChatTypingMsg m;
    m.typing = typing;
    d.Send(net::Encode(m), true);
}

bool NetClient::WorldReady() const { return impl_->worldReady; }
World& NetClient::GetWorld() { return impl_->world; }
const World& NetClient::GetWorld() const { return impl_->world; }
const net::WelcomeMsg& NetClient::Welcome() const { return impl_->welcome; }
int NetClient::MyPlayerId() const { return impl_->myId; }
const std::string& NetClient::MyName() const { return impl_->myName; }

const std::vector<net::PlayerEntry>& NetClient::Players() const { return impl_->players; }
const std::vector<net::HighscoreEntry>& NetClient::Highscores() const { return impl_->highscores; }
long long NetClient::TotalDelivered() const { return impl_->totalDelivered; }
int NetClient::Spectators() const { return impl_->spectators; }

std::vector<SoundEvent> NetClient::TakeSounds() {
    std::vector<SoundEvent> out;
    out.swap(impl_->sounds);
    return out;
}

std::vector<NetClient::GoalNotice> NetClient::TakeGoals() {
    std::vector<GoalNotice> out;
    out.swap(impl_->goals);
    return out;
}

bool NetClient::TakeRespawned() {
    const bool r = impl_->respawned;
    impl_->respawned = false;
    return r;
}

std::vector<net::ChatLineMsg> NetClient::TakeChat() {
    std::vector<net::ChatLineMsg> out;
    out.swap(impl_->chat);
    return out;
}

bool NetClient::TakeWorldRebuilt() {
    const bool r = impl_->worldRebuilt;
    impl_->worldRebuilt = false;
    return r;
}
