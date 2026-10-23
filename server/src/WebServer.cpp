// ===========================================================================
// WebServer.cpp – Verwaltungs-Webseite des Mondschürfer-Servers
//
// WOFÜR?
//   Der Betreiber des Servers (z. B. im Docker-Container) soll im Browser sehen,
//   wie stark der Server ausgelastet ist, wer gerade spielt, wie die Bestenliste
//   aussieht, und er soll die Spielparameter (Schwierigkeitsgrad, Stationen,
//   Mondfarbe, maximale Spieler) und die Schiffe des Raumschiffsverkäufers
//   ändern können.
//
// WER BENUTZT DIESE DATEI?
//   server/src/main.cpp legt ein WebServer-Objekt an, füllt WebConfig aus den
//   Umgebungsvariablen (ADMIN_USER, ADMIN_PASSWORD, MONDSCHUERFER_WEB_PORT,
//   MONDSCHUERFER_WEB_ROOT) und ruft Start() auf. Beim Beenden ruft main.cpp
//   Stop() (oder der Destruktor erledigt das).
//
// ABHÄNGIGKEITEN
//   - third_party/httplib/httplib.h (cpp-httplib 0.22.0): ein kompletter
//     HTTP-Server in einer einzigen Header-Datei. Wir benutzen ihn OHNE
//     OpenSSL, also nur unverschlüsseltes HTTP. Deshalb ist die Seite in
//     docker-compose.yml standardmäßig nur lokal (127.0.0.1) erreichbar.
//     Für den Zugriff über das Internet gehört ein Reverse-Proxy mit HTTPS
//     (oder ein VPN) davor – siehe server/README.md.
//   - SharedState.hpp: der einzige Weg, auf dem Daten zwischen Spiel und
//     Webseite fließen (Kopie des Zustands hin, Einstellungswünsche zurück).
//   - GameSettings.hpp: Grenzen (limits::...), Sanitize, Parse-/Key-Funktionen.
//   - Crypto.hpp: SHA-256 und ConstantTimeEqual für den Passwortvergleich.
//   - server/web/index.html, app.js, style.css: die eigentliche Seite. Der
//     Browser lädt sie einmal und fragt danach alle 2 Sekunden /api/status ab.
//
// THREADS – WER LÄUFT WO?
//   Start() läuft im Haupt-Thread. Es öffnet den Port und startet einen
//   eigenen "Web-Thread", der in httplib::Server::listen_after_bind() auf
//   Verbindungen wartet. Jede angenommene Verbindung wird von einem von vier
//   Arbeits-Threads (Thread-Pool von httplib) bearbeitet. Alle Handler unten
//   können daher GLEICHZEITIG in mehreren Threads laufen. Gemeinsam genutzte
//   Daten sind deshalb entweder unveränderlich nach Start() (Konfiguration,
//   Passwort-Hashes) oder durch einen Mutex geschützt (Sperrliste, SharedState).
//   Der Spiel-Thread wird nie direkt angefasst.
//   Alles, was die Handler brauchen, steckt im WebContext (über std::shared_ptr
//   geteilt). Stop() wartet höchstens 2 Sekunden auf den Web-Thread und
//   koppelt ihn sonst ab – Einzelheiten bei WebContext und WebServer::Stop().
//
// ABLAUF EINER ANFRAGE
//   1. httplib liest Anfragezeile und Header.
//   2. Pre-Routing-Handler (läuft für JEDE Anfrage, auch für unbekannte Pfade):
//      a) IP gesperrt?           -> 429 "Too Many Requests"
//      b) Basic-Auth korrekt?    -> weiter zu 3. (POST ohne Content-Length -> 411)
//      c) sonst                  -> 401 (Browser zeigt Anmeldedialog),
//                                   falsche Daten zählen als Fehlversuch.
//   3. Routing auf genau neun feste Pfade (drei Dateien, Status, Einstellungen, Schiffe, Karten).
//      Alles andere ergibt 404. Es gibt KEIN allgemeines Ausliefern eines
//      Ordners, daher kann niemand mit "/../" andere Dateien lesen.
//   4. Jede Antwort bekommt Sicherheits-Header (set_default_headers).
// ===========================================================================
#include "WebServer.hpp"

#include "Crypto.hpp"
#include "GameSettings.hpp"

// httplib.h bindet unter Windows winsock2.h/ws2tcpip.h ein (und setzt vorher
// NOMINMAX, damit std::min/std::max nicht von Windows-Makros zerstört werden).
// raylib.h darf in dieser Datei NICHT eingebunden werden: raylib und windows.h
// definieren gleichnamige Funktionen (CloseWindow, DrawText, ...). Deshalb
// benutzen wir hier auch Util.hpp nicht (es bindet raylib.h ein).
#include "httplib.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;   // monoton: springt nicht bei Zeitumstellung

// ---------------------------------------------------------------------------
// Einstellbare Grenzen des Webservers
// ---------------------------------------------------------------------------
constexpr int kWorkerThreads = 4;          // gleichzeitig bearbeitete Verbindungen
constexpr size_t kMaxQueuedConnections = 32;   // weitere warten; noch mehr werden sofort geschlossen
constexpr size_t kMaxPayload = 16 * 1024;  // größter erlaubter Anfrage-Körper (Formular)
constexpr time_t kIoTimeoutSec = 5;        // Lese-/Schreib-Zeitlimit pro Verbindung
constexpr size_t kKeepAliveMaxCount = 4;   // höchstens 4 Anfragen über dieselbe Verbindung
constexpr time_t kKeepAliveTimeoutSec = 1; // eine offene Verbindung belegt einen Arbeits-Thread!

// Sperre nach falschen Anmeldungen (pro IP-Adresse, siehe LoginGuard)
constexpr int kFailuresBeforeLock = 5;
constexpr std::chrono::seconds kFirstLock{60};
constexpr std::chrono::seconds kMaxLock{15 * 60};
constexpr std::chrono::seconds kForgetAfter{30 * 60};  // so lange ohne Fehlversuch -> Eintrag vergessen
constexpr size_t kMaxGuardEntries = 4096;              // Obergrenze der Tabelle (Speicherschutz)

constexpr size_t kMaxAuthHeader = 1024;    // längere Authorization-Header sind sicher kein Login
constexpr size_t kMaxStaticFile = 2 * 1024 * 1024;

const char* const kJsonType = "application/json; charset=utf-8";
const char* const kTextType = "text/plain; charset=utf-8";

// ---------------------------------------------------------------------------
// Kleine Text-Hilfen
// ---------------------------------------------------------------------------

// Eigene Kleinbuchstaben-Funktion (Util.hpp darf hier nicht eingebunden werden,
// siehe Kommentar bei #include "httplib.h"). Nur A-Z werden umgewandelt;
// UTF-8-Bytes (>= 0x80) bleiben unverändert.
std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) { return LowerAscii(a) == LowerAscii(b); }

std::string TrimSpaces(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// Länge einer gültigen UTF-8-Folge ab s[i] (1..4) oder 0, wenn sie ungültig ist.
// Geprüft wird nach RFC 3629: keine "überlangen" Kodierungen, keine
// UTF-16-Ersatzzeichen (Surrogates), nichts über U+10FFFF. Warum so genau?
// JSON muss gültiges UTF-8 sein, sonst verweigert der Browser (JSON.parse)
// die ganze Antwort – ein einziger kaputter Servername würde die Seite lahmlegen.
size_t Utf8SequenceLength(const std::string& s, size_t i) {
    const auto at = [&](size_t k) -> unsigned { return k < s.size() ? static_cast<unsigned char>(s[k]) : 0u; };
    const auto cont = [&](size_t k) { return (at(k) & 0xC0u) == 0x80u; };   // hinter dem Ende: at() = 0 -> false
    const unsigned c = at(i);
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
    if (c == 0xE0) return (at(i + 1) >= 0xA0 && at(i + 1) <= 0xBF && cont(i + 2)) ? 3 : 0;
    if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) return (cont(i + 1) && cont(i + 2)) ? 3 : 0;
    if (c == 0xED) return (at(i + 1) >= 0x80 && at(i + 1) <= 0x9F && cont(i + 2)) ? 3 : 0;
    if (c == 0xF0) return (at(i + 1) >= 0x90 && at(i + 1) <= 0xBF && cont(i + 2) && cont(i + 3)) ? 4 : 0;
    if (c >= 0xF1 && c <= 0xF3) return (cont(i + 1) && cont(i + 2) && cont(i + 3)) ? 4 : 0;
    if (c == 0xF4) return (at(i + 1) >= 0x80 && at(i + 1) <= 0x8F && cont(i + 2) && cont(i + 3)) ? 4 : 0;
    return 0;
}

