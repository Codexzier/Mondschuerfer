// ---------------------------------------------------------------------------
// Accounts.cpp – Speichern und Laden der Serverdaten (siehe Accounts.hpp).
//
// Aufbau dieser Datei:
//   1. Hilfsfunktionen für Dateien: atomar schreiben, zeilenweise lesen.
//   2. Hilfsfunktionen für Text <-> Zahl, unabhängig von Spracheinstellungen
//      (ein deutsches System würde sonst "3,5" statt "3.5" schreiben und die
//      Datei wäre auf einem anderen System nicht mehr lesbar).
//   3. Die Klasse Accounts: Laden, Konten, Bestenliste, Serverdaten,
//      Einstellungen.
//
// Threads: Alle öffentlichen Funktionen laufen im Spiel-Thread. Nur das
// eigentliche Schreiben der Dateien (WriteFileAtomic, inkl. fsync) erledigt
// ein eigener Schreib-Thread (WriterLoop), damit langsame Datenträger das
// Spiel nicht ausbremsen. Gespeichert wird beim Abmelden, nach Ziel/Absturz
// und alle 30 Sekunden.
// ---------------------------------------------------------------------------
#include "Accounts.hpp"
#include "Crypto.hpp"
#include "Util.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <system_error>
#include <utility>

// fsync gibt es nur auf POSIX-Systemen (Linux, macOS). Unter Windows
// verzichten wir darauf; das atomare Umbenennen schützt trotzdem vor halb
// geschriebenen Dateien bei einem Absturz des Programms.
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* ACCOUNTS_FILE = "accounts.txt";
constexpr const char* HIGHSCORES_FILE = "highscores.txt";
constexpr const char* SERVER_FILE = "server.txt";
constexpr const char* SETTINGS_FILE = "settings.txt";

// Pfad als Text für Meldungen (UTF-8; in C++17 liefert u8string() einen std::string)
std::string PathText(const fs::path& p) { return p.u8string(); }

// ===========================================================================
// 1. Dateien
// ===========================================================================

#if !defined(_WIN32)
// Bittet das Betriebssystem, den Inhalt einer Datei (oder bei einem Ordner:
// den Verzeichniseintrag, also das Umbenennen) wirklich auf den Datenträger zu
// schreiben. Ohne das könnte nach einem Stromausfall trotz Umbenennen eine
// leere Datei übrig bleiben, weil der Inhalt noch im Zwischenspeicher lag.
void SyncPath(const fs::path& p) {
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return;
    (void)::fsync(fd);
    ::close(fd);
}
#endif

// Schreibt content "atomar" nach target:
//   1. in target.tmp schreiben (nur für den Besitzer lesbar),
//   2. auf den Datenträger zwingen (fsync),
//   3. target.tmp in target umbenennen.
// Das Umbenennen ist unter Linux ein einziger, unteilbarer Schritt: Wer die
// Datei liest, sieht entweder die alte oder die neue Fassung, nie eine halbe.
bool WriteFileAtomic(const fs::path& target, const std::string& content, std::string& error) {
    fs::path tmp = target;
    tmp += ".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Datei kann nicht geschrieben werden: " + PathText(tmp);
            return false;
        }
        // Rechte sofort einschränken, BEVOR Inhalt hineinkommt (Linux: 0600).
        // Fehler sind hier nicht schlimm (z. B. Windows/FAT kennen das nicht).
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            error = "Fehler beim Schreiben (Datenträger voll?): " + PathText(tmp);
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
#if !defined(_WIN32)
    SyncPath(tmp);
#else
    // Unter Windows (MinGW) überschreibt rename eine vorhandene Datei nicht.
    // Deshalb vorher löschen. Geht genau dazwischen etwas schief, liegt die
    // neue Fassung noch als *.tmp vor; ReadLines() nimmt sie dann.
    fs::remove(target, ec);
    ec.clear();
