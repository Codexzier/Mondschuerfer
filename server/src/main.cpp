// ---------------------------------------------------------------------------
// main.cpp – Einstiegspunkt des Mondschürfer-Servers.
//
// Aufgaben:
//   1. Konfiguration einlesen: zuerst Umgebungsvariablen (so konfiguriert
//      docker-compose.yml den Server), dann die Kommandozeile (überschreibt).
//   2. Datenordner öffnen (Accounts: Konten, Bestenliste, Salz, Seed) und die
//      auf der Webseite gespeicherten Einstellungen laden.
//   3. Spielserver (GameServer, ENet/UDP) und Verwaltungs-Webseite
//      (WebServer, HTTP, eigener Thread) starten.
//   4. Auf Strg+C / "docker stop" (Signale SIGINT/SIGTERM) warten und dann
//      sauber beenden: alle Spieler speichern, Clients benachrichtigen.
//
// Sonderfall --benchmark N: misst nur die Rechenzeit und Datenmenge für N
// simulierte Spieler (ohne Netzwerk) und beendet sich danach.
//
// Threads: main() selbst ist der Spiel-Thread (GameServer::Run). WebServer
// startet einen zweiten Thread; beide tauschen Daten nur über SharedState aus.
//
// Abhängigkeiten: GameServer.hpp, Accounts.hpp, WebServer.hpp, SharedState.hpp,
// shared/GameSettings.hpp (Standardwerte und Prüfung der Einstellungen).
// ---------------------------------------------------------------------------
#include "Accounts.hpp"
#include "GameServer.hpp"
#include "GameSettings.hpp"
#include "SharedState.hpp"
#include "WebServer.hpp"
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Signale
//
// Ein Signal-Handler darf fast nichts tun (er unterbricht das Programm an
// beliebiger Stelle). Erlaubt ist das Setzen einer atomaren Variable. Die
// Hauptschleife (GameServer::Run) prüft sie bei jedem Durchlauf, spätestens
// alle ~33 ms, und beendet sich dann geordnet.
// ---------------------------------------------------------------------------
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "atomic<bool> muss sperrfrei sein (Signal-Handler)");

extern "C" void OnSignal(int) {
    // Zweites Strg+C: sofort beenden, falls das geordnete Beenden hängt
    if (g_stop.exchange(true)) std::_Exit(130);
}

// ---------------------------------------------------------------------------
// Konfiguration
// ---------------------------------------------------------------------------
struct Options {
    int port = net::DEFAULT_PORT;
    int webPort = 8080;
    // Standard: nur dieser Rechner. Docker setzt 0.0.0.0 selbst (docker-compose.yml),
    // weil Anfragen dort über eine eigene Netzwerkkarte des Containers kommen.
    std::string webBind = "127.0.0.1";
    std::string trustedProxy;      // Reverse-Proxy, dessen X-Real-IP gilt (z. B. "caddy")
    std::string dataDir = "data";
    std::string webRoot;           // leer = <Ordner der Programmdatei>/web
    bool noWeb = false;
    int maxPerIp = 8;
    int benchmarkPlayers = 0;      // > 0: Benchmark statt Server
    double benchmarkSeconds = 20.0;
    GameSettings defaults;         // Spielparameter aus den Umgebungsvariablen
    std::string adminUser, adminPassword;
};

// Umgebungsvariable lesen; leer zählt wie "nicht gesetzt" (docker-compose
// setzt bei ${VAR:-} einen leeren Text).
bool Env(const char* name, std::string& out) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    out = v;
    return true;
}

bool ParseIntText(const std::string& s, int& out) {
    int v = 0;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto res = std::from_chars(first, last, v);
    if (res.ec != std::errc() || res.ptr != last) return false;
    out = v;
    return true;
}

bool ParseDoubleText(const std::string& s, double& out) {
    std::istringstream is(s);
    is.imbue(std::locale::classic());
    double v = 0.0;
    is >> v;
    if (is.fail() || !is.eof()) return false;
    out = v;
    return true;
}

// Zahl aus einer Umgebungsvariable mit Bereichsprüfung. Ungültig -> Warnung,
// der bisherige Wert bleibt.
void EnvInt(const char* name, int& value, int lo, int hi) {
    std::string text;
    if (!Env(name, text)) return;
    int v = 0;
    if (!ParseIntText(text, v)) {
        std::cerr << "Warnung: " << name << "=\"" << text << "\" ist keine Zahl – verwende " << value << "\n";
        return;
    }
    if (v < lo || v > hi) {
        const int clamped = v < lo ? lo : hi;
        std::cerr << "Warnung: " << name << "=" << v << " liegt außerhalb von " << lo << ".." << hi << " – verwende "
                  << clamped << "\n";
        v = clamped;
    }
    value = v;
}