bool IsValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const size_t n = Utf8SequenceLength(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

// Anzahl der Zeichen (nicht Bytes): "Mondschürfer" hat 12 Zeichen, aber 13 Bytes.
size_t Utf8CodePoints(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

bool HasControlChars(const std::string& s) {
    for (unsigned char c : s)
        if (c < 32 || c == 127) return true;
    return false;
}

// ---------------------------------------------------------------------------
// JSON schreiben
//
// Wir bauen das JSON selbst, statt eine Bibliothek einzubinden. Wichtig sind
// zwei Dinge, die man leicht falsch macht:
//   1. Strings korrekt maskieren (Anführungszeichen, Backslash, Steuerzeichen),
//      sonst könnte ein Spieler- oder Servername das JSON kaputt machen.
//   2. Zahlen müssen endlich sein: "nan" oder "inf" ist kein gültiges JSON.
// Die Klasse merkt sich auf einem Stapel, ob im aktuellen Objekt/Array schon
// ein Element steht, und setzt die Kommas automatisch.
// ---------------------------------------------------------------------------
class JsonWriter {
public:
    JsonWriter& BeginObject() { return Open('{'); }
    JsonWriter& EndObject() { return Close('}'); }
    JsonWriter& BeginArray() { return Open('['); }
    JsonWriter& EndArray() { return Close(']'); }

    JsonWriter& Key(const char* key) {
        Separator();
        AppendString(key);
        out_ += ':';
        afterKey_ = true;   // der folgende Wert braucht kein Komma
        return *this;
    }

    JsonWriter& String(const std::string& s) {
        Separator();
        AppendString(s);
        return *this;
    }

    // decimals = Nachkommastellen. Nicht endliche Werte werden zu 0, riesige
    // Werte begrenzt (damit der Puffer reicht und das JSON lesbar bleibt).
    JsonWriter& Number(double v, int decimals) {
        Separator();
        if (!std::isfinite(v)) v = 0.0;
        v = std::clamp(v, -1e15, 1e15);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", std::clamp(decimals, 0, 6), v);
        // snprintf richtet sich nach der eingestellten "locale". Würde irgendwo
        // setlocale(LC_ALL, "de_DE") aufgerufen, käme "1,5" heraus – in JSON
        // muss es aber immer ein Punkt sein.
        for (char* p = buf; *p; ++p)
            if (*p == ',') *p = '.';
        out_ += buf;
        return *this;
    }

    JsonWriter& Int(long long v) {
        Separator();
        out_ += std::to_string(v);   // to_string ist für Ganzzahlen locale-unabhängig
        return *this;
    }

    JsonWriter& Bool(bool b) {
        Separator();
        out_ += b ? "true" : "false";
        return *this;
    }

    const std::string& Result() const { return out_; }

private:
    JsonWriter& Open(char c) {
        Separator();
        out_ += c;
        first_.push_back(true);
        return *this;
    }
    JsonWriter& Close(char c) {
        out_ += c;
        if (!first_.empty()) first_.pop_back();
        return *this;
    }

    // Komma vor jedem Element außer dem ersten – aber nicht zwischen "key": und Wert.
    void Separator() {
        if (afterKey_) {
            afterKey_ = false;
            return;
        }
        if (first_.empty()) return;
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }

    void AppendString(const std::string& s) {
        static const char* const hex = "0123456789abcdef";
        out_ += '"';
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 0x80) {
                // Mehrbyte-Zeichen nur übernehmen, wenn sie gültiges UTF-8 sind;
                // kaputte Bytes werden zum Ersatzzeichen U+FFFD.
                const size_t n = Utf8SequenceLength(s, i);
                if (n == 0) {
                    out_ += "\\ufffd";
                    ++i;
                } else {
                    out_.append(s, i, n);
                    i += n;
                }
                continue;
            }
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                case '\b': out_ += "\\b"; break;
                case '\f': out_ += "\\f"; break;
                default:
                    // Steuerzeichen und zur Sicherheit auch < > & als \u00XX:
                    // Selbst wenn das JSON jemals in HTML landet, kann es dann
                    // kein <script> bilden.
                    if (c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == '&') {
                        out_ += "\\u00";
                        out_ += hex[c >> 4];
                        out_ += hex[c & 0x0F];
                    } else {
                        out_ += static_cast<char>(c);
                    }
            }
            ++i;
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;   // pro offenem Objekt/Array: noch kein Element geschrieben?
    bool afterKey_ = false;
};

// Einstellungen als JSON-Objekt (für "settings" und "defaults", gleich aufgebaut).
void WriteSettings(JsonWriter& w, const GameSettings& g) {
    w.BeginObject();
    w.Key("serverName").String(g.serverName);
    w.Key("difficulty").String(DifficultyKey(g.difficulty));
    w.Key("enemyShips").Int(g.enemyShips);
    w.Key("tradeStations").Int(g.tradeStations);
    w.Key("shipyards").Int(g.shipyards);
    w.Key("moon").String(MoonColorKey(g.moon));
    w.Key("maxPlayers").Int(g.maxPlayers);
    w.Key("lives").Int(g.lives);
    w.Key("missions").Int(g.missions);
    w.EndObject();
}

// Schiffstypen als JSON-Liste (Bereich "Schiffe" der Webseite).
void WriteShips(JsonWriter& w, const std::vector<ShipType>& ships) {
    w.BeginArray();
    for (const ShipType& t : ships) {
        w.BeginObject();
        w.Key("name").String(t.name);
        w.Key("model").String(t.model);
        w.Key("shield").Int(t.shield);
        w.Key("scanner").Int(t.scanner);
        w.Key("speed").Int(t.speed);
        w.Key("weapon").Int(t.weapon);
        w.Key("cargo").Int(t.cargo);
        w.Key("cost").Int(t.cost);
        w.Key("guns").Int(t.guns);
        w.Key("forSale").Bool(t.forSale);
        w.EndObject();
    }
    w.EndArray();
}

// Komplette Antwort für GET /api/status (erklärt in docs/Mehrspieler.md, Abschnitt 11).
// Die Daten stammen aus der Kopie, die der Spiel-Thread einmal pro Sekunde in
// SharedState ablegt. Hier wird nichts berechnet, nur umgeformt.
std::string BuildStatusJson(const ServerStatus& s) {
    JsonWriter w;
    w.BeginObject();
    w.Key("serverName").String(s.serverName);
    w.Key("uptime").Number(s.uptimeSeconds, 1);
    w.Key("tickRate").Int(s.tickRate);
    w.Key("avgTickMs").Number(s.avgTickMs, 3);
    w.Key("maxTickMs").Number(s.maxTickMs, 3);
    w.Key("load").Number(s.loadPercent, 2);
    w.Key("cpu").Number(s.cpuPercent, 1);
    w.Key("memoryMB").Number(s.memoryMB, 1);
    w.Key("cores").Int(s.cpuCores);
    w.Key("playersOnline").Int(s.playersOnline);
    w.Key("maxPlayers").Int(s.maxPlayers);
    // Falls der Spiel-Thread die Grenzen (noch) nicht einträgt, die festen Werte nehmen.
    w.Key("maxPlayersLimit").Int(s.maxPlayersLimit > 0 ? s.maxPlayersLimit : limits::MAX_PLAYERS_LIMIT);
    w.Key("spectators").Int(s.spectators);
    w.Key("maxSpectators").Int(s.maxSpectators > 0 ? s.maxSpectators : limits::MAX_SPECTATORS);
    w.Key("netInKBs").Number(s.netInKBs, 1);
    w.Key("netOutKBs").Number(s.netOutKBs, 1);
    w.Key("totalDelivered").Int(s.totalDelivered);
    w.Key("stationStage").Int(s.stationStage);
    w.Key("enemiesAlive").Int(s.enemiesAlive);
    w.Key("accounts").Int(s.registeredAccounts);
    w.Key("gamePort").Int(s.gamePort);

    w.Key("players").BeginArray();
    for (const PlayerStatus& p : s.players) {
        w.BeginObject();
        w.Key("name").String(p.name);
        w.Key("progress").Number(p.progressPercent, 1);
        w.Key("balance").Int(p.balance);
        w.Key("round").Int(p.round);
        w.Key("upgrades").BeginArray();
        for (int u : p.upgrades) w.Int(u);
        w.EndArray();
        w.Key("shipTime").Number(p.shipTime, 1);
        w.Key("session").Number(p.sessionSeconds, 1);
        w.Key("totalLogin").Number(p.totalLoginSeconds, 1);
        w.Key("lives").Int(p.lives);
        w.Key("kills").Int(p.kills);
        w.Key("deaths").Int(p.deaths);
        w.Key("goals").Int(p.goals);
        w.Key("alive").Bool(p.alive);
        w.Key("docked").Bool(p.docked);
        w.Key("ping").Int(p.pingMs);
        w.EndObject();
    }
    w.EndArray();

    w.Key("highscores").BeginArray();
    for (const HighscoreStatus& h : s.highscores) {
        w.BeginObject();
        w.Key("name").String(h.name);
        w.Key("seconds").Number(h.seconds, 1);
        w.Key("round").Int(h.round);
        w.Key("when").Int(static_cast<long long>(h.when));
        w.EndObject();
    }
    w.EndArray();

    w.Key("settings");
    WriteSettings(w, s.settings);
    w.Key("defaults");
    WriteSettings(w, s.defaults);
    w.Key("ships");
    WriteShips(w, s.settings.ships);
    w.Key("defaultShips");
    WriteShips(w, DefaultShipTypes());
    w.Key("shipModels").BeginArray();
    for (const std::string& m : s.shipModels) w.String(m);
    w.EndArray();

    w.Key("maps").BeginArray();
    for (const MapStatus& m : s.maps) {
        w.BeginObject();
        w.Key("file").String(m.file);
        w.Key("name").String(m.name);
        w.Key("author").String(m.author);
        w.Key("pieces").Int(m.pieces);
        w.Key("loops").Int(m.loops);
        w.EndObject();
    }
    w.EndArray();
    w.Key("activeMap").String(s.activeMap);
    w.Key("mapUploaders").BeginArray();
    for (const std::string& n : s.mapUploaders) w.String(n);
    w.EndArray();
    w.Key("mapNotice").String(s.mapNotice);
    w.Key("mapNoticeSeq").Int(s.mapNoticeSeq);
    w.Key("mapNoticeError").Bool(s.mapNoticeError);

    w.Key("limits").BeginObject();
    w.Key("maxEnemyShips").Int(limits::MAX_ENEMY_SHIPS);
    w.Key("maxTradeStations").Int(limits::MAX_TRADE_STATIONS);
    w.Key("maxShipyards").Int(limits::MAX_SHIPYARDS);
    w.Key("maxPlayersLimit").Int(limits::MAX_PLAYERS_LIMIT);
    w.Key("maxLives").Int(limits::MAX_LIVES);
    w.Key("maxMissions").Int(limits::MAX_MISSIONS);
    w.Key("maxShipTypes").Int(limits::MAX_SHIP_TYPES);
    w.Key("maxShipName").Int(limits::MAX_SHIP_NAME);
    w.Key("minShipShield").Int(limits::MIN_SHIP_SHIELD);
    w.Key("maxShipShield").Int(limits::MAX_SHIP_SHIELD);
    w.Key("minShipPercent").Int(limits::MIN_SHIP_PERCENT);
    w.Key("maxShipPercent").Int(limits::MAX_SHIP_PERCENT);
    w.Key("minShipSpeed").Int(limits::MIN_SHIP_SPEED);
    w.Key("maxShipSpeed").Int(limits::MAX_SHIP_SPEED);
    w.Key("minShipCargo").Int(limits::MIN_SHIP_CARGO);
    w.Key("maxShipCargo").Int(limits::MAX_SHIP_CARGO);
    w.Key("maxShipCost").Int(limits::MAX_SHIP_COST);
    w.Key("maxGuns").Int(cfg::MAX_GUNS);
    w.EndObject();

    w.EndObject();
    return w.Result();
}

void SendJsonError(httplib::Response& res, int status, const std::string& message) {
    JsonWriter w;
    w.BeginObject().Key("ok").Bool(false).Key("error").String(message).EndObject();
    res.status = status;
    res.set_content(w.Result(), kJsonType);
}

// ---------------------------------------------------------------------------
// Prüfung der Zugangsdaten beim Start
//
// Die Webseite kann Spielparameter ändern und zeigt alle Spielernamen. Läuft
// sie mit einem Standardpasswort aus einer Anleitung im Internet, ist sie in
// Minuten übernommen. Deshalb startet sie gar nicht erst, wenn das Passwort
// offensichtlich schwach ist (das Spiel selbst läuft trotzdem).
// ---------------------------------------------------------------------------

// Für den Vergleich mit der Liste schwacher Passwörter: klein schreiben, nur
// Buchstaben/Ziffern behalten ("Change-Me!" -> "changeme", "Ändern" -> "ändern").
std::string NormalizeForWeakCheck(const std::string& s) {
    std::string in = LowerAscii(s);
    // Großbuchstaben aus Latin-1 (Ä, Ö, Ü, É ...) liegen in UTF-8 bei C3 80..C3 9E;
    // der Kleinbuchstabe ist jeweils 0x20 weiter (Ä = C3 84 -> ä = C3 A4).
    // C3 97 ist das Malzeichen und bleibt.
    for (size_t i = 0; i + 1 < in.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(in[i]), b = static_cast<unsigned char>(in[i + 1]);
        if (a == 0xC3 && b >= 0x80 && b <= 0x9E && b != 0x97) in[i + 1] = static_cast<char>(b + 0x20);
    }
    std::string out;
    for (unsigned char c : in)
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) out.push_back(static_cast<char>(c));
    return out;
}