#endif
    fs::rename(tmp, target, ec);
    if (ec) {
        error = "Umbenennen fehlgeschlagen (" + ec.message() + "): " + PathText(tmp);
        return false;
    }
#if !defined(_WIN32)
    SyncPath(target.parent_path());
#endif
    return true;
}

enum class ReadResult { Ok, Missing, Error };

// Liest eine Textdatei zeilenweise (ohne Zeilenende, auch Windows-"\r\n").
// Fehlt die Datei, aber es gibt noch eine *.tmp (Abbruch beim Speichern unter
// Windows, siehe oben), wird diese gelesen.
ReadResult ReadLines(const fs::path& p, std::vector<std::string>& lines, std::string& error) {
    lines.clear();
    std::error_code ec;
    fs::path source = p;
    if (!fs::exists(source, ec)) {
        fs::path tmp = p;
        tmp += ".tmp";
        if (!fs::exists(tmp, ec)) return ReadResult::Missing;
        std::cerr << "Hinweis: " << PathText(p) << " fehlt, verwende " << PathText(tmp) << "\n";
        source = tmp;
    }
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        error = "Datei kann nicht gelesen werden (Zugriffsrechte?): " + PathText(source);
        return ReadResult::Error;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    if (in.bad()) {
        error = "Lesefehler: " + PathText(source);
        return ReadResult::Error;
    }
    return ReadResult::Ok;
}

// Wurden beim Lesen kaputte Zeilen übersprungen, wären sie nach dem nächsten
// Speichern endgültig weg. Deshalb vorher eine Kopie der Originaldatei
// anlegen (z. B. "accounts.txt.defekt-1760000000"), damit man von Hand
// retten kann, was zu retten ist.
void BackupDamagedFile(const fs::path& p) {
    std::error_code ec;
    fs::path backup = p;
    backup += ".defekt-" + std::to_string(static_cast<long long>(std::time(nullptr)));
    fs::copy_file(p, backup, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        fs::permissions(backup, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        std::cerr << "         Kopie der Originaldatei: " << PathText(backup) << "\n";
    }
}

// Leere Zeilen und Kommentare ('#') werden überall übersprungen
bool IsCommentOrEmpty(const std::string& line) {
    for (char c : line) {
        if (c == ' ' || c == '\t') continue;
        return c == '#';
    }
    return true;
}

// ===========================================================================
// 2. Text <-> Zahl
// ===========================================================================

// Zerlegt "a=1 b=2 c=x" in Paare. Werte dürfen keine Leerzeichen enthalten
// (Spielernamen und Hex-Schlüssel enthalten nie welche).
std::vector<std::pair<std::string, std::string>> SplitFields(const std::string& line) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream is(line);
    std::string token;
    while (is >> token) {
        const size_t eq = token.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        out.emplace_back(token.substr(0, eq), token.substr(eq + 1));
    }
    return out;
}

// Ganzzahl mit std::from_chars: unabhängig von der Spracheinstellung, ohne
// Ausnahmen, und der GANZE Text muss eine Zahl sein ("12abc" -> Fehler).
bool ParseI64(const std::string& s, long long& out) {
    if (s.empty()) return false;
    long long v = 0;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto res = std::from_chars(first, last, v);
    if (res.ec != std::errc() || res.ptr != last) return false;
    out = v;
    return true;
}

bool ParseInt(const std::string& s, int& out, int lo, int hi) {
    long long v = 0;
    if (!ParseI64(s, v)) return false;
    out = static_cast<int>(std::clamp<long long>(v, lo, hi));
    return true;
}

// Kommazahl: über einen Stream mit der "klassischen" C-Spracheinstellung, damit
// immer der Punkt als Dezimaltrenner gilt. NaN/unendlich werden abgelehnt.
bool ParseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    std::istringstream is(s);
    is.imbue(std::locale::classic());
    double v = 0.0;
    is >> v;
    // fail(): keine Zahl. !eof(): hinter der Zahl steht noch etwas ("1.5abc").
    if (is.fail() || !is.eof() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::string NumText(double v) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(3) << (std::isfinite(v) ? v : 0.0);
    return os.str();
}