void ReadEnvironment(Options& o) {
    EnvInt("MONDSCHUERFER_PORT", o.port, 1, 65535);
    EnvInt("MONDSCHUERFER_WEB_PORT", o.webPort, 1, 65535);
    Env("MONDSCHUERFER_WEB_BIND", o.webBind);
    Env("MONDSCHUERFER_DATA", o.dataDir);
    Env("MONDSCHUERFER_WEB_ROOT", o.webRoot);
    Env("MONDSCHUERFER_TRUSTED_PROXY", o.trustedProxy);
    EnvInt("MAX_CONNECTIONS_PER_IP", o.maxPerIp, 0, 1000);
    Env("ADMIN_USER", o.adminUser);
    Env("ADMIN_PASSWORD", o.adminPassword);

    // Spielparameter (Standardwerte, solange auf der Webseite nichts geändert wurde)
    GameSettings& s = o.defaults;
    Env("SERVER_NAME", s.serverName);
    std::string text;
    if (Env("DIFFICULTY", text) && !ParseDifficulty(text, s.difficulty))
        std::cerr << "Warnung: DIFFICULTY=\"" << text << "\" unbekannt (easy|medium|hard|custom) – verwende "
                  << DifficultyKey(s.difficulty) << "\n";
    const bool enemiesSet = std::getenv("ENEMY_SHIPS") && *std::getenv("ENEMY_SHIPS");
    EnvInt("ENEMY_SHIPS", s.enemyShips, 0, limits::MAX_ENEMY_SHIPS);
    if (enemiesSet && s.difficulty != Difficulty::Custom)
        std::cerr << "Hinweis: ENEMY_SHIPS gilt nur mit DIFFICULTY=custom (sonst legt der Schwierigkeitsgrad die Anzahl fest)\n";
    EnvInt("TRADE_STATIONS", s.tradeStations, 1, limits::MAX_TRADE_STATIONS);
    EnvInt("SHIPYARDS", s.shipyards, 1, limits::MAX_SHIPYARDS);
    if (Env("MOON_COLOR", text) && !ParseMoonColor(text, s.moon))
        std::cerr << "Warnung: MOON_COLOR=\"" << text << "\" unbekannt (grey|orange|blue) – verwende "
                  << MoonColorKey(s.moon) << "\n";
    EnvInt("MAX_PLAYERS", s.maxPlayers, 1, limits::MAX_PLAYERS_LIMIT);
    EnvInt("LIVES", s.lives, 1, limits::MAX_LIVES);
    EnvInt("MISSIONS", s.missions, 0, limits::MAX_MISSIONS);
    Sanitize(s);
}

void PrintUsage() {
    std::cout
        << "Mondschürfer-Server\n\n"
        << "Aufruf: mondschuerfer-server [Optionen]\n\n"
        << "  --port N           UDP-Port für das Spiel (Standard 7777, Umgebung MONDSCHUERFER_PORT)\n"
        << "  --web-port N       TCP-Port der Verwaltungsseite (Standard 8080, MONDSCHUERFER_WEB_PORT)\n"
        << "  --web-bind ADRESSE Adresse der Verwaltungsseite (Standard 127.0.0.1 = nur dieser Rechner,\n"
        << "                     MONDSCHUERFER_WEB_BIND; 0.0.0.0 = alle Netzwerkkarten, unverschlüsselt!)\n"
        << "  --trusted-proxy NAME  Reverse-Proxy (Name oder IP), dessen Header X-Real-IP als Adresse\n"
        << "                     des Besuchers gilt (MONDSCHUERFER_TRUSTED_PROXY; Docker: caddy)\n"
        << "  --data ORDNER      Datenordner (Standard ./data, MONDSCHUERFER_DATA)\n"
        << "  --web-root ORDNER  Dateien der Webseite (Standard <Programmordner>/web, MONDSCHUERFER_WEB_ROOT)\n"
        << "  --no-web           Verwaltungsseite nicht starten\n"
        << "  --max-per-ip N     max. Verbindungen je öffentlicher IP (Standard 8, 0 = unbegrenzt,\n"
        << "                     MAX_CONNECTIONS_PER_IP)\n"
        << "  --benchmark N      Leistungstest mit N simulierten Spielern (ohne Netzwerk), dann Ende\n"
        << "  --seconds S        Dauer des Leistungstests in Spielsekunden (Standard 20)\n"
        << "  --help             diese Hilfe\n\n"
        << "Spielparameter (Umgebungsvariablen, auf der Webseite änderbar):\n"
        << "  SERVER_NAME, DIFFICULTY (easy|medium|hard|custom), ENEMY_SHIPS (nur custom),\n"
        << "  TRADE_STATIONS (1-" << limits::MAX_TRADE_STATIONS << "), SHIPYARDS (1-" << limits::MAX_SHIPYARDS
        << "), MOON_COLOR (grey|orange|blue), MAX_PLAYERS (1-" << limits::MAX_PLAYERS_LIMIT << "), LIVES (1-" << limits::MAX_LIVES << "),\n"
        << "  MISSIONS (0-" << limits::MAX_MISSIONS << ", Mini-Missionen gleichzeitig)\n"
        << "Verwaltungsseite: ADMIN_USER und ADMIN_PASSWORD (mind. 10 Zeichen) müssen gesetzt sein.\n";
}