bool IsObviouslyWeak(const std::string& user, const std::string& password) {
    // Normalisierte Form, z. B. "Admin_2024!" -> "admin2024". Dazu dieselbe Form
    // ohne angehängte Ziffern ("admin"), denn "passwort123" ist nicht besser
    // als "passwort".
    const std::string norm = NormalizeForWeakCheck(password);
    std::string stem = norm;
    while (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') stem.pop_back();

    // Bekannte Standard- und Platzhalterpasswörter (bereits normalisiert).
    static const char* const kWeak[] = {
        "changeme", "changethis", "change", "password", "passwort", "passwd", "passw0rd", "kennwort",
        "admin", "administrator", "root", "mondschuerfer", "mondschurfer", "mondsch\xc3\xbc" "rfer", "server",
        "secret", "geheim", "letmein", "welcome", "willkommen", "default", "standard", "test", "demo",
        "qwerty", "qwertz", "hallo", "hello", "bitteaendern", "bitte\xc3\xa4" "ndern", "aendern",
        "aendermich", "\xc3\xa4" "ndern", "\xc3\xa4" "ndermich", "geheimespasswort", "sicherespasswort",
    };
    for (const char* w : kWeak)
        if (norm == w || (!stem.empty() && stem == w)) return true;

    // Benutzername == Passwort (auch "Admin" / "admin1")
    const std::string userNorm = NormalizeForWeakCheck(user);
    if (!userNorm.empty() && (norm == userNorm || stem == userNorm)) return true;
    if (EqualsIgnoreCase(TrimSpaces(user), TrimSpaces(password))) return true;

    // Nur ein einziges Zeichen wiederholt ("aaaaaaaaaa", "1111111111")
    if (!password.empty() && std::all_of(password.begin(), password.end(), [&](char c) { return c == password[0]; }))
        return true;

    // Tastatur- oder Zahlenfolgen ("1234567890", "abcdefghij", "qwertzuiop")
    static const char* const kSequences[] = {
        "012345678901234567890", "098765432109876543210", "abcdefghijklmnopqrstuvwxyz",
        "qwertzuiopasdfghjklyxcvbnm", "qwertyuiopasdfghjklzxcvbnm",
    };
    if (norm.size() >= 4)
        for (const char* seq : kSequences)
            if (std::string(seq).find(norm) != std::string::npos) return true;
    return false;
}

// Liefert true, wenn Benutzer und Passwort für den Betrieb taugen; sonst steht
// in reason eine verständliche Begründung (für stderr / docker logs).
bool CheckAdminCredentials(const std::string& user, const std::string& password, std::string& reason) {
    if (TrimSpaces(user).empty()) {
        reason = "ADMIN_USER ist nicht gesetzt (leer).";
        return false;
    }
    if (user.find(':') != std::string::npos) {
        // Bei HTTP Basic Auth trennt der erste Doppelpunkt Benutzer und Passwort.
        reason = "ADMIN_USER darf keinen Doppelpunkt (:) enthalten.";
        return false;
    }
    if (HasControlChars(user) || HasControlChars(password)) {
        reason = "ADMIN_USER/ADMIN_PASSWORD enthalten Steuerzeichen (z. B. Zeilenumbruch).";
        return false;
    }
    if (!IsValidUtf8(user) || !IsValidUtf8(password)) {
        reason = "ADMIN_USER/ADMIN_PASSWORD sind kein gültiges UTF-8.";
        return false;
    }
    if (Utf8CodePoints(password) < 10) {
        reason = "ADMIN_PASSWORD ist zu kurz (mindestens 10 Zeichen, besser 16 oder mehr).";
        return false;
    }
    if (IsObviouslyWeak(user, password)) {
        reason = "ADMIN_PASSWORD ist zu leicht zu erraten (Standardwort, Platzhalter, Zahlenfolge "
                 "oder gleich dem Benutzernamen).";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// HTTP Basic Auth
//
// Der Browser schickt nach dem Anmeldedialog bei jeder Anfrage den Header
//   Authorization: Basic <Base64("benutzer:passwort")>
// Base64 ist KEINE Verschlüsselung, nur eine Kodierung. Ohne HTTPS kann jeder
// im selben Netz das Passwort mitlesen – darum standardmäßig nur 127.0.0.1.
// ---------------------------------------------------------------------------

// Strenger Base64-Dekoder (Standardalphabet, '=' nur am Ende).
bool DecodeBase64(const std::string& in, std::string& out) {
    out.clear();
    uint32_t buffer = 0;
    int bits = 0;
    size_t padding = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') {
            ++padding;
            continue;
        } else
            return false;
        if (padding > 0) return false;   // Daten nach dem Füllzeichen
        buffer = ((buffer << 6) | static_cast<uint32_t>(v)) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    if (padding > 2) return false;
    if (padding > 0 && in.size() % 4 != 0) return false;
    return bits < 6;   // 6 übrige Bits = ein einzelnes überzähliges Zeichen -> ungültig
}

// Zerlegt den Authorization-Header. false, wenn er fehlt oder kein Basic-Login ist.
bool ParseBasicAuth(const std::string& header, std::string& user, std::string& password) {
    if (header.empty() || header.size() > kMaxAuthHeader) return false;
    const size_t space = header.find(' ');
    if (space == std::string::npos || !EqualsIgnoreCase(header.substr(0, space), "basic")) return false;
    std::string decoded;
    if (!DecodeBase64(TrimSpaces(header.substr(space + 1)), decoded)) return false;
    const size_t colon = decoded.find(':');
    if (colon == std::string::npos) return false;
    user = decoded.substr(0, colon);
    password = decoded.substr(colon + 1);
    return true;
}

// ---------------------------------------------------------------------------
// LoginGuard: Sperre nach falschen Anmeldungen (pro IP-Adresse)
//
// Regeln:
//   - Ab dem 5. Fehlversuch ist die Adresse 60 s gesperrt (Antwort 429).
//   - Jeder weitere Fehlversuch nach Ablauf der Sperre sperrt erneut, jeweils
//     doppelt so lange (120 s, 240 s, ...), höchstens 15 Minuten.
//   - 30 Minuten ohne Fehlversuch (und ohne laufende Sperre): alles vergessen.
//   - Eine ERFOLGREICHE Anmeldung setzt den Zähler bewusst NICHT zurück.
//     Grund: Hinter einem Reverse-Proxy oder NAT haben alle Besucher dieselbe
//     IP. Würde der Erfolg zurücksetzen, könnte ein Angreifer seine Versuche
//     zwischen die automatischen Abfragen (alle 2 s) des echten Admins legen
//     und hätte so unbegrenzt viele Versuche.
//   - Während einer Sperre werden die Zugangsdaten gar nicht erst geprüft,
//     sonst könnte man trotz Sperre weiterraten.
// Die Tabelle wird von allen Arbeits-Threads benutzt -> Mutex. Damit sie nicht
// unbegrenzt wächst (Angreifer mit vielen Adressen), ist sie auf
// kMaxGuardEntries begrenzt; alte Einträge werden aufgeräumt.
// ---------------------------------------------------------------------------
class LoginGuard {
public:
    // Sekunden, die die Adresse noch gesperrt ist (0 = nicht gesperrt).
    int SecondsLocked(const std::string& key, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(m_);
        auto it = entries_.find(key);
        if (it == entries_.end() || it->second.lockedUntil <= now) return 0;
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(it->second.lockedUntil - now).count();
        return static_cast<int>(left) + 1;   // aufrunden, nie 0 während der Sperre
    }

    // Fehlversuch eintragen. Liefert die Sperrdauer in Sekunden, wenn die
    // Adresse JETZT gesperrt wurde, sonst 0.
    int RegisterFailure(const std::string& key, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(m_);
        Cleanup(now, false);
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            if (entries_.size() >= kMaxGuardEntries) Cleanup(now, true);
            it = entries_.emplace(key, Entry{}).first;
        } else if (Expired(it->second, now)) {
            it->second = Entry{};   // lange her: wie ein neuer Besucher behandeln
        }
        Entry& e = it->second;
        e.lastFailure = now;
        ++e.failures;
        if (e.failures < kFailuresBeforeLock) return 0;

        // Sperrdauer verdoppelt sich mit jeder Sperre: 60 s * 2^(Stufe-1), max. 15 min
        ++e.lockLevel;
        auto duration = kFirstLock;
        for (int i = 1; i < e.lockLevel && duration < kMaxLock; ++i) duration *= 2;
        duration = std::min(duration, kMaxLock);
        e.lockedUntil = now + duration;
        return static_cast<int>(duration.count());
    }

private:
    struct Entry {
        int failures = 0;
        int lockLevel = 0;                 // wie oft schon gesperrt (für die Verdopplung)
        Clock::time_point lastFailure{};
        Clock::time_point lockedUntil{};
    };

    static bool Expired(const Entry& e, Clock::time_point now) {
        return e.lockedUntil <= now && now - e.lastFailure > kForgetAfter;
    }

    // Aufräumen (m_ ist bereits gesperrt). Normal höchstens einmal pro Minute.
    // force = Tabelle voll: dann notfalls auch noch relevante Einträge löschen,
    // und zwar die, deren Sperre/Gedächtnis am frühesten abläuft.
    void Cleanup(Clock::time_point now, bool force) {
        if (!force && now - lastCleanup_ < std::chrono::seconds(60)) return;
        lastCleanup_ = now;
        for (auto it = entries_.begin(); it != entries_.end();)
            it = Expired(it->second, now) ? entries_.erase(it) : std::next(it);
        while (force && entries_.size() >= kMaxGuardEntries) {
            auto victim = entries_.begin();
            auto relevantUntil = [](const Entry& e) { return std::max(e.lockedUntil, e.lastFailure + kForgetAfter); };
            for (auto it = entries_.begin(); it != entries_.end(); ++it)
                if (relevantUntil(it->second) < relevantUntil(victim->second)) victim = it;
            entries_.erase(victim);
        }
    }

    std::mutex m_;
    std::unordered_map<std::string, Entry> entries_;
    Clock::time_point lastCleanup_{};
};

// Schlüssel für die Sperrliste aus der Absenderadresse (req.remote_addr):
//   IPv4                     -> unverändert ("203.0.113.7")
//   IPv4 in IPv6 verpackt    -> als IPv4 ("::ffff:203.0.113.7" -> "203.0.113.7")
//   IPv6                     -> nur die ersten 64 Bit. Ein Internetanschluss
//                               bekommt meist ein ganzes /64-Netz; sonst könnte
//                               ein Angreifer für jeden Versuch eine neue
//                               Adresse aus seinem Netz nehmen.
std::string ClientKey(const std::string& addr) {
    if (addr.find(':') == std::string::npos) return addr;
    unsigned char b[16] = {};
    if (inet_pton(AF_INET6, addr.c_str(), b) != 1) return addr;   // z. B. mit Zonen-ID: roh übernehmen
    static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    char buf[64];
    if (std::memcmp(b, kMapped, sizeof(kMapped)) == 0) {
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    } else {
        std::snprintf(buf, sizeof(buf), "%02x%02x:%02x%02x:%02x%02x:%02x%02x::/64", b[0], b[1], b[2], b[3], b[4], b[5],
                      b[6], b[7]);
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Schutz gegen Cross-Site-Request-Forgery (CSRF)
//
// Das Problem: Der Browser hat sich das Admin-Passwort gemerkt und schickt es
// automatisch mit – auch wenn eine FREMDE Webseite heimlich ein Formular an
// unseren Server abschickt. Gegenmaßnahmen (alle drei müssen passen):
//   1. Header "X-Requested-With: mondschuerfer": Ein normales HTML-Formular
//      kann keine eigenen Header setzen. Ein Skript einer fremden Seite
//      dürfte es nur nach einer CORS-Freigabe, die wir nie erteilen.
//   2. Origin-Header (vom Browser gesetzt, nicht fälschbar durch Skripte):
//      Wenn vorhanden, muss er auf genau diesen Host zeigen.
//   3. Sec-Fetch-Site (moderne Browser): Wenn vorhanden, nur "same-origin".
// ---------------------------------------------------------------------------

// "host:port" in einheitlicher Form: klein geschrieben, Standardport entfernt.
std::string NormalizeAuthority(const std::string& authority, const std::string& scheme) {
    std::string a = LowerAscii(TrimSpaces(authority));
    const std::string defaultPort = scheme == "https" ? ":443" : ":80";
    if (a.size() > defaultPort.size() && a.compare(a.size() - defaultPort.size(), defaultPort.size(), defaultPort) == 0)
        a.resize(a.size() - defaultPort.size());
    return a;
}

bool OriginMatchesHost(const std::string& origin, const std::string& host) {
    const size_t sep = origin.find("://");
    if (sep == std::string::npos || host.empty()) return false;
    const std::string scheme = LowerAscii(origin.substr(0, sep));
    if (scheme != "http" && scheme != "https") return false;   // auch "null" landet hier
    const std::string authority = origin.substr(sep + 3);
    // Ein Origin besteht nur aus Schema://Host[:Port], ohne Pfad oder Benutzer.
    if (authority.empty() || authority.find_first_of("/?#@ ") != std::string::npos) return false;
    return NormalizeAuthority(authority, scheme) == NormalizeAuthority(host, scheme);
}

// Liefert true, wenn die POST-Anfrage von unserer eigenen Seite stammt.
// Sonst ist res bereits mit 403 gefüllt.
bool CheckSameOriginPost(const httplib::Request& req, httplib::Response& res) {
    if (!EqualsIgnoreCase(req.get_header_value("X-Requested-With"), "mondschuerfer")) {
        SendJsonError(res, 403, "Anfrage abgelehnt: Header X-Requested-With fehlt (nur über die Verwaltungsseite möglich).");
        return false;
    }
    if (req.has_header("Origin") && !OriginMatchesHost(req.get_header_value("Origin"), req.get_header_value("Host"))) {
        SendJsonError(res, 403, "Anfrage abgelehnt: Sie stammt nicht von dieser Seite (Origin passt nicht zum Host).");
        return false;
    }
    if (req.has_header("Sec-Fetch-Site")) {
        const std::string site = LowerAscii(req.get_header_value("Sec-Fetch-Site"));
        if (site != "same-origin" && site != "none") {
            SendJsonError(res, 403, "Anfrage abgelehnt: Sie stammt von einer anderen Webseite.");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Formular auswerten (POST /api/settings)
//
// Der Browser schickt "application/x-www-form-urlencoded", z. B.
//   serverName=Mein+Server&difficulty=hard&tradeStations=2
// Wir lesen den Körper selbst (statt req.params von httplib), weil httplib
// dort auch Parameter aus der URL (?...) mischt und ungültige %-Folgen
// stillschweigend durchlässt. Hier ist alles Ungültige ein Fehler.
// ---------------------------------------------------------------------------
int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool PercentDecode(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out.push_back(' ');
        } else if (in[i] == '%') {
            if (i + 2 >= in.size()) return false;   // "%" braucht genau zwei Hex-Ziffern dahinter
            const int hi = HexValue(in[i + 1]), lo = HexValue(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<char>(hi * 16 + lo));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return true;
}

bool ParseFormBody(const std::string& body, std::map<std::string, std::string>& fields, std::string& error) {
    fields.clear();
    if (body.empty()) return true;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('&', start);
        if (end == std::string::npos) end = body.size();
        const std::string pair = body.substr(start, end - start);
        start = end + 1;
        if (pair.empty()) continue;   // "a=1&&b=2" ist harmlos
        const size_t eq = pair.find('=');
        std::string key, value;
        if (!PercentDecode(pair.substr(0, eq), key) ||
            !PercentDecode(eq == std::string::npos ? std::string() : pair.substr(eq + 1), value)) {
            error = "Formulardaten sind ungültig kodiert.";
            return false;
        }
        if (fields.count(key) != 0) {
            error = "Feld \"" + key + "\" ist doppelt vorhanden.";
            return false;
        }
        fields[key] = value;
    }
    return true;
}

// Ganze Zahl im Bereich [minValue, maxValue]. Nur Ziffern (optional '-'),
// keine Leerzeichen, kein "+", keine Kommazahl.
bool ParseIntInRange(const std::string& text, int minValue, int maxValue, int& out) {
    if (text.empty() || text.size() > 9) return false;
    int v = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto r = std::from_chars(first, last, v);
    if (r.ec != std::errc() || r.ptr != last) return false;
    if (v < minValue || v > maxValue) return false;
    out = v;
    return true;
}

// Übernimmt die Formularfelder in "next" (Startwert = aktuelle Einstellungen,
// fehlende Felder bleiben also unverändert). false + error bei ungültigen Werten.
bool ApplyForm(const std::map<std::string, std::string>& fields, GameSettings& next, std::string& error) {
    static const char* const kKnown[] = {"serverName", "difficulty", "enemyShips", "tradeStations",
                                         "shipyards",  "moon",       "maxPlayers", "lives", "missions"};
    for (const auto& kv : fields) {
        const bool known = std::any_of(std::begin(kKnown), std::end(kKnown), [&](const char* k) { return kv.first == k; });
        if (!known) {
            error = "Unbekanntes Feld im Formular.";
            return false;
        }
    }

    auto field = [&](const char* name) -> const std::string* {
        auto it = fields.find(name);
        return it == fields.end() ? nullptr : &it->second;
    };

    if (const std::string* v = field("serverName")) {
        const std::string name = TrimSpaces(*v);
        if (name.empty()) {
            error = "Der Servername darf nicht leer sein.";
            return false;
        }
        if (!IsValidUtf8(name) || HasControlChars(name)) {
            error = "Der Servername enthält ungültige Zeichen.";
            return false;
        }
        if (name.size() > static_cast<size_t>(limits::MAX_SERVER_NAME)) {
            error = "Der Servername ist zu lang (höchstens " + std::to_string(limits::MAX_SERVER_NAME) +
                    " Byte; Umlaute zählen doppelt).";
            return false;
        }
        next.serverName = name;
    }
    if (const std::string* v = field("difficulty")) {
        if (!ParseDifficulty(*v, next.difficulty)) {
            error = "Unbekannter Schwierigkeitsgrad.";
            return false;
        }
    }
    if (const std::string* v = field("enemyShips")) {
        if (!ParseIntInRange(*v, 0, limits::MAX_ENEMY_SHIPS, next.enemyShips)) {
            error = "Anzahl feindlicher Raumschiffe: bitte eine ganze Zahl von 0 bis " +
                    std::to_string(limits::MAX_ENEMY_SHIPS) + ".";
            return false;
        }
    }
    if (const std::string* v = field("tradeStations")) {
        if (!ParseIntInRange(*v, 1, limits::MAX_TRADE_STATIONS, next.tradeStations)) {
            error = "Anzahl Handelsstationen: bitte eine ganze Zahl von 1 bis " +
                    std::to_string(limits::MAX_TRADE_STATIONS) + ".";
            return false;
        }
    }
    if (const std::string* v = field("shipyards")) {
        if (!ParseIntInRange(*v, 1, limits::MAX_SHIPYARDS, next.shipyards)) {
            error = "Anzahl Werften: bitte eine ganze Zahl von 1 bis " + std::to_string(limits::MAX_SHIPYARDS) + ".";
            return false;
        }
    }
    if (const std::string* v = field("moon")) {
        if (!ParseMoonColor(*v, next.moon)) {
            error = "Unbekannte Mondfarbe.";
            return false;
        }
    }
    if (const std::string* v = field("maxPlayers")) {
        if (!ParseIntInRange(*v, 1, limits::MAX_PLAYERS_LIMIT, next.maxPlayers)) {
            error = "Maximale Spieler: bitte eine ganze Zahl von 1 bis " + std::to_string(limits::MAX_PLAYERS_LIMIT) + ".";
            return false;
        }
    }
    if (const std::string* v = field("lives")) {
        if (!ParseIntInRange(*v, 1, limits::MAX_LIVES, next.lives)) {
            error = "Leben pro Spieler: bitte eine ganze Zahl von 1 bis " + std::to_string(limits::MAX_LIVES) + ".";
            return false;
        }
    }
    if (const std::string* v = field("missions")) {
        if (!ParseIntInRange(*v, 0, limits::MAX_MISSIONS, next.missions)) {
            error = "Mini-Missionen: bitte eine ganze Zahl von 0 bis " + std::to_string(limits::MAX_MISSIONS) + ".";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Formular "Schiffe" auswerten (POST /api/ships)
//
// Felder: count=Anzahl, dann je Schiff i (0 = Startschiff) name<i>, model<i>,
// shield<i>, scanner<i>, speed<i>, weapon<i>, cargo<i>, cost<i>, guns<i> und
// forSale<i> (0/1). Alle Felder sind Pflicht. Schiffe lassen sich nur
// hinzufügen, nicht entfernen: Spieler besitzen sie über ihre Nummer.
// ---------------------------------------------------------------------------
bool ApplyShipForm(const std::map<std::string, std::string>& fields, const ServerStatus& status,
                   std::vector<ShipType>& ships, std::string& error) {
    static const char* const kShipFields[] = {"name", "model", "shield", "scanner", "speed",
                                              "weapon", "cost", "cargo", "guns", "forSale"};
    auto field = [&](const std::string& name) -> const std::string* {
        auto it = fields.find(name);
        return it == fields.end() ? nullptr : &it->second;
    };
    const std::string* countText = field("count");
    int count = 0;
    if (!countText || !ParseIntInRange(*countText, 1, limits::MAX_SHIP_TYPES, count)) {
        error = "Anzahl Schiffe: höchstens " + std::to_string(limits::MAX_SHIP_TYPES) + ".";
        return false;
    }
    if (count < status.settings.ShipCount()) {
        error = "Schiffe lassen sich nicht entfernen (Spieler können sie besitzen). Stattdessen „Im Verkauf“ ausschalten.";
        return false;
    }
    // Unbekannte Felder ablehnen: erlaubt sind nur "count" und <feld><nummer> mit nummer < count
    for (const auto& kv : fields) {
        if (kv.first == "count") continue;
        bool known = false;
        for (const char* f : kShipFields) {
            const size_t len = std::strlen(f);
            int index = 0;
            if (kv.first.compare(0, len, f) == 0 && kv.first.size() > len &&
                ParseIntInRange(kv.first.substr(len), 0, count - 1, index) && kv.first.substr(len) == std::to_string(index)) {
                known = true;
                break;
            }
        }
        if (!known) {
            error = "Unbekanntes Feld im Formular.";
            return false;
        }
    }

    ships.clear();
    for (int i = 0; i < count; ++i) {
        const std::string n = std::to_string(i);
        const std::string label = "Schiff " + std::to_string(i + 1);
        const std::string *name = field("name" + n), *model = field("model" + n), *forSale = field("forSale" + n);
        if (!name || !model || !forSale) {
            error = label + ": Angaben fehlen.";
            return false;
        }
        ShipType t;
        t.name = TrimSpaces(*name);
        if (t.name.empty()) {
            error = label + ": Der Name darf nicht leer sein.";
            return false;
        }
        if (!IsValidUtf8(t.name) || HasControlChars(t.name)) {
            error = label + ": Der Name enthält ungültige Zeichen.";
            return false;
        }
        if (t.name.size() > static_cast<size_t>(limits::MAX_SHIP_NAME)) {
            error = label + ": Der Name ist zu lang (höchstens " + std::to_string(limits::MAX_SHIP_NAME) +
                    " Byte; Umlaute zählen doppelt).";
            return false;
        }
        if (std::find(status.shipModels.begin(), status.shipModels.end(), *model) == status.shipModels.end()) {
            error = label + ": Unbekanntes Modell.";
            return false;
        }
        t.model = *model;
        struct Num {
            const char* key;
            const char* text;
            int lo, hi;
            int* out;
        };
        const Num nums[] = {
            {"shield", "Schild", limits::MIN_SHIP_SHIELD, limits::MAX_SHIP_SHIELD, &t.shield},
            {"scanner", "Scanner", limits::MIN_SHIP_PERCENT, limits::MAX_SHIP_PERCENT, &t.scanner},
            {"speed", "Antrieb", limits::MIN_SHIP_SPEED, limits::MAX_SHIP_SPEED, &t.speed},
            {"weapon", "Waffen", limits::MIN_SHIP_PERCENT, limits::MAX_SHIP_PERCENT, &t.weapon},
            {"cargo", "Lager", limits::MIN_SHIP_CARGO, limits::MAX_SHIP_CARGO, &t.cargo},
            {"cost", "Preis", 0, limits::MAX_SHIP_COST, &t.cost},
            {"guns", "Kanonen", 1, cfg::MAX_GUNS, &t.guns},
        };
        for (const Num& k : nums) {
            const std::string* v = field(k.key + n);
            if (!v || !ParseIntInRange(*v, k.lo, k.hi, *k.out)) {
                error = label + ", " + k.text + ": bitte eine ganze Zahl von " + std::to_string(k.lo) + " bis " +
                        std::to_string(k.hi) + ".";
                return false;
            }
        }
        if (*forSale != "0" && *forSale != "1") {
            error = label + ": „Im Verkauf“ ist ungültig.";
            return false;
        }
        t.forSale = *forSale == "1";
        ships.push_back(t);
    }
    return true;
}

bool SameShips(const std::vector<ShipType>& a, const std::vector<ShipType>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const ShipType &x = a[i], &y = b[i];
        if (x.name != y.name || x.model != y.model || x.shield != y.shield || x.scanner != y.scanner ||
            x.speed != y.speed || x.weapon != y.weapon || x.cargo != y.cargo || x.cost != y.cost || x.guns != y.guns ||
            x.forSale != y.forSale)
            return false;
    }
    return true;
}

bool SameSettings(const GameSettings& a, const GameSettings& b) {
    return a.serverName == b.serverName && a.difficulty == b.difficulty && a.enemyShips == b.enemyShips &&
           a.tradeStations == b.tradeStations && a.shipyards == b.shipyards && a.moon == b.moon &&
           a.maxPlayers == b.maxPlayers && a.lives == b.lives && a.missions == b.missions;
}

// Erfolgsantwort für beide Einstellungs-Routen.
void SendSettingsOk(httplib::Response& res, const GameSettings& s, bool rebuild, const std::string& message) {
    JsonWriter w;
    w.BeginObject();
    w.Key("ok").Bool(true);
    w.Key("settings");
    WriteSettings(w, s);
    w.Key("rebuild").Bool(rebuild);
    w.Key("message").String(message);
    w.EndObject();
    res.status = 200;
    res.set_content(w.Result(), kJsonType);
}

// Text für die Rückmeldung auf der Seite.
std::string SettingsMessage(const GameSettings& current, const GameSettings& next, bool rebuild, int playersOnline,
                            const char* prefix) {
    std::string msg = prefix;
    if (SameSettings(current, next)) {
        msg += " Es hat sich nichts geändert.";
        return msg;
    }
    if (rebuild)
        msg += " Die Welt wird neu aufgebaut, alle Spieler starten an einer Station neu, ihr Fortschritt bleibt erhalten.";
    else
        msg += " Die Änderungen gelten ab sofort.";
    if (next.maxPlayers < playersOnline)
        msg += " Hinweis: Zurzeit sind " + std::to_string(playersOnline) +
               " Spieler online, mehr als das neue Maximum. Neue Anmeldungen werden abgelehnt, bis wieder Plätze frei sind.";
    return msg;
}

// Datei vollständig lesen (binär, damit unter Windows nichts umgewandelt wird).
bool ReadWholeFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return false;
    out = ss.str();
    return out.size() <= kMaxStaticFile;
}

std::string JoinPath(const std::string& dir, const char* file) {
    if (dir.empty()) return file;
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + file : dir + "/" + file;
}


// ===========================================================================
// WebContext – alles, was die Handler (in den Arbeits-Threads) brauchen.
//
// Warum ein eigenes Objekt, das über std::shared_ptr geteilt wird?
//   Stop() wartet höchstens kStopWait auf das Ende des Web-Threads (siehe
//   dort). Hängt eine Verbindung länger (z. B. ein Angreifer, der seine
//   Anfrage absichtlich im Schneckentempo schickt), läuft der Web-Thread
//   "abgekoppelt" weiter, während main.cpp schon aufräumt. Dieser Thread darf
//   dann nichts mehr benutzen, was gleich gelöscht wird:
//     - Die Handler halten eine eigene Referenz auf den WebContext, er lebt
//       also so lange wie der letzte Handler.
//     - Der Zeiger auf SharedState (gehört main.cpp!) wird in Stop() unter
//       einem Mutex auf nullptr gesetzt. Danach greift kein Handler mehr darauf zu.
// ===========================================================================
class WebContext {
public:
    WebContext(const WebConfig& cfg, SharedState* shared)
        : config_(cfg), userHash_(crypto::Sha256(cfg.user)), passHash_(crypto::Sha256(cfg.password)), shared_(shared) {
        config_.password.clear();   // ab hier wird nur noch der Hash gebraucht
    }

    // Ab jetzt nie mehr auf SharedState zugreifen (aus Stop()).
    void Detach() {
        std::lock_guard<std::mutex> lock(sharedMutex_);
        shared_ = nullptr;
    }

    // --- Pre-Routing: Anmeldung und Sperre (läuft in einem Arbeits-Thread) ----
    httplib::Server::HandlerResponse Authenticate(const httplib::Request& req, httplib::Response& res) {
        const std::string client = ClientKey(ClientAddress(req));
        const Clock::time_point now = Clock::now();

        if (const int wait = guard_.SecondsLocked(client, now)) {
            SendLocked(res, wait);
            return httplib::Server::HandlerResponse::Handled;
        }

        const std::string header = req.get_header_value("Authorization");
        std::string user, password;
        if (ParseBasicAuth(header, user, password) && CredentialsMatch(user, password)) {
            // Angemeldet. Eine letzte Prüfung, bevor httplib den Anfrage-Körper
            // liest: Die Größengrenze (set_payload_max_length) greift in httplib
            // nur, wenn der Körper eine Content-Length hat. Bei "chunked" oder
            // ganz ohne Längenangabe würde beliebig viel gelesen. Browser
            // schicken bei fetch() immer eine Content-Length, also verlangen wir sie.
            const bool mayHaveBody = req.method == "POST" || req.method == "PUT" || req.method == "PATCH" ||
                                     req.method == "DELETE";
            if (req.has_header("Transfer-Encoding") || (mayHaveBody && !req.has_header("Content-Length"))) {
                res.status = 411;
                res.set_header("Connection", "close");
                res.set_content("Content-Length erforderlich.\n", kTextType);
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;   // normales Routing
        }

        // Ohne Authorization-Header ist es der erste Besuch (der Browser fragt
        // erst nach einer 401 nach dem Passwort) – das ist kein Fehlversuch.
        if (!header.empty()) {
            if (const int lockedFor = guard_.RegisterFailure(client, now)) {
                std::fprintf(stderr, "[Web] Zu viele falsche Anmeldungen von %s: %d s gesperrt.\n", client.c_str(),
                             lockedFor);
                SendLocked(res, lockedFor);
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        res.status = 401;
        // realm = Name, den der Browser im Anmeldedialog zeigt; charset sagt dem
        // Browser, dass Umlaute im Passwort als UTF-8 geschickt werden sollen.
        res.set_header("WWW-Authenticate", "Basic realm=\"Mondschuerfer\", charset=\"UTF-8\"");
        res.set_content("Anmeldung erforderlich.\n", kTextType);
        return httplib::Server::HandlerResponse::Handled;
    }

    // --- Statische Dateien ---------------------------------------------------
    // Nur diese drei Namen sind möglich (fest im Code). Der Pfad aus der
    // Anfrage wird nie zum Dateinamen – Pfad-Traversal ist damit ausgeschlossen.
    // Die Datei wird bei jeder Anfrage neu gelesen: So kann man index.html &
    // Co. ändern, ohne den Server neu zu starten (die Dateien sind klein).
    void ServeStatic(const char* file, const char* contentType, httplib::Response& res) const {
        std::string body;
        const std::string path = JoinPath(config_.webRoot, file);
        if (!ReadWholeFile(path, body)) {
            std::fprintf(stderr, "[Web] Datei fehlt oder ist nicht lesbar: %s\n", path.c_str());
            res.status = 500;
            res.set_content(std::string("Die Datei ") + file +
                                " der Verwaltungsseite fehlt. Bitte MONDSCHUERFER_WEB_ROOT bzw. --web-root "
                                "prüfen (Ordner mit index.html, app.js, style.css).\n",
                            kTextType);
            return;
        }
        res.set_content(body, contentType);
    }

    // --- GET /api/status ------------------------------------------------------
    void HandleStatus(httplib::Response& res) {
        ServerStatus status;
        if (!ReadStatus(status, res)) return;
        res.set_content(BuildStatusJson(status), kJsonType);
    }

    // --- POST /api/settings ---------------------------------------------------
    // Ablauf: CSRF-Schutz -> aktuellen Stand holen -> Formular streng prüfen ->
    // Sanitize -> in SharedState ablegen. Der Spiel-Thread übernimmt die Werte
    // zu Beginn seines nächsten Schritts (TakeSettingsRequest); erst danach
    // (spätestens eine Sekunde später) zeigt /api/status die neuen Werte.
    void HandleSettings(const httplib::Request& req, httplib::Response& res) {
        if (!CheckSameOriginPost(req, res)) return;
        if (!req.body.empty() &&
            LowerAscii(req.get_header_value("Content-Type")).rfind("application/x-www-form-urlencoded", 0) != 0) {
            SendJsonError(res, 400, "Formulardaten (application/x-www-form-urlencoded) erwartet.");
            return;
        }
        ServerStatus status;
        if (!ReadStatus(status, res)) return;
        std::map<std::string, std::string> fields;
        std::string error;
        GameSettings next = status.settings;   // fehlende Felder = aktueller Wert
        if (!ParseFormBody(req.body, fields, error) || !ApplyForm(fields, next, error)) {
            SendJsonError(res, 400, error);
            return;
        }
        Sanitize(next);   // passt enemyShips zum Schwierigkeitsgrad an
        if (!WithShared(res, [&](SharedState& s) { s.RequestSettings(next); })) return;
        const bool rebuild = NeedsWorldRebuild(status.settings, next);
        SendSettingsOk(res, next, rebuild,
                       SettingsMessage(status.settings, next, rebuild, status.playersOnline, "Gespeichert."));
    }

    // --- POST /api/ships ---------------------------------------------------------
    // Wie /api/settings, aber nur für die Schiffe. Der Spiel-Thread übernimmt
    // sie zu Beginn seines nächsten Schritts (TakeShipsRequest) und schickt sie
    // sofort an alle Spieler; eine neue Welt ist dafür nicht nötig.
    void HandleShips(const httplib::Request& req, httplib::Response& res) {
        if (!CheckSameOriginPost(req, res)) return;
        if (LowerAscii(req.get_header_value("Content-Type")).rfind("application/x-www-form-urlencoded", 0) != 0) {
            SendJsonError(res, 400, "Formulardaten (application/x-www-form-urlencoded) erwartet.");
            return;
        }
        ServerStatus status;
        if (!ReadStatus(status, res)) return;
        std::map<std::string, std::string> fields;
        std::string error;
        std::vector<ShipType> ships;
        if (!ParseFormBody(req.body, fields, error) || !ApplyShipForm(fields, status, ships, error)) {
            SendJsonError(res, 400, error);
            return;
        }
        GameSettings next = status.settings;
        next.ships = ships;
        Sanitize(next);
        if (!WithShared(res, [&](SharedState& s) { s.RequestShips(next.ships); })) return;
        JsonWriter w;
        w.BeginObject();
        w.Key("ok").Bool(true);
        w.Key("ships");
        WriteShips(w, next.ships);
        w.Key("message").String(SameShips(status.settings.ships, next.ships)
                                    ? "Gespeichert. Es hat sich nichts geändert."
                                    : "Gespeichert. Die Änderungen gelten ab sofort, auch für Schiffe, die Spieler schon besitzen.");
        w.EndObject();
        res.status = 200;
        res.set_content(w.Result(), kJsonType);
    }

    // --- POST /api/maps ------------------------------------------------------------
    // Bereich "Karten": action = activate | delete (file = Karte, bei activate
    // "" = Standardkarte) oder allow | deny (name = Spieler). Der Spiel-Thread
    // führt den Auftrag im nächsten Schritt aus (TakeMapCommands); ob es
    // geklappt hat (z. B. ob es das Konto gibt), steht danach als mapNotice im Status.
    void HandleMaps(const httplib::Request& req, httplib::Response& res) {
        if (!CheckSameOriginPost(req, res)) return;
        if (LowerAscii(req.get_header_value("Content-Type")).rfind("application/x-www-form-urlencoded", 0) != 0) {
            SendJsonError(res, 400, "Formulardaten (application/x-www-form-urlencoded) erwartet.");
            return;
        }
        ServerStatus status;
        if (!ReadStatus(status, res)) return;
        std::map<std::string, std::string> fields;
        std::string error;
        if (!ParseFormBody(req.body, fields, error)) {
            SendJsonError(res, 400, error);
            return;
        }
        const std::string action = fields.count("action") ? fields["action"] : std::string();
        MapCommand cmd;
        if (action == "activate" || action == "delete") {
            cmd.kind = action == "activate" ? MapCommand::Kind::Activate : MapCommand::Kind::Delete;
            cmd.arg = fields.count("file") ? fields["file"] : std::string();
            const bool known = std::any_of(status.maps.begin(), status.maps.end(),
                                           [&](const MapStatus& m) { return m.file == cmd.arg; });
            if (!known && !(cmd.arg.empty() && cmd.kind == MapCommand::Kind::Activate)) {
                SendJsonError(res, 400, "Diese Karte gibt es nicht (mehr).");
                return;
            }
        } else if (action == "allow" || action == "deny") {
            cmd.kind = action == "allow" ? MapCommand::Kind::Allow : MapCommand::Kind::Deny;
            cmd.arg = TrimSpaces(fields.count("name") ? fields["name"] : std::string());
            // Spielernamen: 3–16 Zeichen A-Z a-z 0-9 _ - (net::ValidPlayerName)
            const bool valid = cmd.arg.size() >= 3 && cmd.arg.size() <= 16 &&
                               std::all_of(cmd.arg.begin(), cmd.arg.end(), [](char c) {
                                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                          c == '_' || c == '-';
                               });
            if (!valid) {
                SendJsonError(res, 400, "Ungültiger Spielername (3–16 Zeichen: Buchstaben, Ziffern, _ und -).");
                return;
            }
        } else {
            SendJsonError(res, 400, "Unbekannte Aktion.");
            return;
        }
        bool queued = false;
        if (!WithShared(res, [&](SharedState& s) { queued = s.RequestMapCommand(cmd); })) return;
        if (!queued) {
            SendJsonError(res, 503, "Der Server ist gerade beschäftigt. Bitte gleich noch einmal versuchen.");
            return;
        }
        JsonWriter w;
        w.BeginObject().Key("ok").Bool(true).Key("message").String("Wird übernommen …").EndObject();
        res.status = 200;
        res.set_content(w.Result(), kJsonType);
    }

    // --- POST /api/settings/reset -----------------------------------------------
    // Zurück auf die Werte aus docker-compose.yml (Umgebungsvariablen). Welche
    // das sind, weiß nur der Spiel-Thread; er veröffentlicht sie in status.defaults.
    void HandleReset(const httplib::Request& req, httplib::Response& res) {
        if (!CheckSameOriginPost(req, res)) return;
        ServerStatus status;
        if (!ReadStatus(status, res)) return;
        GameSettings defaults = status.defaults;
        Sanitize(defaults);
        if (!WithShared(res, [](SharedState& s) { s.RequestReset(); })) return;
        const bool rebuild = NeedsWorldRebuild(status.settings, defaults);
        SendSettingsOk(res, defaults, rebuild,
                       SettingsMessage(status.settings, defaults, rebuild, status.playersOnline,
                                       "Auf die Standardwerte aus docker-compose.yml zurückgesetzt."));
    }

private:
    bool CredentialsMatch(const std::string& user, const std::string& password) const {
        const crypto::Digest u = crypto::Sha256(user);
        const crypto::Digest p = crypto::Sha256(password);
        // Beide Vergleiche IMMER ausführen (kein &&-Kurzschluss vor dem zweiten),
        // damit die Laufzeit nicht verrät, ob der Benutzername stimmte.
        const bool userOk = crypto::ConstantTimeEqual(u.data(), userHash_.data(), u.size());
        const bool passOk = crypto::ConstantTimeEqual(p.data(), passHash_.data(), p.size());
        return userOk & passOk;
    }

    static void SendLocked(httplib::Response& res, int seconds) {
        res.status = 429;
        res.set_header("Retry-After", std::to_string(seconds));
        res.set_content("Zu viele falsche Anmeldungen. Bitte in " + std::to_string(seconds) +
                            " Sekunden erneut versuchen.\n",
                        kTextType);
    }

    // Führt f mit dem SharedState aus, solange der Server nicht beendet wird.
    // Sonst 503 in res und false.
    template <typename F>
    bool WithShared(httplib::Response& res, F&& f) {
        std::lock_guard<std::mutex> lock(sharedMutex_);
        if (!shared_) {
            SendJsonError(res, 503, "Server wird beendet");
            return false;
        }
        f(*shared_);
        return true;
    }

    // Kopie des aktuellen Serverzustands holen. false (und 503 in res), wenn der
    // Spiel-Thread noch keinen Zustand veröffentlicht hat (erste Sekunde nach
    // dem Start) oder der Server gerade beendet wird.
    bool ReadStatus(ServerStatus& out, httplib::Response& res) {
        bool has = false;
        if (!WithShared(res, [&](SharedState& s) { has = s.GetStatus(out); })) return false;
        if (!has) {
            res.set_header("Retry-After", "2");
            SendJsonError(res, 503, "Server startet noch");
        }
        return has;
    }

    // Absenderadresse für die Sperrliste. Normalerweise req.remote_addr.
    // Hinter Caddy (Profil "online") wäre das aber für ALLE Besucher die
    // Adresse des Caddy-Containers: Ein Angreifer könnte dann mit ein paar
    // Fehlversuchen alle aussperren, auch den Admin, und zwar immer wieder.
    // Deshalb: Kommt die Anfrage vom vertrauenswürdigen Proxy
    // (config_.trustedProxy, in docker-compose.yml "caddy"), gilt die Adresse,
    // die er im Header "X-Real-IP" mitschickt (server/Caddyfile setzt ihn).
    // Von jedem anderen Absender wird der Header ignoriert, sonst könnte sich
    // jeder für jeden Versuch eine neue Adresse ausdenken.
    std::string ClientAddress(const httplib::Request& req) {
        if (config_.trustedProxy.empty() || !IsTrustedProxy(req.remote_addr)) return req.remote_addr;
        const std::string real = req.get_header_value("X-Real-IP");
        unsigned char buf[16];
        if (inet_pton(AF_INET, real.c_str(), buf) == 1 || inet_pton(AF_INET6, real.c_str(), buf) == 1) return real;
        return req.remote_addr;
    }

    // Ist addr eine Adresse des Proxys? Der Name wird über DNS aufgelöst (in
    // Docker liefert der eingebaute DNS die Adresse des Containers "caddy";
    // läuft Caddy nicht, gibt es keine Adresse und damit kein Vertrauen).
    // Ergebnis 30 s zwischenspeichern; aufgelöst wird ohne gesperrten Mutex,
    // damit eine langsame DNS-Antwort nicht alle Arbeits-Threads aufhält.
    bool IsTrustedProxy(const std::string& addr) {
        const std::string a = addr.rfind("::ffff:", 0) == 0 ? addr.substr(7) : addr;
        bool refresh = false;
        {
            std::lock_guard<std::mutex> lock(proxyMutex_);
            const auto now = Clock::now();
            if (!proxyResolving_ && (!proxyResolved_ || now - proxyResolvedAt_ > std::chrono::seconds(30))) {
                proxyResolving_ = true;
                refresh = true;
            }
        }
        if (refresh) {
            std::vector<std::string> addrs = ResolveHost(config_.trustedProxy);
            std::lock_guard<std::mutex> lock(proxyMutex_);
            proxyAddrs_ = std::move(addrs);
            proxyResolvedAt_ = Clock::now();
            proxyResolved_ = true;
            proxyResolving_ = false;
        }
        std::lock_guard<std::mutex> lock(proxyMutex_);
        return std::find(proxyAddrs_.begin(), proxyAddrs_.end(), a) != proxyAddrs_.end();
    }

    // Alle IP-Adressen eines Namens (oder die Adresse selbst, wenn host schon
    // eine ist) als Text, z. B. "172.18.0.3".
    static std::vector<std::string> ResolveHost(const std::string& host) {
        std::vector<std::string> out;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return out;
        for (addrinfo* p = res; p; p = p->ai_next) {
            char buf[INET6_ADDRSTRLEN] = {0};
            const void* src = nullptr;
            if (p->ai_family == AF_INET) src = &reinterpret_cast<const sockaddr_in*>(p->ai_addr)->sin_addr;
            else if (p->ai_family == AF_INET6) src = &reinterpret_cast<const sockaddr_in6*>(p->ai_addr)->sin6_addr;
            if (src && inet_ntop(p->ai_family, src, buf, sizeof(buf))) out.emplace_back(buf);
        }
        freeaddrinfo(res);
        return out;
    }

    WebConfig config_;
    std::mutex proxyMutex_;                     // schützt die vier proxy*-Felder
    std::vector<std::string> proxyAddrs_;       // aufgelöste Adressen von config_.trustedProxy
    Clock::time_point proxyResolvedAt_{};
    bool proxyResolved_ = false;
    bool proxyResolving_ = false;               // ein Thread löst gerade auf
    // Wir speichern NICHT Benutzer/Passwort zum Vergleichen, sondern ihre
    // SHA-256-Werte. Beim Vergleich wird die Eingabe ebenfalls gehasht; beide
    // Hashes sind immer 32 Byte lang, und ConstantTimeEqual braucht immer
    // gleich lange. So verrät die Antwortzeit weder, wie viele Zeichen richtig
    // waren, noch wie lang das echte Passwort ist.
    crypto::Digest userHash_{};
    crypto::Digest passHash_{};
    LoginGuard guard_;

    std::mutex sharedMutex_;          // schützt nur den Zeiger shared_ (SharedState hat eigenen Mutex)
    SharedState* shared_ = nullptr;   // gehört main.cpp; nullptr nach Stop()
};

// Richtet einen neuen httplib::Server ein: Grenzen, Sicherheits-Header,
// Anmeldung, Routen. Alle Handler halten eine Referenz auf ctx (siehe oben).
void ConfigureServer(httplib::Server& svr, const std::shared_ptr<WebContext>& ctx) {
    // Kleiner Thread-Pool mit begrenzter Warteschlange: Die Seite hat nur
    // wenige Besucher; mehr Threads würden bei einem Angriff nur Speicher kosten.
    svr.new_task_queue = [] { return new httplib::ThreadPool(kWorkerThreads, kMaxQueuedConnections); };
    svr.set_payload_max_length(kMaxPayload);
    svr.set_read_timeout(kIoTimeoutSec, 0);
    svr.set_write_timeout(kIoTimeoutSec, 0);
    svr.set_keep_alive_max_count(kKeepAliveMaxCount);
    svr.set_keep_alive_timeout(kKeepAliveTimeoutSec);

    // Optionen für den lauschenden Socket. httplib setzt unter Linux
    // standardmäßig SO_REUSEPORT – damit könnten zwei Serverprozesse
    // denselben Port gleichzeitig öffnen und sich die Besucher zufällig
    // teilen, und Start() würde "Port belegt" nie bemerken. Wir wollen:
    //   Linux/macOS: SO_REUSEADDR – nach einem Neustart sofort wieder
    //                binden, auch wenn alte Verbindungen noch "nachklingen"
    //                (TIME_WAIT); ein zweiter lauschender Prozess scheitert.
    //   Windows:     SO_EXCLUSIVEADDRUSE – dort würde SO_REUSEADDR sogar
    //                erlauben, einen belegten Port zu "stehlen".
    svr.set_socket_options([](socket_t sock) {
        int yes = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes),
                   static_cast<int>(sizeof(yes)));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, static_cast<socklen_t>(sizeof(yes)));
#endif
    });

    // Diese Header bekommt JEDE Antwort (auch 401, 404 und Fehlerseiten):
    //   CSP: Die Seite darf nur Dateien von diesem Server laden, keine
    //        Inline-Skripte ausführen und nicht in fremde Seiten eingebettet
    //        werden. Selbst wenn ein Spielername HTML enthielte, liefe kein Code.
    //   nosniff: Der Browser darf den Content-Type nicht "erraten".
    //   X-Frame-Options: Schutz gegen Clickjacking (ältere Browser).
    //   Referrer-Policy: Die Adresse der Seite wird nicht weitergegeben.
    //   Cache-Control: Nichts davon auf der Festplatte des Browsers speichern.
    svr.set_default_headers({
        {"Content-Security-Policy",
         "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; "
         "frame-ancestors 'none'; base-uri 'none'; form-action 'self'"},
        {"X-Content-Type-Options", "nosniff"},
        {"X-Frame-Options", "DENY"},
        {"Referrer-Policy", "no-referrer"},
        {"Cache-Control", "no-store"},
        {"Cross-Origin-Opener-Policy", "same-origin"},
        {"Cross-Origin-Resource-Policy", "same-origin"},
    });

    svr.set_pre_routing_handler(
        [ctx](const httplib::Request& req, httplib::Response& res) { return ctx->Authenticate(req, res); });

    // Routen: httplib vergleicht den Pfad mit einem regulären Ausdruck über
    // den GANZEN Pfad. Der Punkt muss daher maskiert werden (\.), sonst
    // passte "/app.js" auch auf "/appXjs".
    auto staticFile = [ctx](const char* file, const char* type) {
        return [ctx, file, type](const httplib::Request&, httplib::Response& res) { ctx->ServeStatic(file, type, res); };
    };
    svr.Get("/", staticFile("index.html", "text/html; charset=utf-8"));
    svr.Get(R"(/index\.html)", staticFile("index.html", "text/html; charset=utf-8"));
    svr.Get(R"(/app\.js)", staticFile("app.js", "text/javascript; charset=utf-8"));
    svr.Get(R"(/style\.css)", staticFile("style.css", "text/css; charset=utf-8"));
    svr.Get(R"(/api/status)", [ctx](const httplib::Request&, httplib::Response& res) { ctx->HandleStatus(res); });
    svr.Post(R"(/api/settings)",
             [ctx](const httplib::Request& req, httplib::Response& res) { ctx->HandleSettings(req, res); });
    svr.Post(R"(/api/settings/reset)",
             [ctx](const httplib::Request& req, httplib::Response& res) { ctx->HandleReset(req, res); });
    svr.Post(R"(/api/ships)", [ctx](const httplib::Request& req, httplib::Response& res) { ctx->HandleShips(req, res); });
    svr.Post(R"(/api/maps)", [ctx](const httplib::Request& req, httplib::Response& res) { ctx->HandleMaps(req, res); });

    // Fehlerantworten ohne eigenen Text (z. B. 404 von httplib) bekommen
    // einen kurzen deutschen Text. Eigene Texte bleiben unverändert.
    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (!res.body.empty()) return httplib::Server::HandlerResponse::Unhandled;
        const char* text = "Fehler.\n";
        switch (res.status) {
            case 400: text = "Ungültige Anfrage.\n"; break;
            case 404: text = "Nicht gefunden.\n"; break;
            case 405: text = "Methode nicht erlaubt.\n"; break;
            case 413: text = "Anfrage zu groß.\n"; break;
            default: break;
        }
        res.set_content(text, kTextType);
        return httplib::Server::HandlerResponse::Handled;
    });

    // Ausnahmen in einem Handler: Ohne eigenen Handler würde httplib den
    // Ausnahmetext als Header "EXCEPTION_WHAT" an den Browser schicken.
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[Web] Interner Fehler: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[Web] Interner Fehler (unbekannt)\n");
        }
        res.status = 500;
        res.set_content("Interner Fehler.\n", kTextType);
    });
}

} // namespace