// Zeitangaben aus Dateien begrenzen (kaputte Werte wie 1e300 abfangen):
// höchstens 100 Jahre in Sekunden.
double ClampSeconds(double v) { return std::clamp(v, 0.0, 100.0 * 365.0 * 24.0 * 3600.0); }

// Sortierung der Bestenliste: kürzeste Zeit zuerst, bei Gleichstand der ältere Eintrag
bool HighscoreLess(const HighscoreRecord& a, const HighscoreRecord& b) {
    if (a.seconds != b.seconds) return a.seconds < b.seconds;
    return a.when < b.when;
}

} // namespace

// ===========================================================================
// 3. Accounts
// ===========================================================================
Accounts::Accounts(std::string dataDir) : dir_(std::move(dataDir)) {
    // Auch ohne Datenordner (Benchmark) braucht der Server ein Salz
    crypto::RandomBytes(realmSalt_.data(), realmSalt_.size());
}

Accounts::~Accounts() {
    // Schreib-Thread bitten aufzuhören. Er erledigt vorher noch alle
    // ausstehenden Aufträge (siehe WriterLoop), es geht also nichts verloren.
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        stopWriter_ = true;
    }
    writeCv_.notify_all();
    if (writer_.joinable()) writer_.join();
}

// ---------------------------------------------------------------------------
// Schreib-Thread
// ---------------------------------------------------------------------------
void Accounts::QueueWrite(const std::string& path, std::string content, bool remove, const char* what) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    // Pro Datei nur der neueste Auftrag: ein älterer, noch nicht geschriebener
    // Stand wird einfach ersetzt (er wäre sowieso sofort überschrieben worden).
    WriteJob& job = pendingWrites_[path];
    job.content = std::move(content);
    job.remove = remove;
    job.what = what;
    if (!writer_.joinable()) writer_ = std::thread(&Accounts::WriterLoop, this);
    writeCv_.notify_one();
}

// Läuft im Schreib-Thread. Die Sperre (writeMutex_) wird nur zum Abholen
// eines Auftrags gehalten, nicht während des Schreibens: so muss der
// Spiel-Thread beim nächsten QueueWrite nie auf die Festplatte warten.
void Accounts::WriterLoop() {
    std::unique_lock<std::mutex> lock(writeMutex_);
    while (true) {
        writeCv_.wait(lock, [&] { return stopWriter_ || !pendingWrites_.empty(); });
        if (pendingWrites_.empty()) {
            if (stopWriter_) break;
            continue;
        }
        auto it = pendingWrites_.begin();
        const std::string path = it->first;
        WriteJob job = std::move(it->second);
        pendingWrites_.erase(it);
        writerBusy_ = true;
        lock.unlock();

        bool ok = true;
        std::string error;
        if (job.remove) {
            std::error_code ec;
            fs::remove(path, ec);
            fs::path tmp = path;
            tmp += ".tmp";
            fs::remove(tmp, ec);
        } else {
            ok = WriteFileAtomic(path, job.content, error);
            if (!ok) std::cerr << "FEHLER beim Speichern (" << job.what << "): " << error << std::endl;
        }

        lock.lock();
        writerBusy_ = false;
        if (!ok) ++writeErrors_;
        if (pendingWrites_.empty()) idleCv_.notify_all();
    }
}

bool Accounts::Flush() {
    std::unique_lock<std::mutex> lock(writeMutex_);
    idleCv_.wait(lock, [&] { return pendingWrites_.empty() && !writerBusy_; });
    const bool ok = writeErrors_ == 0;
    writeErrors_ = 0;
    return ok;
}

std::string Accounts::PathOf(const char* file) const { return (fs::path(dir_) / file).string(); }