// Liefert 0 = weiter, 1 = Fehler, 2 = Hilfe ausgegeben
int ParseArguments(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;
        bool hasValue = false;
        // "--port=7777" und "--port 7777" sind beide erlaubt
        const size_t eq = arg.find('=');
        if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
            value = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
            hasValue = true;
        }
        auto next = [&](const char* what) -> bool {
            if (hasValue) return true;
            if (i + 1 >= argc) {
                std::cerr << "Fehler: " << arg << " braucht einen Wert (" << what << ")\n";
                return false;
            }
            value = argv[++i];
            return true;
        };
        auto intArg = [&](int& target, int lo, int hi, const char* what) -> bool {
            if (!next(what)) return false;
            int v = 0;
            if (!ParseIntText(value, v) || v < lo || v > hi) {
                std::cerr << "Fehler: " << arg << " erwartet " << what << " (" << lo << ".." << hi << "), nicht \"" << value
                          << "\"\n";
                return false;
            }
            target = v;
            return true;
        };

        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 2;
        } else if (arg == "--port") {
            if (!intArg(o.port, 1, 65535, "Portnummer")) return 1;
        } else if (arg == "--web-port") {
            if (!intArg(o.webPort, 1, 65535, "Portnummer")) return 1;
        } else if (arg == "--web-bind") {
            if (!next("Adresse")) return 1;
            o.webBind = value;
        } else if (arg == "--data") {
            if (!next("Ordner")) return 1;
            o.dataDir = value;
        } else if (arg == "--trusted-proxy") {
            if (!next("Name")) return 1;
            o.trustedProxy = value;
        } else if (arg == "--web-root") {
            if (!next("Ordner")) return 1;
            o.webRoot = value;
        } else if (arg == "--no-web") {
            o.noWeb = true;
        } else if (arg == "--max-per-ip") {
            if (!intArg(o.maxPerIp, 0, 1000, "Anzahl")) return 1;
        } else if (arg == "--benchmark") {
            if (!intArg(o.benchmarkPlayers, 1, 1000, "Spielerzahl")) return 1;
        } else if (arg == "--seconds") {
            if (!next("Sekunden")) return 1;
            double v = 0.0;
            if (!ParseDoubleText(value, v) || v < 1.0 || v > 3600.0) {
                std::cerr << "Fehler: --seconds erwartet 1..3600, nicht \"" << value << "\"\n";
                return 1;
            }
            o.benchmarkSeconds = v;
        } else {
            std::cerr << "Fehler: unbekannte Option \"" << arg << "\" (--help zeigt alle Optionen)\n";
            return 1;
        }
    }
    return 0;
}

// Ordner, in dem die Programmdatei liegt (dort liegt nach dem Bauen "web/").
// Linux: /proc/self/exe zeigt auf die echte Datei, auch wenn das Programm
// über einen Link oder aus einem anderen Arbeitsordner gestartet wurde.
fs::path ExecutableDir(const char* argv0) {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) exe = fs::absolute(fs::path(argv0 ? argv0 : ""), ec);
    return exe.parent_path();
}