// ===========================================================================
// WebServer::Impl – Pimpl ("pointer to implementation"): So muss
// WebServer.hpp httplib.h nicht einbinden, und main.cpp bleibt schlank.
// ===========================================================================
struct WebServer::Impl {
    std::shared_ptr<WebContext> ctx;
    std::shared_ptr<httplib::Server> server;   // geteilt mit dem Web-Thread (siehe Stop)
    std::thread thread;                        // der Web-Thread (listen_after_bind)
    std::shared_ptr<std::atomic<bool>> threadDone;   // setzt der Web-Thread ganz am Ende
};

// ===========================================================================
// Öffentliche Schnittstelle
// ===========================================================================
WebServer::WebServer() : impl_(std::make_unique<Impl>()) {}

WebServer::~WebServer() { Stop(); }

bool WebServer::Start(const WebConfig& config, SharedState& shared) {
    if (impl_->server) {
        std::fprintf(stderr, "[Web] Die Verwaltungsseite läuft bereits.\n");
        return false;
    }

    // 1. Zugangsdaten prüfen. Lieber gar keine Webseite als eine ungeschützte.
    std::string reason;
    if (!CheckAdminCredentials(config.user, config.password, reason)) {
        std::fprintf(stderr,
                     "[Web] Die Verwaltungsseite wird NICHT gestartet: %s\n"
                     "[Web] Bitte in docker-compose.yml (bzw. als Umgebungsvariablen) ADMIN_USER und ein sicheres\n"
                     "[Web] ADMIN_PASSWORD setzen (mindestens 10 Zeichen, kein Standardwort). Das Spiel läuft trotzdem.\n",
                     reason.c_str());
        return false;
    }

    // Fehlende Dateien früh melden (die Seite startet trotzdem; der Browser
    // bekommt dann eine Fehlermeldung mit Hinweis).
    for (const char* file : {"index.html", "app.js", "style.css"}) {
        std::ifstream probe(JoinPath(config.webRoot, file), std::ios::binary);
        if (!probe)
            std::fprintf(stderr, "[Web] Warnung: %s nicht gefunden (Web-Ordner: %s)\n", file, config.webRoot.c_str());
    }

    // 2. Server anlegen und konfigurieren (für jeden Start neu).
    auto ctx = std::make_shared<WebContext>(config, &shared);
    auto server = std::make_shared<httplib::Server>();
    ConfigureServer(*server, ctx);

    // 3. Port öffnen. bind_to_port() läuft noch im aufrufenden Thread, damit
    //    wir einen Fehler (Port belegt, keine Berechtigung) hier melden können.
    if (!server->bind_to_port(config.bindAddress, config.port)) {
        std::fprintf(stderr, "[Web] Port %d auf %s kann nicht geöffnet werden (schon belegt?). Webseite aus.\n",
                     config.port, config.bindAddress.c_str());
        return false;
    }

    // 4. Web-Thread starten: Er nimmt ab jetzt Verbindungen an, bis Stop()
    //    den Server-Socket schließt. Der Thread hält eine eigene Referenz auf
    //    den Server (shared_ptr), damit der Server garantiert so lange lebt
    //    wie der Thread – auch im Notfall-Fall von Stop().
    //    wait_until_ready() wartet, bis httplib "läuft" meldet; erst dann
    //    wirkt ein späteres stop() zuverlässig.
    auto done = std::make_shared<std::atomic<bool>>(false);
    impl_->thread = std::thread([server, done] {
        if (!server->listen_after_bind()) std::fprintf(stderr, "[Web] Der Webserver wurde unerwartet beendet.\n");
        done->store(true);
    });
    server->wait_until_ready();

    impl_->ctx = std::move(ctx);
    impl_->server = std::move(server);
    impl_->threadDone = std::move(done);

    std::fprintf(stdout, "[Web] Verwaltungsseite läuft auf http://%s:%d/ (Benutzer: %s)\n",
                 config.bindAddress.c_str(), config.port, config.user.c_str());
    std::fflush(stdout);
    return true;
}