bool Accounts::Load(std::string& error) {
    if (dir_.empty()) return true;   // nur im Arbeitsspeicher

    // --- Ordner anlegen ------------------------------------------------------
    std::error_code ec;
    const fs::path dir(dir_);
    const bool existed = fs::exists(dir, ec);
    if (!existed) {
        fs::create_directories(dir, ec);
        if (ec) {
            error = "Datenordner kann nicht angelegt werden (" + ec.message() + "): " + PathText(dir);
            return false;
        }
        // Neu angelegter Ordner: nur der Besitzer darf hinein (Linux: 0700).
        // Einen vorhandenen Ordner (z. B. Docker-Volume) lassen wir, wie er ist.
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    } else if (!fs::is_directory(dir, ec)) {
        error = "Datenordner ist kein Verzeichnis: " + PathText(dir);
        return false;
    }

    std::vector<std::string> lines;

    // --- server.txt: Salz, Seed, abgelieferte Ressourcen ----------------------
    bool haveSalt = false;
    ReadResult rr = ReadLines(PathOf(SERVER_FILE), lines, error);
    if (rr == ReadResult::Error) return false;
    for (const auto& line : lines) {
        if (IsCommentOrEmpty(line)) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        long long v = 0;
        if (key == "realmSalt") {
            haveSalt = crypto::FromHex(value, realmSalt_.data(), realmSalt_.size());
        } else if (key == "worldSeed" && ParseI64(value, v) && v >= 0 && v <= 0xFFFFFFFFLL) {
            SetWorldSeed(static_cast<uint32_t>(v));
        } else if (key == "totalDelivered" && ParseI64(value, v)) {
            SetTotalDelivered(v);
        }
    }
    if (!haveSalt) crypto::RandomBytes(realmSalt_.data(), realmSalt_.size());

    // --- accounts.txt -----------------------------------------------------------
    rr = ReadLines(PathOf(ACCOUNTS_FILE), lines, error);
    if (rr == ReadResult::Error) return false;
    int skipped = 0;
    for (const auto& line : lines) {
        if (IsCommentOrEmpty(line)) continue;
        Account a;
        bool hasName = false, hasKey = false;
        for (const auto& [k, v] : SplitFields(line)) {
            long long i = 0;
            double d = 0.0;
            PlayerProgress& p = a.progress;
            if (k == "name") { a.name = v; hasName = net::ValidPlayerName(v); }
            else if (k == "key") hasKey = crypto::FromHex(v, a.key.data(), a.key.size());
            else if (k == "created" && ParseI64(v, i)) a.created = i;
            else if (k == "lastLogin" && ParseI64(v, i)) a.lastLogin = i;
            else if (k == "totalLogin" && ParseDouble(v, d)) a.totalLoginSeconds = ClampSeconds(d);
            else if (k == "balance" && ParseI64(v, i)) p.balance = std::max(0LL, i);
            else if (k == "delivered" && ParseI64(v, i)) p.delivered = std::max(0LL, i);
            else if (k == "cargo") ParseInt(v, p.cargo, 0, 1000000);
            else if (k == "hp" && ParseDouble(v, d)) p.hp = static_cast<float>(std::clamp(d, -1.0, 100000.0));
            else if (k == "shipTime" && ParseDouble(v, d)) p.shipTime = ClampSeconds(d);
            else if (k == "roundTime" && ParseDouble(v, d)) p.roundTime = ClampSeconds(d);
            else if (k == "round") ParseInt(v, p.round, 1, 1000000);
            else if (k == "kills") ParseInt(v, p.kills, 0, 1000000000);
            else if (k == "deaths") ParseInt(v, p.deaths, 0, 1000000000);
            else if (k == "goals") ParseInt(v, p.goals, 0, 1000000000);
            else if (k == "upgrades") {
                // "1,2,0,3" -> vier Stufen, jeweils auf 0..UPGRADE_LEVELS begrenzt
                std::istringstream is(v);
                std::string part;
                for (int u = 0; u < cfg::UP_COUNT && std::getline(is, part, ','); ++u)
                    ParseInt(part, p.upgrades[u], 0, cfg::UPGRADE_LEVELS);
            }
            // unbekannte Felder (z. B. aus einer neueren Version) werden ignoriert
        }
        // Ohne gültigen Namen und Schlüssel ist das Konto unbrauchbar: überspringen
        if (!hasName || !hasKey) { ++skipped; continue; }
        const std::string lower = ToLowerAscii(a.name);
        if (accounts_.count(lower)) { ++skipped; continue; }   // doppelter Name: erster gewinnt
        accounts_.emplace(lower, std::move(a));
    }
    if (skipped > 0) {
        std::cerr << "Warnung: " << skipped << " beschädigte oder doppelte Zeile(n) in " << ACCOUNTS_FILE << " übersprungen\n";
        BackupDamagedFile(PathOf(ACCOUNTS_FILE));
    }
    if (!haveSalt && !accounts_.empty()) {
        // Ohne das alte Salz passen die gespeicherten Schlüssel nicht mehr:
        // die Spieler können sich nicht mehr anmelden. Laut melden!
        std::cerr << "WARNUNG: " << SERVER_FILE << " fehlt oder ist beschädigt, es gibt aber " << accounts_.size()
                  << " Konten. Es wurde ein NEUES Server-Salz erzeugt – bestehende Spieler können sich\n"
                     "         erst wieder anmelden, wenn die alte server.txt (Sicherung) zurückgespielt wird.\n";
    }

    // --- highscores.txt -----------------------------------------------------------
    rr = ReadLines(PathOf(HIGHSCORES_FILE), lines, error);
    if (rr == ReadResult::Error) return false;
    skipped = 0;
    for (const auto& line : lines) {
        if (IsCommentOrEmpty(line)) continue;
        HighscoreRecord h;
        bool okName = false, okSeconds = false;
        for (const auto& [k, v] : SplitFields(line)) {
            long long i = 0;
            double d = 0.0;
            if (k == "name") { h.name = v; okName = net::ValidPlayerName(v); }
            else if (k == "seconds" && ParseDouble(v, d) && d >= 0.0) { h.seconds = ClampSeconds(d); okSeconds = true; }
            else if (k == "round") ParseInt(v, h.round, 1, 65535);
            else if (k == "when" && ParseI64(v, i)) h.when = i;
        }
        if (!okName || !okSeconds) { ++skipped; continue; }
        highscores_.push_back(std::move(h));
    }
    if (skipped > 0) {
        std::cerr << "Warnung: " << skipped << " beschädigte Zeile(n) in " << HIGHSCORES_FILE << " übersprungen\n";
        BackupDamagedFile(PathOf(HIGHSCORES_FILE));
    }
    std::stable_sort(highscores_.begin(), highscores_.end(), HighscoreLess);
    if (highscores_.size() > MAX_HIGHSCORES_STORED) highscores_.resize(MAX_HIGHSCORES_STORED);

    // server.txt immer einmal schreiben: legt beim ersten Start das Salz ab und
    // prüft gleich, ob der Ordner beschreibbar ist (sonst lieber gar nicht starten,
    // als stundenlang zu spielen und dann nichts speichern zu können).
    SaveServerData();
    if (!Flush()) {
        error = "Datenordner ist nicht beschreibbar: " + PathText(dir);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Konten
// ---------------------------------------------------------------------------
Account* Accounts::Find(const std::string& name) {
    auto it = accounts_.find(ToLowerAscii(name));
    return it == accounts_.end() ? nullptr : &it->second;
}

const Account* Accounts::Find(const std::string& name) const {
    auto it = accounts_.find(ToLowerAscii(name));
    return it == accounts_.end() ? nullptr : &it->second;
}

Account* Accounts::Create(const std::string& name, const net::Key32& key, int64_t now) {
    const std::string lower = ToLowerAscii(name);
    if (accounts_.count(lower) || Count() >= MAX_ACCOUNTS) return nullptr;
    Account a;
    a.name = name;
    a.key = key;
    a.created = now;
    a.lastLogin = now;
    // std::map: Zeiger auf Elemente bleiben gültig, auch wenn später weitere
    // Konten eingefügt werden. GameServer darf sich den Zeiger also merken.
    return &accounts_.emplace(lower, std::move(a)).first->second;
}

void Accounts::SaveAccounts() {
    if (dir_.empty()) return;
    std::string out;
    out.reserve(accounts_.size() * 260 + 512);
    out += "# Mondschürfer – Spielerkonten. Bitte nur bei gestopptem Server bearbeiten!\n";
    out += "# Eine Zeile pro Konto. key = Anmeldeschlüssel (aus Passwort + Server-Salz abgeleitet, hex).\n";
    out += "# Das Passwort selbst kennt der Server nicht. Wer diese Datei hat, kann sich aber als\n";
    out += "# jeder Spieler anmelden: Datei geheim halten.\n";
    for (const auto& entry : accounts_) {
        const Account& a = entry.second;
        const PlayerProgress& p = a.progress;
        out += "name=" + a.name;
        out += " key=" + crypto::ToHex(a.key.data(), a.key.size());
        out += " created=" + std::to_string(a.created);
        out += " lastLogin=" + std::to_string(a.lastLogin);
        out += " totalLogin=" + NumText(a.totalLoginSeconds);
        out += " balance=" + std::to_string(p.balance);
        out += " delivered=" + std::to_string(p.delivered);
        out += " upgrades=";
        for (int u = 0; u < cfg::UP_COUNT; ++u) out += (u ? "," : "") + std::to_string(p.upgrades[u]);
        out += " cargo=" + std::to_string(p.cargo);
        out += " hp=" + NumText(p.hp);
        out += " shipTime=" + NumText(p.shipTime);
        out += " roundTime=" + NumText(p.roundTime);
        out += " round=" + std::to_string(p.round);
        out += " kills=" + std::to_string(p.kills);
        out += " deaths=" + std::to_string(p.deaths);
        out += " goals=" + std::to_string(p.goals);
        out += "\n";
    }
    QueueWrite(PathOf(ACCOUNTS_FILE), std::move(out), false, "Konten");
}

// ---------------------------------------------------------------------------
// Bestenliste
// ---------------------------------------------------------------------------
void Accounts::AddHighscore(const HighscoreRecord& r) {
    // Sortiert einfügen: hinter allen Einträgen, die schneller oder gleich
    // schnell waren (upper_bound), damit ältere Gleichstände vorne bleiben.
    auto pos = std::upper_bound(highscores_.begin(), highscores_.end(), r, HighscoreLess);
    highscores_.insert(pos, r);
    if (highscores_.size() > MAX_HIGHSCORES_STORED) highscores_.pop_back();   // langsamsten verwerfen
}

std::vector<HighscoreRecord> Accounts::TopHighscores(size_t n) const {
    const size_t count = std::min(n, highscores_.size());
    return std::vector<HighscoreRecord>(highscores_.begin(), highscores_.begin() + static_cast<long>(count));
}

void Accounts::SaveHighscores() {
    if (dir_.empty()) return;
    std::string out = "# Mondschürfer – Bestenliste (alle erreichten Ziele, kürzeste Zeit zuerst)\n";
    out += "# round > 1: Ziel mit einem Schiff erreicht, das seine Aufrüstungen behalten hat\n";
    for (const auto& h : highscores_) {
        out += "name=" + h.name + " seconds=" + NumText(h.seconds) + " round=" + std::to_string(h.round) +
               " when=" + std::to_string(h.when) + "\n";
    }
    QueueWrite(PathOf(HIGHSCORES_FILE), std::move(out), false, "Bestenliste");
}

// ---------------------------------------------------------------------------
// Serverdaten
// ---------------------------------------------------------------------------
void Accounts::SaveServerData() {
    if (dir_.empty()) return;
    std::string out = "# Mondschürfer – Serverdaten. NICHT löschen: ohne realmSalt passen die\n";
    out += "# gespeicherten Anmeldeschlüssel nicht mehr und niemand kann sich anmelden.\n";
    out += "realmSalt=" + crypto::ToHex(realmSalt_.data(), realmSalt_.size()) + "\n";
    if (hasWorldSeed_) out += "worldSeed=" + std::to_string(worldSeed_) + "\n";
    out += "totalDelivered=" + std::to_string(totalDelivered_) + "\n";
    QueueWrite(PathOf(SERVER_FILE), std::move(out), false, "Serverdaten");
}

// ---------------------------------------------------------------------------
// Einstellungen
// ---------------------------------------------------------------------------
bool Accounts::LoadSettings(GameSettings& inOut) const {
    if (dir_.empty()) return false;
    std::vector<std::string> lines;
    std::string error;
    const ReadResult rr = ReadLines(PathOf(SETTINGS_FILE), lines, error);
    if (rr == ReadResult::Missing) return false;
    if (rr == ReadResult::Error) {
        std::cerr << "Warnung: " << error << " – verwende die Standardwerte\n";
        return false;
    }
    for (const auto& line : lines) {
        if (IsCommentOrEmpty(line)) continue;
        // Hier "schlüssel=rest der Zeile", weil der Servername Leerzeichen enthalten darf
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "serverName") inOut.serverName = value;
        else if (key == "difficulty") ParseDifficulty(value, inOut.difficulty);
        else if (key == "enemyShips") ParseInt(value, inOut.enemyShips, 0, limits::MAX_ENEMY_SHIPS);
        else if (key == "tradeStations") ParseInt(value, inOut.tradeStations, 1, limits::MAX_TRADE_STATIONS);
        else if (key == "shipyards") ParseInt(value, inOut.shipyards, 1, limits::MAX_SHIPYARDS);
        else if (key == "moon") ParseMoonColor(value, inOut.moon);
        else if (key == "maxPlayers") ParseInt(value, inOut.maxPlayers, 1, limits::MAX_PLAYERS_LIMIT);
    }
    Sanitize(inOut);
    return true;
}

void Accounts::SaveSettings(const GameSettings& s) {
    if (dir_.empty()) return;
    std::string out = "# Mondschürfer – auf der Verwaltungsseite geänderte Einstellungen.\n";
    out += "# Sie haben Vorrang vor den Werten aus docker-compose.yml (Umgebungsvariablen).\n";
    out += "# Datei löschen oder auf der Webseite \"Standardwerte\" wählen = wieder die Werte aus docker-compose.yml.\n";
    out += "serverName=" + s.serverName + "\n";
    out += std::string("difficulty=") + DifficultyKey(s.difficulty) + "\n";
    out += "enemyShips=" + std::to_string(s.enemyShips) + "\n";
    out += "tradeStations=" + std::to_string(s.tradeStations) + "\n";
    out += "shipyards=" + std::to_string(s.shipyards) + "\n";
    out += std::string("moon=") + MoonColorKey(s.moon) + "\n";
    out += "maxPlayers=" + std::to_string(s.maxPlayers) + "\n";
    QueueWrite(PathOf(SETTINGS_FILE), std::move(out), false, "Einstellungen");
}

void Accounts::RemoveSettings() {
    if (dir_.empty()) return;
    // Auch das Löschen geht über den Schreib-Thread: Läge noch ein
    // Schreibauftrag für settings.txt in der Warteschlange, würde er die
    // Datei sonst nach dem Löschen wieder anlegen.
    QueueWrite(PathOf(SETTINGS_FILE), std::string(), true, "Einstellungen");
}