void PrintSettings(const GameSettings& s) {
    std::cout << "   Servername      : " << s.serverName << "\n"
              << "   Schwierigkeit   : " << DifficultyName(s.difficulty) << " (" << s.enemyShips
              << " feindliche Schiffe)\n"
              << "   Stationen       : " << s.tradeStations << " Handelsstation(en), " << s.shipyards << " Werft(en)\n"
              << "   Mondoberfläche  : " << MoonColorName(s.moon) << "\n"
              << "   Max. Spieler    : " << s.maxPlayers << " (Obergrenze " << limits::MAX_PLAYERS_LIMIT
              << "), dazu bis zu " << limits::MAX_SPECTATORS << " Zuschauer\n"
              << "   Leben pro Spieler: " << s.lives << " (höchstens " << limits::MAX_LIVES << ")\n"
              << "   Mini-Missionen  : " << s.missions << " gleichzeitig (höchstens " << limits::MAX_MISSIONS << ")\n"
              << "   Neue Konten     : " << (s.allowRegistration ? "erlaubt" : "nicht erlaubt") << "\n";
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    ReadEnvironment(opt);
    const int parsed = ParseArguments(argc, argv, opt);
    if (parsed == 2) return 0;
    if (parsed != 0) return 2;

    // --- Leistungstest ----------------------------------------------------------
    if (opt.benchmarkPlayers > 0) return GameServer::RunBenchmark(opt.benchmarkPlayers, opt.benchmarkSeconds, opt.defaults);

    // --- Signale ------------------------------------------------------------------
    // Schon vor dem Start einrichten: Kommt "docker stop" während des Starts,
    // beendet sich der Server trotzdem geordnet (Run kehrt sofort zurück).
    std::signal(SIGINT, OnSignal);    // Strg+C
    std::signal(SIGTERM, OnSignal);   // docker stop / systemd (unter Windows nicht ausgelöst)

    // --- Datenordner -------------------------------------------------------------
    Accounts accounts(opt.dataDir);
    std::string error;
    if (!accounts.Load(error)) {
        std::cerr << "FEHLER: " << error << "\n"
                  << "Der Server startet nicht, damit keine gespeicherten Daten überschrieben werden.\n";
        return 1;
    }
    // Einstellungen: Standardwerte aus der Umgebung, darüber ggf. die Datei der Webseite
    GameSettings settings = opt.defaults;
    const bool fromFile = accounts.LoadSettings(settings);
    if (accounts.LoadShips(settings.ships)) Sanitize(settings);   // Schiffe der Webseite (ships.txt)

    // --- Spielserver ----------------------------------------------------------------
    SharedState shared;
    ServerConfig sc;
    sc.port = opt.port;
    sc.defaults = opt.defaults;
    sc.settings = settings;
    sc.maxConnectionsPerIp = opt.maxPerIp;
    GameServer server(sc, accounts, &shared);
    if (!server.Start(error)) {
        std::cerr << "FEHLER: " << error << "\n";
        return 1;
    }

    // --- Verwaltungs-Webseite (eigener Thread) -----------------------------------------
    WebServer web;
    bool webOk = false;
    WebConfig wc;
    wc.bindAddress = opt.webBind;
    wc.port = opt.webPort;
    wc.user = opt.adminUser;
    wc.password = opt.adminPassword;
    wc.trustedProxy = opt.trustedProxy;
    wc.logDir = accounts.LoadLogDir();
    wc.webRoot = opt.webRoot.empty() ? (ExecutableDir(argc > 0 ? argv[0] : nullptr) / "web").string() : opt.webRoot;
    if (!opt.noWeb) {
        // WebServer::Start prüft selbst, ob Benutzer/Passwort sicher genug sind,
        // und schreibt den Grund einer Ablehnung nach stderr.
        webOk = web.Start(wc, shared);
        if (!webOk) {
            std::cerr << "\n*** WARNUNG: Die Verwaltungsseite wurde NICHT gestartet (Grund siehe oben).\n"
                      << "*** Das Spiel läuft trotzdem. Prüfe ADMIN_USER / ADMIN_PASSWORD (mind. 10 Zeichen)\n"
                      << "*** und ob TCP-Port " << opt.webPort << " frei ist.\n\n";
        }
    }

    // --- Startmeldung ---------------------------------------------------------------------
    std::error_code ec;
    const fs::path dataAbs = fs::absolute(opt.dataDir, ec);
    std::cout << "==================================================================\n"
              << " Mondschürfer-Server (Protokoll " << net::PROTOCOL_VERSION << ")\n"
              << "==================================================================\n"
              << " Spielport        : " << opt.port << "/UDP\n";
    if (opt.noWeb)
        std::cout << " Verwaltungsseite : abgeschaltet (--no-web)\n";
    else if (webOk)
        std::cout << " Verwaltungsseite : http://" << opt.webBind << ":" << opt.webPort << "/  (Benutzer \"" << opt.adminUser
                  << "\", Dateien aus " << wc.webRoot << ")\n";
    else
        std::cout << " Verwaltungsseite : NICHT AKTIV (siehe Warnung oben)\n";
    std::cout << " Datenordner      : " << (ec ? opt.dataDir : dataAbs.string()) << "  (" << accounts.Count() << " Konten, "
              << accounts.HighscoreCount() << " Bestenlisten-Einträge)\n"
              << " Verbindungen/IP  : " << (opt.maxPerIp > 0 ? std::to_string(opt.maxPerIp) : std::string("unbegrenzt"))
              << " (lokale Netze ausgenommen)\n"
              << " Einstellungen    : "
              << (fromFile ? "aus settings.txt (auf der Webseite geändert)" : "aus den Umgebungsvariablen") << "\n";
    PrintSettings(settings);
    std::cout << " Beenden mit Strg+C (Docker: docker compose stop)\n"
              << "==================================================================" << std::endl;

    // --- Hauptschleife (bis ein Signal kommt) -------------------------------------------------
    server.Run(g_stop);

    std::cout << "Signal empfangen – beende geordnet ..." << std::endl;
    server.Shutdown();
    if (webOk) web.Stop();
    return 0;
}