void WebServer::Stop() {
    if (!impl_ || !impl_->server) return;

    // a) Ab sofort keine Zugriffe mehr auf SharedState (gehört main.cpp und
    //    wird nach Stop() eventuell gelöscht).
    impl_->ctx->Detach();

    // b) stop() schließt den lauschenden Socket. listen_after_bind() kehrt
    //    zurück, sobald die Arbeits-Threads ihre laufenden Verbindungen
    //    beendet haben – normalerweise nach Millisekunden.
    impl_->server->stop();

    // c) Höchstens kStopWait warten. Ein Angreifer könnte seine Anfrage
    //    absichtlich Byte für Byte schicken (jedes Byte vor Ablauf der
    //    5-s-Zeitgrenze) und damit einen Arbeits-Thread minutenlang festhalten.
    //    Docker gibt dem Prozess beim Beenden aber nur 10 s – die braucht der
    //    Spiel-Thread zum Speichern. Deshalb wird der Web-Thread in diesem
    //    Fall abgekoppelt (detach): Er hält Server und WebContext selbst am
    //    Leben, kommt an SharedState nicht mehr heran und endet mit dem Prozess.
    constexpr auto kStopWait = std::chrono::seconds(2);
    const auto deadline = Clock::now() + kStopWait;
    while (!impl_->threadDone->load() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    if (impl_->threadDone->load()) {
        impl_->thread.join();
    } else {
        std::fprintf(stderr, "[Web] Eine Verbindung hängt noch; der Web-Thread wird nicht weiter abgewartet.\n");
        impl_->thread.detach();
    }
    impl_->server.reset();
    impl_->ctx.reset();
    impl_->threadDone.reset();
}
