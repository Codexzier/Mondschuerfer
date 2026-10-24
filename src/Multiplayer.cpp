// ---------------------------------------------------------------------------
// Multiplayer.cpp: Menüs und Anzeige des Mehrspielers (siehe Multiplayer.hpp).
//
// Aufbau dieser Datei:
//   1. kleine Hilfsfunktionen (Textfilter, Zeitformat, Zeichenhilfen)
//   2. Lebenszyklus und Zustandswechsel (Address -> Lobby -> Login/Spectating
//      -> Playing), Auswertung der NetClient-Zustände und -Ereignisse
//   3. Update: Eingaben, Autopilot, Kameras, Feuerwerk
//   4. Ton
//   5. Zeichnen der einzelnen Phasen
//
// Woher kommen die Daten?
//   - Serverinfo (Name, Spieler online, Einstellungen): NetClient::Info(),
//     der Server schickt sie in der Lobby alle 2 Sekunden neu.
//   - Spielwelt (Schiffe, Lagerstätten ...): NetClient::GetWorld(), eine
//     "Spiegelwelt", die NetClient aus den Snapshots des Servers füllt.
//     players[0]/ships[0] sind dort der eigene Spieler und sein Schiff.
//   - Spielerliste und Bestenliste: NetClient::Players()/Highscores(), vom
//     Server jede Sekunde bzw. bei Änderung geschickt.
//
// Das Zeichnen folgt dem "immediate mode" von Ui.hpp: Knöpfe werden beim
// Zeichnen abgefragt, deshalb werden Klicks in den Draw...-Funktionen
// ausgewertet (wie im Hauptmenü in main.cpp). Alles, was auch ohne Zeichnen
// laufen muss (Netzwerk, automatische Testabläufe, Kamera), steht in Update.
//
// Alles läuft im Spiel-Thread.
// ---------------------------------------------------------------------------
#include "Multiplayer.hpp"
#include "Assets.hpp"
#include "Audio.hpp"
#include "ClipPlayer.hpp"
#include "Hud.hpp"
#include "TiltShift.hpp"
#include "raylib.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

using namespace ui;

namespace {

// Dauer der Abschlussanimation "Ziel erreicht" in Sekunden
constexpr float kGoalAnimTime = 6.0f;
// Zuschauer: dieses Kartenstück (Spielfeld 300 x 300 plus Rand) passt bei
// Zoom 1 genau in die freie Fläche zwischen den Seitenleisten.
constexpr float kSpectatorSpan = 2.0f * cfg::WORLD_HALF + 36.0f;
// Abstand der Zuschauerkamera über dem Boden (orthografisch: nur für die
// Tiefenwerte wichtig, nicht für die Größe auf dem Bildschirm)
constexpr float kSpectatorHeight = 400.0f;

const Color kYardColor{120, 210, 255, 255};   // Werft (wie im HUD)
const Color kTraderColor{120, 200, 255, 255}; // Händler/Miner (blau)

// --- Zeichenfilter für die Textfelder -----------------------------------------
// Spielername: genau die Zeichen, die net::ValidPlayerName erlaubt. So kann man
// gar nicht erst einen ungültigen Namen tippen.
bool NameChar(int cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '_' || cp == '-';
}
// Serveradresse: sichtbare ASCII-Zeichen ohne Leerzeichen (Hostname, IPv4, Port)
bool AddressChar(int cp) { return cp > 32 && cp < 127; }

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// Zeit immer als h:mm:ss (Bestenliste: alle Einträge gleich breit und vergleichbar)
std::string Hms(float seconds) {
    int s = static_cast<int>(std::max(0.0f, seconds));
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    return buf;
}

// Fortschritt eines Spielers zum Ziel (Kontostand / 100.000), 0..1
float Progress(long long balance) {
    return std::clamp(static_cast<float>(balance) / static_cast<float>(cfg::GOAL_POINTS), 0.0f, 1.0f);
}

std::string Percent(long long balance) { return std::to_string(static_cast<int>(Progress(balance) * 100.0f)) + " %"; }

// Text kürzen ("Sehr langer Na..."), damit er in maxW Pixel passt
std::string Fit(const std::string& t, float size, float maxW) {
    if (Width(t, size) <= maxW) return t;
    std::string s = t;
    while (!s.empty() && Width(s + "...", size) > maxW) {
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // UTF-8-Folgebytes
        if (!s.empty()) s.pop_back();
    }
    return s + "...";
}

// Zeilenumbruch an Leerzeichen, damit ein Text in maxW Pixel passt
std::vector<std::string> Wrap(const std::string& text, float size, float maxW) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && Width(candidate, size) > maxW) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') flushWord();
        else word.push_back(c);
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Mehrzeiligen Text zeichnen; liefert die Höhe, die er belegt hat
float TextBlock(const std::string& text, float x, float y, float size, float maxW, Color c, float lineGap = 1.3f) {
    float yy = y;
    for (const std::string& l : Wrap(text, size, maxW)) {
        Text(l, x, yy, size, c);
        yy += size * lineGap;
    }
    return yy - y;
}

// "Schlüssel ........ Wert" in einer Zeile
void InfoRow(const std::string& key, const std::string& value, float x, float y, float w, float size, Color vc = TEXT) {
    Text(key, x, y, size, MUTED);
    TextRight(Fit(value, size, w * 0.58f), x + w, y, size, vc);
}

std::string EnemiesText(int n) {
    if (n <= 0) return "keine Feinde";
    if (n == 1) return "1 Feind";
    return std::to_string(n) + " Feinde";
}

// Zufallszahl über raylib (nicht über world.rng: der Zufall der Spiegelwelt
// soll von der Anzeige unberührt bleiben)
float RandF(float a, float b) { return a + (b - a) * static_cast<float>(GetRandomValue(0, 10000)) / 10000.0f; }

// Kleines Dreieck in Flugrichtung (Zuschauerkarte). Die Karte zeigt die Welt
// von oben mit x nach rechts und z nach unten – wie die Minikarte im HUD.
void ShipArrow(Vector2 c, float yaw, float size, Color col) {
    Vector3 f = ForwardFromYaw(yaw);
    Vector2 fd{f.x, f.z};
    Vector2 sd{-fd.y, fd.x};
    Vector2 tip = Vector2Add(c, Vector2Scale(fd, size));
    Vector2 a = Vector2Add(c, Vector2Add(Vector2Scale(fd, -size * 0.65f), Vector2Scale(sd, size * 0.7f)));
    Vector2 b = Vector2Add(c, Vector2Add(Vector2Scale(fd, -size * 0.65f), Vector2Scale(sd, -size * 0.7f)));
    // raylib füllt Dreiecke nur bei einer Umlaufrichtung; beide zeichnen ist einfacher als prüfen
    DrawTriangle(tip, a, b, col);
    DrawTriangle(tip, b, a, col);
    DrawTriangleLines(tip, a, b, Color{0, 0, 0, 150});
}

// Bildschirmaufteilung des Zuschauermodus (in Update UND Draw gebraucht,
// deshalb an einer Stelle berechnet)
Rectangle SpectatorLeftPanel() {
    const float s = Scale();
    return {16 * s, 16 * s, 340 * s, static_cast<float>(GetScreenHeight()) - 32 * s};
}
Rectangle SpectatorRightPanel() {
    const float s = Scale();
    return {static_cast<float>(GetScreenWidth()) - 16 * s - 370 * s, 16 * s, 370 * s, static_cast<float>(GetScreenHeight()) - 32 * s};
}

// Günstigstes bezahlbares Upgrade für den Autopiloten (gleiche Reihenfolge wie
// CheapestUpgrade in shared/Ai.cpp: bei gleichem Preis Lager, Waffen, Panzerung, Antrieb)
int CheapestUpgrade(const World& w, long long budget) {
    const int order[4] = {cfg::UP_CARGO, cfg::UP_WEAPON, cfg::UP_ARMOR, cfg::UP_SPEED};
    int best = -1, bestCost = 1 << 30;
    for (int t : order) {
        int c = w.UpgradeCost(w.Me(), t);
        if (c >= 0 && c <= budget && c < bestCost) { best = t; bestCost = c; }
    }
    return best;
}

// Spielerliste nach Fortschritt sortiert (Runde, dann Kontostand). Der Server
// schickt sie schon so sortiert; hier zur Sicherheit noch einmal, damit die
// Anzeige nicht von dieser Annahme abhängt.
std::vector<net::PlayerEntry> SortedPlayers(const std::vector<net::PlayerEntry>& in) {
    std::vector<net::PlayerEntry> v = in;
    std::stable_sort(v.begin(), v.end(), [](const net::PlayerEntry& a, const net::PlayerEntry& b) {
        if (a.round != b.round) return a.round > b.round;
        return a.balance > b.balance;
    });
    return v;
}

} // namespace

// ===========================================================================
// Lebenszyklus und Zustandswechsel
// ===========================================================================
MultiplayerScreen::MultiplayerScreen(const MultiplayerOptions& opt) : opt_(opt) {
    // g_clientConfig hat main() schon beim Start geladen (fehlt die Datei:
    // Standardwerte, also localhost:7777).
    addressField_.text = g_clientConfig.server;
    addressField_.placeholder = "z. B. localhost:7777";
    addressField_.maxChars = 120;
    addressField_.accept = AddressChar;
    addressField_.focused = true;

    if (!opt_.name.empty()) nameField_.text = opt_.name;   // Testoption --mp-name: vorbelegen
    nameField_.placeholder = "Spielername";
    nameField_.maxChars = net::MAX_NAME;
    nameField_.accept = NameChar;

    passwordField_.placeholder = "mindestens 6 Zeichen";
    passwordField_.maxChars = 64;
    passwordField_.password = true;

    // Hintergrund der Menüs: nur Mondoberfläche und Stationen, ohne Schiffe.
    // BuildMirror ist dafür ideal, weil es genau das (und nicht mehr) erzeugt.
    backdrop_.BuildMirror(static_cast<unsigned int>(std::time(nullptr)) ^ 0x4d50u, GameSettings{});
}

MultiplayerScreen::~MultiplayerScreen() {
    // Abmelden (der Server speichert dann sofort den Fortschritt) und trennen
    if (net_.State() != NetState::Disconnected) net_.Disconnect();
    // Passwort nicht länger als nötig im Speicher behalten
    std::fill(passwordField_.text.begin(), passwordField_.text.end(), '\0');
    passwordField_.text.clear();
    g_assets.SetMoonColor(MoonColor::Grey);   // Einzelspieler und Hauptmenü: grauer Mond
}

// Phasenwechsel an einer Stelle: phaseFrames_ zählt die Frames seit dem
// Wechsel. Tastenkürzel (ESC, Enter) werten die Draw-Funktionen erst ab dem
// zweiten Frame aus – sonst würde z. B. dasselbe ESC, das den Zuschauermodus
// beendet, im neuen Bildschirm gleich noch "Abbrechen" auslösen (raylib
// meldet einen Tastendruck während des ganzen Frames).
void MultiplayerScreen::SetPhase(Phase p) {
    phase_ = p;
    phaseFrames_ = 0;
}

void MultiplayerScreen::SetStatus(const std::string& text, Color c) {
    status_ = text;
    statusColor_ = c;
}

// Verbindung zur Adresse im Textfeld aufbauen. Ob es klappt, meldet NetClient
// später über State() (Lobby) bzw. TakeError() – siehe HandleNetwork.
void MultiplayerScreen::StartConnect() {
    const std::string addr = Trim(addressField_.text);
    if (addr.empty()) {
        SetStatus("Bitte eine Serveradresse eingeben.", BAD);
        g_audio.Play(Sfx::Denied);
        return;
    }
    // Adresse gleich merken – auch wenn der Server gerade nicht läuft, will
    // man sie beim nächsten Mal nicht wieder eintippen.
    g_clientConfig.server = addr;
    g_clientConfig.Save();

    if (net_.State() != NetState::Disconnected) net_.Disconnect();
    if (!net_.Connect(addr)) {
        std::string err = net_.TakeError();
        SetStatus(err.empty() ? "Ungültige Serveradresse." : err, BAD);
        g_audio.Play(Sfx::Denied);
        return;
    }
    connecting_ = true;
    addressField_.focused = false;
    SetStatus("", MUTED);
}

// Zurück zur Adresseingabe (Trennen, Verbindungsverlust, Fehler)
void MultiplayerScreen::BackToAddress(const std::string& reason) {
    if (net_.State() != NetState::Disconnected) net_.Disconnect();
    SetPhase(Phase::Address);
    connecting_ = false;
    authPending_ = false;
    spectatePending_ = false;
    paused_ = false;
    optionsOpen_ = false;
    goalAnimT_ = -1.0f;
    controls_.Reset();
    addressField_.focused = true;
    SetStatus(reason, BAD);
    g_assets.SetMoonColor(MoonColor::Grey);
}

void MultiplayerScreen::EnterLobby() {
    SetPhase(Phase::Lobby);
    connecting_ = false;
    lobbySel_ = 1;   // "Mitspielen" vorauswählen
    SetStatus("", MUTED);
    g_audio.Play(Sfx::UiConfirm);
}

void MultiplayerScreen::EnterLogin() {
    SetPhase(Phase::Login);
    if (nameField_.text.empty()) nameField_.text = g_clientConfig.name;   // zuletzt benutzter Name
    // Fokus dorthin, wo noch etwas fehlt
    nameField_.focused = nameField_.text.empty();
    passwordField_.focused = !nameField_.focused;
    SetStatus("", MUTED);
}

void MultiplayerScreen::EnterPlaying() {
    SetPhase(Phase::Playing);
    authPending_ = false;
    paused_ = false;
    optionsOpen_ = false;
    dockFrames_ = 0;
    undockSentT_ = -1.0f;
    snapCam_ = true;
    hadShip_ = false;
    goalAnimT_ = -1.0f;
    zoom_ = 1.1f;
    padZoom_ = 0.0f;
    controls_.Reset();
    autoDockT_ = 0.0f;
    chat_.Clear();
    chatTypingSent_ = false;
    ResetAutopilot();   // Autopilot-Zustand (Ai.cpp) gehört jetzt der Spiegelwelt
    SetStatus("", MUTED);

    // Erfolgreich angemeldet: Namen für das nächste Mal speichern (nie das Passwort!)
    // und das Passwort aus dem Speicher löschen – es wird nicht mehr gebraucht.
    g_clientConfig.name = net_.MyName().empty() ? Trim(nameField_.text) : net_.MyName();
    g_clientConfig.Save();
    std::fill(passwordField_.text.begin(), passwordField_.text.end(), '\0');
    passwordField_.text.clear();
}

void MultiplayerScreen::EnterSpectating() {
    SetPhase(Phase::Spectating);
    spectatePending_ = false;
    specZoom_ = 1.0f;
    specCenter_ = {0.0f, 0.0f};
    listScroll_ = 0;
    dragging_ = false;
    // "Wie gedrückt" beginnen: Hält der Spieler die Taste vom Klick auf
    // "Zuschauen" noch fest, soll das nicht gleich als Ziehen zählen. Erst ein
    // neuer Druck verschiebt die Karte.
    specPointerWasDown_ = true;
    SetStatus("", MUTED);
}

// Anmelden oder Konto anlegen. Name und Passwort werden zuerst hier geprüft,
// damit offensichtliche Fehler gar nicht erst zum Server gehen.
void MultiplayerScreen::SubmitLogin(bool registerAccount) {
    if (authPending_ || net_.State() != NetState::Lobby) return;
    const std::string name = Trim(nameField_.text);
    if (!net::ValidPlayerName(name)) {
        SetStatus(net::ErrorText(net::ErrorCode::NameInvalid), BAD);
        g_audio.Play(Sfx::Denied);
        nameField_.focused = true;
        passwordField_.focused = false;
        return;
    }
    if (Utf8Length(passwordField_.text) < net::MIN_PASSWORD) {
        SetStatus(Fmt("Das Passwort muss mindestens %d Zeichen lang sein.", net::MIN_PASSWORD), BAD);
        g_audio.Play(Sfx::Denied);
        passwordField_.focused = true;
        nameField_.focused = false;
        return;
    }
    // NetClient berechnet aus dem Passwort den Anmeldeschlüssel (PBKDF2, siehe
    // shared/Crypto.hpp). Das dauert ein paar Millisekunden; das Passwort
    // selbst verlässt den Rechner nie.
    if (registerAccount) net_.Register(name, passwordField_.text);
    else net_.Login(name, passwordField_.text);
    authPending_ = true;
    authWasRegister_ = registerAccount;
    authStartT_ = elapsed_;
    SetStatus(registerAccount ? "Konto wird angelegt ..." : "Anmeldung läuft ...", MUTED);
    g_audio.Play(Sfx::UiConfirm);
}

// Wertet aus, was NetClient seit dem letzten Frame erlebt hat: Fehler,
// Zustandswechsel (verbunden, angemeldet, getrennt) und Ereignisse.
void MultiplayerScreen::HandleNetwork() {
    const NetState st = net_.State();
    const std::string err = net_.TakeError();   // wird beim Abholen geleert

    switch (phase_) {
        case Phase::Address:
            if (connecting_ && net_.HasInfo() &&
                (st == NetState::Lobby || st == NetState::LoggingIn || st == NetState::Playing || st == NetState::Spectating)) {
                EnterLobby();
            } else if (connecting_ && st == NetState::Disconnected) {
                connecting_ = false;
                addressField_.focused = true;   // Adresse gleich korrigieren können
                SetStatus(err.empty() ? "Verbindung fehlgeschlagen." : err, BAD);
                g_audio.Play(Sfx::Denied);
            } else if (!err.empty()) {
                SetStatus(err, BAD);
            }
            break;

        case Phase::Lobby:
        case Phase::Login:
            if (st == NetState::Disconnected) {
                BackToAddress(err.empty() ? "Verbindung zum Server verloren." : err);
                g_audio.Play(Sfx::Denied);
                return;
            }
            if (st == NetState::Playing) { EnterPlaying(); break; }
            if (st == NetState::Spectating) { EnterSpectating(); break; }
            if (!err.empty()) {
                // Antwort des Servers auf Anmeldung/Zuschauen: z. B. "Name vergeben",
                // "Name oder Passwort falsch", "Keine Zuschauerplätze mehr frei".
                // NetClient ist dann wieder in der Lobby.
                const bool nameTaken = err.find(net::ErrorText(net::ErrorCode::NameTaken)) != std::string::npos;
                const bool retryAsLogin = autoRegisterFallback_ && authWasRegister_ && nameTaken;
                authPending_ = false;
                spectatePending_ = false;
                SetStatus(err, BAD);
                if (phase_ == Phase::Login) {   // gleich korrigieren können
                    nameField_.focused = nameTaken;
                    passwordField_.focused = !nameTaken;
                }
                if (retryAsLogin) {
                    // Testablauf --mp-register: Konto gibt es schon -> einfach anmelden
                    autoRegisterFallback_ = false;
                    retryLogin_ = true;
                } else {
                    g_audio.Play(Sfx::Denied);
                }
            }
            // Sicherheitsnetz: Hängt eine Anfrage ohne Antwort (sollte nicht
            // vorkommen), werden die Knöpfe nach einer Weile wieder freigegeben.
            if (authPending_ && st == NetState::Lobby && elapsed_ - authStartT_ > 8.0f) authPending_ = false;
            if (spectatePending_ && elapsed_ - spectateStartT_ > 8.0f) spectatePending_ = false;
            if (retryLogin_ && st == NetState::Lobby && !authPending_) {
                retryLogin_ = false;
                SubmitLogin(false);
            }
            break;

        case Phase::Playing:
        case Phase::Spectating:
            if (st == NetState::Disconnected) {
                BackToAddress(err.empty() ? "Verbindung zum Server verloren." : err);
                g_audio.Play(Sfx::Denied);
                return;
            }
            // Fehler während des Spiels (z. B. abgelehnte Aktion): als Meldung zeigen
            if (!err.empty() && net_.WorldReady()) net_.GetWorld().Notify(err, BAD);
            break;
    }

    // (Neue) Welt vom Server: Mondfarbe des Servers übernehmen. Kommt nach dem
    // ersten Welcome und nach jedem Neuaufbau (Einstellungen auf der Webseite
    // geändert) – dann kann das eigene Schiff an einer anderen Station stehen.
    if (net_.TakeWorldRebuilt()) {
        g_assets.SetMoonColor(net_.Welcome().settings.moon);
        snapCam_ = true;
        dockFrames_ = 0;
        undockSentT_ = -1.0f;
    }
    if (net_.TakeRespawned()) {   // neues Schiff: Kamera springt hin, altes Mausziel gilt nicht mehr
        snapCam_ = true;
        controls_.Reset();
    }

    // Ziel erreicht: eigenes -> große Abschlussanimation; fremdes -> NetClient
    // zeigt schon eine Meldung, wir zünden zusätzlich ein kleines Feuerwerk
    // über dem Schiff dieses Spielers (falls es gerade sichtbar ist).
    for (const NetClient::GoalNotice& g : net_.TakeGoals()) {
        if (g.mine && phase_ == Phase::Playing) {
            goalAnimT_ = 0.0f;
            goalSeconds_ = g.seconds;
            goalRound_ = g.round;
            burstT_ = 0.0f;
            g_audio.Play(Sfx::UiConfirm);   // die Sieges-Fanfare (Victory) schickt der Server als Ton-Ereignis
        } else if (net_.WorldReady()) {
            World& w = net_.GetWorld();
            for (const auto& p : net_.Players()) {
                if (p.name != g.name) continue;
                for (const Ship& s : w.ships) {
                    if (s.dead || s.hidden || &s == &w.Player()) continue;
                    if (s.playerId == p.id || (p.shipId >= 0 && s.id == p.shipId)) {
                        for (int i = 0; i < 3; ++i) FireworkBurst(s.pos, 6.0f, 1);
                    }
                }
            }
        }
    }

    // Chat: neue Zeilen ins Fenster (auch die eigenen kommen vom Server zurück)
    for (const net::ChatLineMsg& l : net_.TakeChat()) chat_.Add(l.name, l.text, l.playerId == net_.MyPlayerId());
}

// ===========================================================================
// Update
// ===========================================================================
void MultiplayerScreen::Update(float frameDt) {
    // frameDt ist die echte Bildzeit. Ungekürzt braucht sie nur die
    // Eingabeuhr im NetClient (Schritt 2). Kamera, Animationen und Zähler hier
    // rechnen wie im Einzelspieler mit höchstens 1/20 s pro Frame.
    const float dt = std::min(frameDt, 1.0f / 20.0f);
    elapsed_ += dt;
    ++phaseFrames_;

    // Testoption --mp-connect: im ersten Frame sofort verbinden
    if (!opt_.connect.empty() && !autoConnectDone_) {
        autoConnectDone_ = true;
        addressField_.text = opt_.connect;
        StartConnect();
    }

    // --- 0. Chat: Enter öffnet die Eingabe, ESC schließt sie ---------------------
    // Vor der Flugsteuerung, die bei offener Eingabe ruht. Ob die Eingabe offen
    // ist, erfährt der Server (Sprechblase am Schiff für die anderen).
    const bool chatAllowed = phase_ == Phase::Playing && net_.WorldReady() && !paused_ && goalAnimT_ < 0.0f;
    chat_.Update(chatAllowed);
    if (chat_.Active() != chatTypingSent_) {
        chatTypingSent_ = chat_.Active();
        net_.SendChatTyping(chatTypingSent_);
    }

    // --- 1. Eingaben fürs eigene Schiff (nur im Spiel) ---------------------------
    ShipInput held;
    bool scan = false, interact = false;
    if (phase_ == Phase::Playing && net_.WorldReady()) ReadGameInput(dt, held, scan, interact);

    // --- 2. Netzwerk: Pakete abarbeiten, Eingaben senden, Welt nachführen --------
    // Muss in JEDER Phase laufen, sonst würden z. B. die Lobby-Infos nicht
    // aktualisiert und die Verbindung liefe in eine Zeitüberschreitung.
    net_.Update(frameDt, held, scan, interact);

    // --- 3. Was ist passiert? -------------------------------------------------------
    HandleNetwork();

    // Testoptionen --mp-name/--mp-pass/--mp-register/--mp-spectate: in der
    // Lobby automatisch zuschauen bzw. anmelden (genau einmal).
    if (phase_ == Phase::Lobby && !autoActionDone_) {
        if (opt_.spectate) {
            autoActionDone_ = true;
            net_.Spectate();
            spectatePending_ = true;
            spectateStartT_ = elapsed_;
        } else if (!opt_.name.empty() && !opt_.password.empty()) {
            autoActionDone_ = true;
            EnterLogin();
            nameField_.text = opt_.name;
            passwordField_.text = opt_.password;
            autoRegisterFallback_ = opt_.registerAccount;
            SubmitLogin(opt_.registerAccount);
        }
    }

    // --- 4. Phase ---------------------------------------------------------------------
    if (phase_ == Phase::Playing && net_.WorldReady()) {
        World& w = net_.GetWorld();
        const Ship& p = w.Player();
        if (!p.dead && !p.hidden) hadShip_ = true;

        // Andocken verfolgen. dockFrames_ zählt die Frames seit dem Andocken:
        // Das Stationsfenster wird erst ab dem 2. Frame bedienbar, damit ein
        // F-Druck nicht im selben Moment andockt UND wieder abdockt (wie main.cpp).
        if (w.Me().dockedAt >= 0) {
            ++dockFrames_;
        } else {
            dockFrames_ = 0;
            undockSentT_ = -1.0f;
        }
        // Abdocken angefragt, aber der Server hat (noch) nicht reagiert: nach
        // 1,5 s das Stationsfenster wieder zeigen, damit man nicht festsitzt.
        if (undockSentT_ >= 0.0f) {
            undockSentT_ += dt;
            if (undockSentT_ > 1.5f) undockSentT_ = -1.0f;
        }

        // Pause (ESC/P, Controller: Menü-Taste). Nur im freien Flug; angedockt
        // schließt ESC das Stationsfenster. Während der Zielanimation beendet
        // ESC (Controller: B) die Animation, nach 1 s auch Enter, Klick oder A.
        // (Die Animation läuft immer angedockt ab, also im Menümodus von Ui.hpp.)
        if (goalAnimT_ >= 0.0f) {
            if (BackPressed() || (goalAnimT_ > 1.0f && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || PointerPressed() ||
                                                        PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)))) {
                goalAnimT_ = -1.0f;
                dockFrames_ = 0;   // dieselbe Taste darf nicht auch das Stationsfenster bedienen
            }
        } else if (!paused_ && w.Me().dockedAt < 0 && !chat_.Busy() && PilotControls::PausePressed()) {
            paused_ = true;
            pauseSel_ = 0;
            pauseFrames_ = 0;
            g_audio.Play(Sfx::PauseOpen);
        }
        if (paused_) ++pauseFrames_;

        UpdateAutopilotDock(dt);
        UpdateFireworks(dt);
        UpdatePlayingCamera(dt);
    } else if (phase_ == Phase::Spectating) {
        if (net_.WorldReady()) net_.GetWorld().drawRange = 2000.0f;   // ganze Karte zeichnen (nach jedem Neuaufbau wieder setzen)
        UpdateSpectatorCamera(dt);
        if (BackPressed() && phaseFrames_ >= 1) {
            // Zuschauen beenden (ESC, Controller: B): Das Protokoll kennt kein "zurück in die Lobby",
            // deshalb trennen und sofort neu verbinden -> Lobby.
            g_audio.Play(Sfx::UiBack);
            net_.Disconnect();
            g_assets.SetMoonColor(MoonColor::Grey);
            SetPhase(Phase::Address);
            addressField_.text = g_clientConfig.server;
            StartConnect();
        }
    } else {
        backdropOrbit_ += dt * 0.06f;
        // Lobby und Anmeldung: Der Hintergrund zeigt den Mond schon in der Farbe
        // des Servers (Einstellung von dessen Webseite, kommt mit ServerInfo).
        if ((phase_ == Phase::Lobby || phase_ == Phase::Login) && net_.HasInfo() &&
            g_assets.GetMoonColor() != net_.Info().settings.moon)
            g_assets.SetMoonColor(net_.Info().settings.moon);
    }
}

// Eingaben fürs eigene Schiff: Tastatur, Maus und Controller über
// PilotControls (Controls.hpp), wie im Einzelspieler. Scanner (E/B) und
// Interaktion (F/Y) kommen getrennt, weil NetClient sie als
// Tastendruck-Zähler an den Server schickt.
void MultiplayerScreen::ReadGameInput(float dt, ShipInput& held, bool& scan, bool& interact) {
    World& w = net_.GetWorld();
    // Angedockt: Ein Mausziel ist erledigt. Angedockt oder Pause: keine
    // Steuerung (wie im Einzelspieler, wo World::Update während des
    // Andockens und der Pause gar nicht läuft). In der Pause bleibt ein
    // Mausziel erhalten; danach geht der Flug dorthin weiter. Ebenso, solange
    // im Chat getippt wird (die Tasten gehören dann dem Text).
    if (w.Me().dockedAt >= 0) controls_.Reset();
    if (paused_ || w.Me().dockedAt >= 0 || chat_.Busy()) return;
    if (opt_.autopilot) {
        // Autopilot (Testoption): dieselbe Steuerlogik wie die Simulation im
        // Einzelspieler, angewendet auf die Spiegelwelt. Er "drückt" nur Tasten;
        // was passiert, entscheidet wie bei einem Menschen der Server.
        ShipInput in = AutopilotInput(w, dt);
        held.thrust = in.thrust;
        held.turn = in.turn;
        held.fire = in.fire;
        scan = in.scanToggle;
        interact = in.interact;
        return;
    }
    const FlightInput fi = controls_.Read(w.Player(), cam_, dt);
    held = fi.held;
    scan = fi.scan;
    interact = fi.interact;
    padZoom_ += fi.zoom;   // UpdatePlayingCamera rechnet es wie Mausrad-Rasten ein
}

// Autopilot an der Station: Reparatur bzw. Upgrades als Anfrage an den Server
// schicken (NICHT AutopilotDockActions aus Ai.cpp – das würde die Spiegelwelt
// direkt verändern, der Server wüsste davon nichts), nach ~1,5 s abdocken.
void MultiplayerScreen::UpdateAutopilotDock(float dt) {
    if (!opt_.autopilot) return;
    World& w = net_.GetWorld();
    const PlayerState& me = w.Me();
    if (me.dockedAt < 0 || me.dockedAt >= static_cast<int>(w.stations.size())) {
        autoDockT_ = 0.0f;
        autoDockNextAction_ = 0.2f;
        return;
    }
    autoDockT_ += dt;
    if (undockSentT_ >= 0.0f) return;
    // Alle 0,4 s eine Aktion: Bis dahin hat ein Snapshot den neuen Kontostand
    // gebracht, so wird nichts doppelt gekauft.
    if (autoDockT_ >= autoDockNextAction_ && autoDockT_ < 1.5f) {
        autoDockNextAction_ += 0.4f;
        if (w.stations[me.dockedAt].type == StationType::Trade) {
            if (w.RepairCost(me) > 0 && me.balance >= cfg::REPAIR_COST_PER_POINT) net_.SendDockAction(net::DockActionType::Repair);
        } else if (w.stations[me.dockedAt].type == StationType::Shipyard) {
            int t = CheapestUpgrade(w, me.balance - 300);
            if (t >= 0) net_.SendDockAction(net::DockActionType::BuyUpgrade, t);
        }
    }
    if (autoDockT_ >= 1.5f) {
        net_.SendDockAction(net::DockActionType::Undock);
        undockSentT_ = 0.0f;
    }
}

// Kamera wie im Einzelspieler (App::UpdateCamera in main.cpp): schräg von
// oben hinter dem eigenen Schiff, weich nachgeführt, Mausrad zoomt.
void MultiplayerScreen::UpdatePlayingCamera(float dt) {
    const World& w = net_.GetWorld();
    const Ship& p = w.Player();
    const bool hasShip = !p.dead && !p.hidden;

    // Mausrad und rechter Stick (padZoom_, in Mausrad-Rasten; ReadGameInput).
    // Über dem Chatfenster blättert das Mausrad bei offener Eingabe im Verlauf.
    float wheel = (chat_.WantsWheel() ? 0.0f : GetMouseWheelMove()) + padZoom_;
    padZoom_ = 0.0f;
    if (wheel != 0.0f && !paused_) zoom_ = Clamp(zoom_ - wheel * 0.08f, 0.55f, 1.6f);

    Vector3 target = hasShip ? Vector3{p.pos.x, 0.0f, p.pos.z} : camTarget_;
    if (snapCam_ && hasShip) {
        camTarget_ = target;   // erstes Schiff / neues Schiff: direkt hin statt quer über die Karte schwenken
        snapCam_ = false;
    }
    float k = 1.0f - expf(-5.0f * dt);
    camTarget_ = Vector3Lerp(camTarget_, target, k);

    cam_.up = {0, 1, 0};
    cam_.fovy = 45.0f;
    cam_.projection = CAMERA_PERSPECTIVE;
    Vector3 off = Vector3Scale({0.0f, 30.0f, 27.0f}, zoom_);
    Vector3 jitter{0, 0, 0};
    if (w.shake > 0.0f) {   // Treffer: Bild wackelt (NetClient erhöht shake)
        float a = w.shake * 0.5f;
        jitter = {sinf(elapsed_ * 71.0f) * a, 0, cosf(elapsed_ * 53.0f) * a};
    }
    cam_.target = Vector3Add(camTarget_, jitter);
    cam_.position = Vector3Add(Vector3Add(camTarget_, off), jitter);
}

// Freie Fläche zwischen den beiden Seitenleisten des Zuschauermodus
Rectangle MultiplayerScreen::SpectatorMapArea() const {
    const float s = Scale();
    const Rectangle l = SpectatorLeftPanel(), r = SpectatorRightPanel();
    const float top = 58 * s, bottom = static_cast<float>(GetScreenHeight()) - 66 * s;
    return {l.x + l.width + 12 * s, top, r.x - 12 * s - (l.x + l.width + 12 * s), bottom - top};
}

// Zuschauerkamera: senkrecht von oben, orthografisch (keine Perspektive –
// die Karte sieht aus wie ein Plan). Die Mitte der Kartenfläche zeigt den
// Weltpunkt specCenter_; specZoom_ = 1 zeigt das ganze Spielfeld.
void MultiplayerScreen::UpdateSpectatorCamera(float dt) {
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const Rectangle area = SpectatorMapArea();
    const float areaCx = area.x + area.width * 0.5f, areaCy = area.y + area.height * 0.5f;
    const float mapPx = std::max(50.0f, std::min(area.width, area.height));
    float upp = kSpectatorSpan / mapPx * specZoom_;   // Welt-Einheiten pro Bildschirmpixel

    // Zeiger = Maus oder Controller-Zeiger (Ui.hpp)
    const Vector2 mouse = PointerPos();
    const bool overList = CheckCollisionPointRec(mouse, SpectatorLeftPanel());
    const bool overRight = CheckCollisionPointRec(mouse, SpectatorRightPanel());
    const float wheel = GetMouseWheelMove();
    // Rechter Stick: stufenlos zoomen, voll ausgelenkt wie 6 Mausrad-Rasten
    // pro Sekunde (nach oben = näher). Er blättert NICHT in der Spielerliste –
    // dort würde ein gehaltener Stick in jedem Frame eine Zeile weiterspringen.
    float padZoom = 0.0f;
    if (Gamepad() >= 0)
        padZoom = -controls::StickDeadzone({0.0f, GetGamepadAxisMovement(Gamepad(), GAMEPAD_AXIS_RIGHT_Y)}).y * 6.0f * std::min(dt, 0.05f);
    if (wheel != 0.0f && overList) {
        listScroll_ -= wheel > 0 ? 1 : -1;   // Spielerliste blättern (begrenzt beim Zeichnen)
    } else if ((wheel != 0.0f && !overRight) || padZoom != 0.0f) {
        // Zoomen um den Punkt unter dem Zeiger: dieser Kartenpunkt soll nach
        // dem Zoom wieder unter dem Zeiger liegen. Steht der Zeiger auf einer
        // Seitenleiste (nur beim Stick möglich), um die Mitte der Karte.
        const float steps = (overList || overRight) ? padZoom : wheel + padZoom;
        const Vector2 anchor = (overList || overRight) ? Vector2{areaCx, areaCy} : mouse;
        Vector2 before{specCenter_.x + (anchor.x - areaCx) * upp, specCenter_.y + (anchor.y - areaCy) * upp};
        specZoom_ = Clamp(specZoom_ * powf(0.85f, steps), 0.15f, 1.0f);
        upp = kSpectatorSpan / mapPx * specZoom_;
        specCenter_ = {before.x - (anchor.x - areaCx) * upp, before.y - (anchor.y - areaCy) * upp};
    }

    // Verschieben durch Ziehen mit gedrückter linker oder rechter Maustaste
    // (Controller: A halten und den Zeiger mit dem linken Stick bewegen –
    // PointerPressed/PointerDown/PointerDelta aus Ui.hpp gelten für beides)
    // Beginn: Flanke von PointerDown (vorher nicht gedrückt, jetzt gedrückt)
    // statt PointerPressed. Der Unterschied zählt beim Controller: Ist sein
    // Zeiger "geparkt", zählt Knopf A noch nicht als Zeiger-Taste. Drückt der
    // Spieler zuerst A und bewegt dann den Stick, wird der Zeiger erst dabei
    // aktiv – PointerPressed käme nie, PointerDown wechselt aber in diesem
    // Moment auf "gedrückt". So verschiebt "A halten + Stick" die Karte.
    const bool pointerDown = PointerDown();
    if (((pointerDown && !specPointerWasDown_) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) && !overList && !overRight)
        dragging_ = true;
    specPointerWasDown_ = pointerDown;
    if (!pointerDown && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) dragging_ = false;
    if (dragging_) {
        Vector2 d = PointerDelta();
        specCenter_.x -= d.x * upp;
        specCenter_.y -= d.y * upp;
    }
    if (IsKeyPressed(KEY_R)) {   // ganze Karte
        specZoom_ = 1.0f;
        specCenter_ = {0.0f, 0.0f};
        upp = kSpectatorSpan / mapPx;
    }
    const float lim = cfg::WORLD_HALF;
    specCenter_.x = Clamp(specCenter_.x, -lim, lim);
    specCenter_.y = Clamp(specCenter_.y, -lim, lim);

    // Die Kamera schaut auf die Bildschirmmitte; die Kartenfläche liegt aber
    // zwischen den Leisten (nicht unbedingt mittig). Deshalb die Kamera so
    // verschieben, dass specCenter_ genau in der Mitte der Kartenfläche landet.
    const float camX = specCenter_.x + (W * 0.5f - areaCx) * upp;
    const float camZ = specCenter_.y + (H * 0.5f - areaCy) * upp;
    cam_.projection = CAMERA_ORTHOGRAPHIC;
    cam_.fovy = H * upp;             // orthografisch: sichtbare Höhe in Welt-Einheiten
    cam_.up = {0.0f, 0.0f, -1.0f};   // Norden (-z) ist oben, +x rechts – wie die Minikarte
    cam_.target = {camX, 0.0f, camZ};
    cam_.position = {camX, kSpectatorHeight, camZ};
    camTarget_ = cam_.target;
}

// ===========================================================================
// Feuerwerk (Abschlussanimation)
// ===========================================================================
void MultiplayerScreen::UpdateFireworks(float dt) {
    if (goalAnimT_ < 0.0f) return;
    goalAnimT_ += dt;
    if (goalAnimT_ > kGoalAnimTime) {
        goalAnimT_ = -1.0f;
        dockFrames_ = 0;
        return;
    }
    // In der letzten Sekunde keine neuen Raketen mehr, damit es ausklingt
    burstT_ -= dt;
    if (burstT_ <= 0.0f && goalAnimT_ < kGoalAnimTime - 1.0f) {
        burstT_ = RandF(0.16f, 0.36f);
        const World& w = net_.GetWorld();
        Vector3 center = w.Player().dead ? camTarget_ : w.Player().pos;
        FireworkBurst(center, 10.0f, 1);
    }
}

// Eine Feuerwerksrakete: kurze Funkenwolke (World::SpawnSparks) plus länger
// leuchtende, langsam fallende Sterne in einer bunten Farbe. Alles nur in der
// eigenen Spiegelwelt – die anderen Spieler sehen davon nichts.
void MultiplayerScreen::FireworkBurst(Vector3 center, float spread, int strength) {
    static const Color palette[] = {
        {110, 240, 170, 255}, {255, 160, 51, 255}, {95, 175, 255, 255}, {255, 110, 200, 255},
        {255, 240, 120, 255}, {180, 130, 255, 255}, {255, 90, 80, 255},
    };
    const int n = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    World& w = net_.GetWorld();
    Vector3 pos{center.x + RandF(-spread, spread), RandF(5.0f, 10.0f), center.z + RandF(-spread, spread)};
    const Color c = palette[GetRandomValue(0, n - 1)];
    const Color c2 = palette[GetRandomValue(0, n - 1)];
    w.SpawnSparks(pos, c, 30 * strength);
    w.SpawnSparks(pos, c2, 14 * strength);
    for (int i = 0; i < 40 * strength; ++i) {
        Particle pt;
        pt.pos = pos;
        // Zufällige Richtung auf einer Kugel -> runde "Blüte"
        Vector3 dir = Vector3Normalize({RandF(-1, 1), RandF(-1, 1), RandF(-1, 1)});
        pt.vel = Vector3Scale(dir, RandF(5.0f, 8.0f));
        pt.life = pt.maxLife = RandF(0.9f, 1.5f);
        pt.size = RandF(0.22f, 0.34f);
        pt.color = (i % 3 == 0) ? c2 : c;
        pt.glow = true;
        pt.gravity = 2.5f;
        w.particles.push_back(pt);
    }
    g_audio.Play(Sfx::Discover, 0.3f, RandF(0.35f, 0.65f));
}

// ===========================================================================
// Ton
// ===========================================================================
void MultiplayerScreen::UpdateAudio(float dt) {
    LoopState loops;   // alles aus, wenn nicht gespielt wird
    int cue = -1;      // Funkspruch-Clip der Mission (ClipPlayer), -1 = keiner
    bool showClip = false;
    // Immer abholen, auch wenn nichts abgespielt wird – sonst sammeln sie sich an
    std::vector<SoundEvent> sounds = net_.TakeSounds();
    if (phase_ == Phase::Playing && net_.WorldReady()) {
        // Hörposition = Kameraziel (wie im Einzelspieler). forPlayer hat
        // NetClient schon gefiltert: alles hier ist für uns bestimmt.
        for (const SoundEvent& e : sounds) g_audio.PlayAt(e, camTarget_);
        // Dauergeräusche aus dem Zustand des eigenen Schiffs (wie main.cpp::UpdateAudio)
        const World& w = net_.GetWorld();
        const Ship& p = w.Player();
        const PlayerState& me = w.Me();
        // Die Welt läuft im Mehrspieler auch in der Pause weiter, der Clip auch.
        if (me.cueT > 0.0f) cue = me.cue;
        showClip = me.dockedAt < 0;
        if (!paused_ && w.PlayerAlive() && !p.hidden && me.dockedAt < 0) {
            loops.engine = 1.0f;
            loops.enginePitch = 0.9f + 0.25f * Vector3Length(p.vel) / std::max(1.0f, p.maxSpeed);
            loops.thrust = p.thrustVisual;
            loops.scanner = me.scanOn ? 1.0f : 0.0f;
            loops.mining = me.mining ? 1.0f : 0.0f;
            loops.alarm = p.hp / std::max(1.0f, p.maxHp) < 0.3f;
        }
    } else if (phase_ == Phase::Spectating) {
        // Zuschauer "schweben" über der ganzen Karte: keine Abstandsdämpfung,
        // dafür leiser (Zuschauer bekommen ohnehin nur wenige Ereignisse).
        for (SoundEvent e : sounds) {
            e.volume *= 0.5f;
            g_audio.PlayAt(e, e.pos);
        }
    }
    // Musik: Spielmusik beim Spielen (auch in Pause und Optionen) und beim
    // Zuschauen, sonst (Adresse, Lobby, Anmeldung) die Menümusik. Beim
    // Wechsel blendet g_audio.Update weich über.
    const bool inGame = phase_ == Phase::Playing || phase_ == Phase::Spectating;
    g_audio.SetMusicTrack(inGame ? MusicTrack::Game : MusicTrack::Menu);
    g_audio.Update(dt, loops);
    g_clip.Update(dt, cue, showClip);
}

void MultiplayerScreen::OnMuteToggled(bool muted) {
    if (phase_ == Phase::Playing && net_.WorldReady())
        net_.GetWorld().Notify(muted ? "Ton aus  [M]" : "Ton an  [M]", MUTED);
}

std::string MultiplayerScreen::Report() const {
    static const char* names[] = {"address", "lobby", "login", "playing", "spectating"};
    std::string r = Fmt("MPREPORT t=%.0f phase=%s state=%d players=%d highscores=%d total=%lld", elapsed_,
                        names[static_cast<int>(phase_)], static_cast<int>(net_.State()), static_cast<int>(net_.Players().size()),
                        static_cast<int>(net_.Highscores().size()), net_.TotalDelivered());
    if (net_.WorldReady() && phase_ == Phase::Playing) {
        const World& w = net_.GetWorld();
        const PlayerState& me = w.Me();
        const Ship& p = w.Player();
        r += Fmt(" name=%s bal=%lld round=%d shipTime=%.0f upg=%d%d%d%d%d hp=%.0f/%.0f cargo=%d alive=%d docked=%d ships=%d deps=%d ping=%.0f",
                 net_.MyName().c_str(), me.balance, me.round, me.playTime, me.upgrades[0], me.upgrades[1], me.upgrades[2],
                 me.upgrades[3], me.upgrades[4], p.hp, p.maxHp, p.cargo, (!p.dead && !p.hidden) ? 1 : 0, me.dockedAt >= 0 ? 1 : 0,
                 static_cast<int>(w.ships.size()), static_cast<int>(w.deposits.size()), net_.PingMs());
    } else if (net_.WorldReady() && phase_ == Phase::Spectating) {
        const World& w = net_.GetWorld();
        r += Fmt(" ships=%d deps=%d wrecks=%d", static_cast<int>(w.ships.size()), static_cast<int>(w.deposits.size()),
                 static_cast<int>(w.wrecks.size()));
    }
    if (!status_.empty()) r += " status=\"" + status_ + "\"";
    return r;
}

// ===========================================================================
// Zeichnen
// ===========================================================================
void MultiplayerScreen::Draw() {
    switch (phase_) {
        case Phase::Address: DrawAddress(); break;
        case Phase::Lobby: DrawLobby(); break;
        case Phase::Login: DrawLogin(); break;
        case Phase::Playing: DrawPlaying(); break;
        case Phase::Spectating: DrawSpectating(); break;
    }
}

// Langsam kreisende Kamera über einer Mondlandschaft (wie im Hauptmenü) und
// links ein dunkler Verlauf, auf dem die Menütexte gut lesbar sind.
void MultiplayerScreen::DrawBackdrop() {
    Vector3 focus{0, 0, 0};
    if (!backdrop_.stations.empty()) focus = backdrop_.stations[0].pos;
    Camera3D cam{};
    cam.up = {0, 1, 0};
    cam.fovy = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    const float r = 58.0f;
    cam.target = focus;
    cam.position = {focus.x + sinf(backdropOrbit_) * r, 34.0f, focus.z + cosf(backdropOrbit_) * r};
    // Kamera seitlich versetzen, damit die Station rechts im Bild steht und
    // nicht hinter den Menütexten auf der linken Seite.
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, cam.up));
    Vector3 shift = Vector3Scale(right, -24.0f);
    cam.target = Vector3Add(cam.target, shift);
    cam.position = Vector3Add(cam.position, shift);
    tiltshift::Begin(Color{14, 17, 26, 255});   // Kameraeffekt (TiltShift.hpp)
    BeginMode3D(cam);
    backdrop_.Draw(cam);
    EndMode3D();
    tiltshift::End();

    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    DrawRectangleGradientH(0, 0, static_cast<int>(W * 0.8f), static_cast<int>(H), Color{10, 12, 20, 245}, Color{10, 12, 20, 0});
}

// Fußzeile aller Menüs (wie im Hauptmenü)
static void MenuFooter(const char* keys) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    Text(keys, 90 * s, H - 44 * s, 18 * s, MUTED);
    TextRight(g_audio.Muted() ? "M Ton aus    F11 Vollbild" : "M Ton an    F11 Vollbild", W - 24 * s, H - 44 * s, 18 * s, MUTED);
}

// --- (a) Serveradresse --------------------------------------------------------------
void MultiplayerScreen::DrawAddress() {
    DrawBackdrop();
    const float s = Scale();
    const float H = static_cast<float>(GetScreenHeight());
    const float x = 90 * s;
    float y = H * 0.10f;
    Text("MEHRSPIELER", x, y, 72 * s, ACCENT);
    Text("Gemeinsam mit anderen auf einem Server Kristalle schürfen", x + 4 * s, y + 84 * s, 24 * s, TEXT);
    y += 150 * s;

    const float fw = 560 * s;
    Text("Serveradresse (Host:Port)", x, y, 20 * s, MUTED);
    y += 28 * s;
    bool submit = addressField_.Draw({x, y, fw, 56 * s}, 26 * s, !connecting_);
    y += 66 * s;
    Text("Beispiele: localhost:7777    192.168.1.20    mein-server.de:7777", x, y, 17 * s, MUTED);
    y += 24 * s;
    Text("Ohne Portangabe wird der Standardport 7777 verwendet.", x, y, 17 * s, MUTED);
    y += 48 * s;

    const float bw = 272 * s, bh = 56 * s;
    bool connect = Button({x, y, bw, bh}, connecting_ ? "Verbinde ..." : "Verbinden", 26 * s, !connecting_, !connecting_);
    bool back = Button({x + bw + 16 * s, y, bw, bh}, connecting_ ? "Abbrechen" : "Zurück", 26 * s, false);
    y += bh + 26 * s;

    // Enter verbindet auch dann, wenn das Feld gerade keinen Fokus hat
    const bool keys = phaseFrames_ >= 1;
    if (!connecting_ && (connect || (keys && (submit || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))))) {
        g_audio.Play(Sfx::UiConfirm);
        StartConnect();
    } else if (back || (keys && BackPressed())) {
        if (connecting_) {
            // Abbrechen: nur den Verbindungsaufbau beenden, im Menü bleiben
            net_.Disconnect();
            connecting_ = false;
            SetStatus("Verbindungsaufbau abgebrochen.", MUTED);
            g_audio.Play(Sfx::UiBack);
        } else {
            wantsMenu_ = true;   // main.cpp spielt UiBack und zeigt das Hauptmenü
        }
    }

    if (connecting_) {
        int dots = static_cast<int>(elapsed_ * 3.0f) % 4;
        Text("Verbinde mit " + Fit(Trim(addressField_.text), 22 * s, fw - 160 * s) + " " + std::string(static_cast<size_t>(dots), '.'), x, y,
             22 * s, TEXT);
    } else if (!status_.empty()) {
        TextBlock(status_, x, y, 22 * s, fw, statusColor_);
    }
    MenuFooter("Enter: verbinden    ESC: zurück");
}

// --- (b) Lobby ------------------------------------------------------------------------
void MultiplayerScreen::DrawLobby() {
    DrawBackdrop();
    const float s = Scale();
    const float H = static_cast<float>(GetScreenHeight());
    const net::ServerInfoMsg& info = net_.Info();
    const float x = 90 * s;
    float y = H * 0.08f;
    Text("MEHRSPIELER", x, y, 56 * s, ACCENT);
    y += 66 * s;
    Text(Fit(info.serverName, 34 * s, 900 * s), x, y, 34 * s, TEXT);
    y += 44 * s;
    // Wird laufend aktualisiert: Der Server schickt in der Lobby alle 2 s eine neue ServerInfo.
    Text(Fmt("Spieler online: %d / %d", info.playersOnline, info.maxPlayers), x, y, 26 * s, GOOD);
    y += 50 * s;

    // --- Infotafel ------------------------------------------------------------
    const float pw = 560 * s;
    const float rowH = 34 * s, ts = 21 * s;
    Rectangle pr{x, y, pw, 8 * rowH + 26 * s};
    Panel(pr);
    float ix = pr.x + 22 * s, iy = pr.y + 16 * s, iw = pr.width - 44 * s;
    const GameSettings& st = info.settings;
    InfoRow("Zuschauer", Fmt("%d / %d", info.spectators, info.maxSpectators), ix, iy, iw, ts);
    iy += rowH;
    InfoRow("Schwierigkeit", std::string(DifficultyName(st.difficulty)) + " (" + EnemiesText(st.enemyShips) + ")", ix, iy, iw, ts);
    iy += rowH;
    InfoRow("Handelsstationen", std::to_string(st.tradeStations), ix, iy, iw, ts);
    iy += rowH;
    InfoRow("Werften", std::to_string(st.shipyards), ix, iy, iw, ts);
    iy += rowH;
    InfoRow("Mondfarbe", MoonColorName(st.moon), ix, iy, iw, ts);
    iy += rowH;
    InfoRow("Insgesamt abgeliefert", Num(info.totalDelivered), ix, iy, iw, ts, ACCENT);
    iy += rowH;
    InfoRow("Server", g_clientConfig.server, ix, iy, iw, ts);
    iy += rowH;
    InfoRow("Ping", Fmt("%.0f ms", net_.PingMs()), ix, iy, iw, ts);

    // --- Knöpfe (rechts daneben) -------------------------------------------------
    const float bx = pr.x + pr.width + 40 * s, bw = 340 * s;
    const bool busy = spectatePending_;
    std::vector<std::string> items = {"Zuschauen", "Mitspielen", "Trennen"};
    int c = ButtonList(items, {!busy, !busy, true}, lobbySel_, bx + bw * 0.5f, pr.y, bw, 58 * s, 14 * s, 26 * s);
    if (BackPressed()) c = 2;       // ESC, Controller: B
    if (phaseFrames_ < 1) c = -1;   // Taste stammt noch aus dem vorigen Bildschirm
    if (c == 0) {
        g_audio.Play(Sfx::UiConfirm);
        net_.Spectate();
        spectatePending_ = true;
        spectateStartT_ = elapsed_;
        SetStatus("Zuschauerplatz wird angefragt ...", MUTED);
    } else if (c == 1) {
        g_audio.Play(Sfx::UiConfirm);
        EnterLogin();
    } else if (c == 2) {
        g_audio.Play(Sfx::UiBack);
        BackToAddress("");
        SetStatus("Verbindung getrennt.", MUTED);
        return;
    }
    // Erklärung zum ausgewählten Knopf
    static const char* help[] = {
        "Die ganze Karte von oben: alle Spieler mit ihrem Fortschritt, die insgesamt abgelieferten Ressourcen und die Bestenliste.",
        "Mit Namen und Passwort anmelden (oder ein neues Konto anlegen) und mitspielen. Dein Fortschritt wird auf dem Server gespeichert.",
        "Verbindung trennen und eine andere Serveradresse eingeben.",
    };
    float hy = pr.y + 3 * (58 * s + 14 * s) + 10 * s;
    if (lobbySel_ >= 0 && lobbySel_ < 3) TextBlock(help[lobbySel_], bx, hy, 18 * s, bw, Color{200, 206, 220, 230});

    if (!status_.empty()) TextBlock(status_, x, pr.y + pr.height + 18 * s, 22 * s, pw, statusColor_);
    // Fußzeile passend zum Gerät (Controls.hpp): Tastatur oder Controller
    if (controls::ActiveDevice() == controls::Device::KeyboardMouse)
        MenuFooter("Pfeiltasten + Enter: wählen    ESC: trennen");
    else
        MenuFooter(Fmt("Steuerkreuz + %s: wählen    %s: trennen", controls::KeyName(controls::Act::Confirm),
                       controls::KeyName(controls::Act::Back)).c_str());
}

// --- (c) Anmeldung -----------------------------------------------------------------------
void MultiplayerScreen::DrawLogin() {
    DrawBackdrop();
    const float s = Scale();
    const float H = static_cast<float>(GetScreenHeight());
    const net::ServerInfoMsg& info = net_.Info();
    const bool busy = authPending_ || net_.State() == NetState::LoggingIn;
    const float x = 90 * s;
    float y = H * 0.08f;
    Text("MITSPIELEN", x, y, 56 * s, ACCENT);
    y += 66 * s;
    Text(Fit(info.serverName, 24 * s, 560 * s) + Fmt("    Spieler online: %d / %d", info.playersOnline, info.maxPlayers), x, y, 24 * s, TEXT);
    y += 52 * s;

    const float fw = 480 * s, fh = 54 * s;
    CycleFocus({&nameField_, &passwordField_});   // Tab wechselt das Feld
    Text("Spielername", x, y, 20 * s, MUTED);
    y += 26 * s;
    bool submit = nameField_.Draw({x, y, fw, fh}, 26 * s, !busy);
    y += fh + 14 * s;
    Text("Passwort", x, y, 20 * s, MUTED);
    y += 26 * s;
    submit = passwordField_.Draw({x, y, fw, fh}, 26 * s, !busy) || submit;
    y += fh + 18 * s;

    // Hinweise: Regeln und was mit dem Passwort passiert
    const float hw = 780 * s, hs = 17 * s;
    const Color hc{200, 206, 220, 230};
    y += TextBlock(Fmt("Name: %d-%d Zeichen, nur Buchstaben (A-Z), Ziffern, _ und -. Jeden Namen gibt es auf dem Server nur "
                       "einmal (Groß/Klein egal).",
                       net::MIN_NAME, net::MAX_NAME),
                   x, y, hs, hw, hc);
    y += TextBlock(Fmt("Passwort: mindestens %d Zeichen. Es wird weder gespeichert noch übertragen - der Server erhält nur einen "
                       "daraus berechneten Schlüssel.",
                       net::MIN_PASSWORD),
                   x, y, hs, hw, hc);
    y += TextBlock("Neu hier? \"Neues Konto anlegen\" legt Name und Passwort fest und meldet dich gleich an. Mit denselben Angaben "
                   "lädst du später deinen Fortschritt und spielst mit deinem letzten Stand weiter.",
                   x, y, hs, hw, MUTED);
    y += 16 * s;

    const float bh = 56 * s;
    bool login = Button({x, y, 220 * s, bh}, "Anmelden", 24 * s, !busy, !busy);
    bool reg = Button({x + 236 * s, y, 320 * s, bh}, "Neues Konto anlegen", 24 * s, false, !busy);
    bool back = Button({x + 572 * s, y, 190 * s, bh}, "Zurück", 24 * s, false);
    y += bh + 18 * s;

    const bool keys = phaseFrames_ >= 1;
    if (!busy && (login || (keys && (submit || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))))) SubmitLogin(false);
    else if (!busy && reg) SubmitLogin(true);
    else if (back || (keys && BackPressed() && !busy)) {
        g_audio.Play(Sfx::UiBack);
        SetPhase(Phase::Lobby);
        SetStatus("", MUTED);
        return;
    }
    if (!status_.empty()) TextBlock(status_, x, y, 22 * s, hw, statusColor_);
    MenuFooter("Enter: anmelden    Tab: Feld wechseln    ESC: zurück");
}

// --- (d) Spielen -------------------------------------------------------------------------
void MultiplayerScreen::DrawPlaying() {
    if (!net_.WorldReady()) {
        DrawBackdrop();
        TextCentered("Lade Spielwelt ...", static_cast<float>(GetScreenWidth()) * 0.5f, static_cast<float>(GetScreenHeight()) * 0.45f,
                     34 * Scale(), TEXT);
        return;
    }
    World& w = net_.GetWorld();
    tiltshift::Begin(Color{14, 17, 26, 255});   // Kameraeffekt (TiltShift.hpp)
    BeginMode3D(cam_);
    w.Draw(cam_);
    controls_.DrawTarget3D(elapsed_);   // Mausziel (nur wenn eins gesetzt ist)
    EndMode3D();
    tiltshift::End();

    // Randpfeile des HUD sollen nicht unter dem Panel "Beste Piloten" landen
    const Rectangle tp = TopPilotsRect();
    DrawShipNames(w);
    SetHudRightReserve(static_cast<float>(GetScreenWidth()) - tp.x + 8 * Scale(), tp.y + tp.height + 8 * Scale());
    SetHudLeftReserve(ChatBox::Height() + 8 * Scale());   // Tastenhilfe über dem Chatfenster
    DrawHud(w, cam_, false);
    SetHudRightReserve(0.0f, 0.0f);   // Standard für den Einzelspieler wiederherstellen
    SetHudLeftReserve(0.0f);
    DrawTopPilots(w);

    // Chat unten links (vor dem Stationsfenster, das darüber liegen darf)
    std::string chatText;
    if (chat_.Draw(chatText)) net_.SendChat(chatText);

    // Stationsfenster: wie im Einzelspieler, aber jede Aktion ist nur eine
    // Anfrage an den Server. Das Ergebnis (neuer Kontostand, Schild ...) kommt
    // mit dem nächsten Snapshot zurück. Während der Zielanimation ausgeblendet
    // (das Ziel wird beim Verkauf erreicht, also immer angedockt).
    if (w.Me().dockedAt >= 0 && goalAnimT_ < 0.0f && undockSentT_ < 0.0f && !paused_) {
        // Beim Tippen im Chat (und im Frame, in dem Enter/ESC ihn geöffnet
        // oder geschlossen hat) ist das Fenster nur Anzeige.
        DockRequest req = DrawDockPanel(w, dockFrames_ > 1 && !opt_.autopilot && !chat_.Busy());
        if (req.repair) net_.SendDockAction(net::DockActionType::Repair);
        if (req.buy >= 0) net_.SendDockAction(net::DockActionType::BuyUpgrade, req.buy);
        // BuyShip: Schiffstyp, Bit 7 = in Zahlung geben (siehe DockActionMsg)
        if (req.buyShip >= 0)
            net_.SendDockAction(net::DockActionType::BuyShip, static_cast<uint8_t>(req.buyShip | (req.tradeIn ? 0x80 : 0)));
        if (req.switchShip >= 0) net_.SendDockAction(net::DockActionType::SwitchShip, static_cast<uint8_t>(req.switchShip));
        if (req.acceptMission >= 0)
            net_.SendDockAction(net::DockActionType::AcceptMission, static_cast<uint8_t>(req.acceptMission));
        if (req.undock) {
            net_.SendDockAction(net::DockActionType::Undock);
            undockSentT_ = 0.0f;   // Fenster ausblenden, bis der Server das Abdocken bestätigt
        }
    }
    DrawNoShip(w);
    if (goalAnimT_ >= 0.0f) DrawGoalAnimation();
    // Pause: entweder das Pausenmenü oder – nach "Optionen" – die Optionen.
    // Schließen sie sich (Zurück/ESC), geht es zurück ins Pausenmenü;
    // pauseFrames_ = 0 verhindert, dass dieselbe ESC-Taste dort gleich
    // auch noch "Weiterspielen" auslöst.
    if (paused_) {
        if (!optionsOpen_) {
            DrawPause();
        } else if (options_.Draw()) {
            optionsOpen_ = false;
            pauseFrames_ = 0;
        }
    }
}

// Name eines Spielers zu einem Schiff: über Ship::playerId (aus dem Snapshot)
// oder, falls das fehlt, über PlayerEntry::shipId.
std::string MultiplayerScreen::NameForShip(const Ship& s) const {
    for (const auto& p : net_.Players()) {
        if (s.playerId >= 0 && p.id == s.playerId) return p.name;
        if (s.playerId < 0 && s.kind == ShipKind::Player && p.shipId >= 0 && p.shipId == s.id) return p.name;
    }
    return "";
}

// Namen über den Schiffen der anderen Spieler (nur wenn auf dem Bildschirm
// und nicht hinter den HUD-Panels oben links bzw. oben rechts), darüber die
// Sprechblase, wenn jemand im Chat tippt (auch über dem eigenen Schiff)
void MultiplayerScreen::DrawShipNames(const World& w) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const Rectangle statusPanel{0, 0, 380 * s, 320 * s};   // Statuspanel des HUD (Hud.cpp) plus Rand
    const Rectangle tp = TopPilotsRect();
    const Rectangle rightColumn{tp.x - 10 * s, 0, W - tp.x + 10 * s, tp.y + tp.height + 6 * s};
    // Eigenes Schiff: Sprechblase, solange wir selbst tippen
    const Ship& own = w.Player();
    if (chat_.Active() && !own.dead && !own.hidden) {
        const Vector2 p = GetWorldToScreen(Vector3Add(own.pos, {0.0f, 2.4f, 0.0f}), cam_);
        DrawChatEmote({p.x, p.y - 4 * s}, elapsed_);
    }
    for (const Ship& sh : w.ships) {
        if (&sh == &w.Player() || sh.dead || sh.hidden) continue;
        if (sh.playerId < 0 && sh.kind != ShipKind::Player) continue;
        if (Dist2D(sh.pos, cam_.target) > w.drawRange) continue;   // wird gar nicht gezeichnet
        const std::string name = NameForShip(sh);
        if (name.empty()) continue;
        Vector2 p = GetWorldToScreen(Vector3Add(sh.pos, {0.0f, 2.4f, 0.0f}), cam_);
        if (p.x < -60 || p.x > W + 60 || p.y < -30 || p.y > H + 30) continue;
        if (CheckCollisionPointRec(p, statusPanel) || CheckCollisionPointRec(p, rightColumn)) continue;
        const float ts = 18 * s;
        const float tw = Width(name, ts);
        DrawRectangleRec({p.x - tw * 0.5f - 6 * s, p.y - ts - 8 * s, tw + 12 * s, ts + 6 * s}, Color{10, 12, 20, 150});
        TextCentered(name, p.x, p.y - ts - 5 * s, ts, sh.crashing ? BAD : GOOD);
        if (sh.chatting) DrawChatEmote({p.x, p.y - ts - 12 * s}, elapsed_);   // tippt gerade im Chat
    }
}

// Platz des eigenen Spielers in der (sortierten) Liste, -1 = nicht enthalten
static int MyIndex(const std::vector<net::PlayerEntry>& list, int myId, const std::string& myName) {
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
        if (list[i].id == myId || (myId < 0 && list[i].name == myName)) return i;
    return -1;
}

// Rechteck des Panels "Beste Piloten". Die Minikarte des HUD ist 230 px groß
// und hat darunter eine zweizeilige Legende (Hud.cpp: Minimap); das Panel
// beginnt darunter. Die Höhe hängt davon ab, wie viele Zeilen es zeigt.
Rectangle MultiplayerScreen::TopPilotsRect() const {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth());
    const std::vector<net::PlayerEntry> list = SortedPlayers(net_.Players());
    const int n = static_cast<int>(list.size());
    const int rows = std::min(3, n);
    const bool extra = MyIndex(list, net_.MyPlayerId(), net_.MyName()) >= 3;
    const float pw = 262 * s;
    float h = 14 * s + 28 * s + static_cast<float>(rows) * 42 * s + (extra ? 30 * s : 0.0f) + (n == 0 ? 26 * s : 0.0f) + 12 * s + 2 * 25 * s + 8 * s;
    return {W - 16 * s - pw, 16 * s + 230 * s + 56 * s, pw, h};
}

// Oben rechts unter der Minikarte: die drei Spieler mit dem weitesten
// Fortschritt, darunter Schiffszeit und Runde des eigenen Spielers.
void MultiplayerScreen::DrawTopPilots(const World& w) {
    const float s = Scale();
    const std::vector<net::PlayerEntry> list = SortedPlayers(net_.Players());
    const int n = static_cast<int>(list.size());
    const int mine = MyIndex(list, net_.MyPlayerId(), net_.MyName());
    const int rows = std::min(3, n);
    const bool extra = mine >= 3;   // eigener Platz außerhalb der Top 3: zusätzliche Zeile
    const float rowH = 42 * s;
    const Rectangle r = TopPilotsRect();
    Panel(r, Color{18, 22, 30, 215});
    const float x = r.x + 14 * s, bw = r.width - 28 * s;
    float y = r.y + 12 * s;
    Text("BESTE PILOTEN", x, y, 17 * s, MUTED);
    TextRight(Fmt("%d online", n), x + bw, y + 1 * s, 16 * s, MUTED);
    y += 28 * s;

    auto row = [&](int i, bool compact) {
        const net::PlayerEntry& e = list[i];
        const bool me = (i == mine);
        if (me) DrawRectangleRec({r.x + 5 * s, y - 4 * s, r.width - 10 * s, (compact ? 24 : 40) * s}, Color{255, 160, 51, 45});
        const Color nc = me ? ACCENT : TEXT;
        Text(Fmt("%d.", i + 1), x, y, 17 * s, MUTED);
        TextRight(Percent(e.balance), x + bw, y, 17 * s, nc);
        Text(Fit(e.name, 17 * s, bw - 30 * s - 56 * s), x + 28 * s, y, 17 * s, nc);
        if (!compact) {
            Bar({x + 28 * s, y + 23 * s, bw - 28 * s - 76 * s, 7 * s}, Progress(e.balance), me ? ACCENT : kYardColor);
            TextRight(Fmt("Runde %d", e.round), x + bw, y + 19 * s, 15 * s, MUTED);
        }
    };
    for (int i = 0; i < rows; ++i) {
        row(i, false);
        y += rowH;
    }
    if (extra) {
        row(mine, true);
        y += 30 * s;
    }
    if (n == 0) {
        Text("Warte auf Spielerliste ...", x, y, 15 * s, MUTED);
        y += 26 * s;
    }

    // Eigene Zeiten: Schiffszeit beginnt nach einem Absturz von vorn,
    // Rundenzeit nach jedem erreichten Ziel.
    DrawLineEx({x, y}, {x + bw, y}, 1.0f, Color{255, 255, 255, 40});
    y += 10 * s;
    const PlayerState& me = w.Me();
    InfoRow("Schiffszeit", Time(me.playTime), x, y, bw, 17 * s);
    y += 25 * s;
    InfoRow(Fmt("Runde %d · Rundenzeit", me.round), Time(me.roundTime), x, y, bw, 17 * s);
}

// Kein eigenes Schiff: entweder abgeschossen (neues kommt nach respawnT
// Sekunden) oder ganz am Anfang, bevor der erste Snapshot da ist.
void MultiplayerScreen::DrawNoShip(const World& w) {
    const Ship& p = w.Player();
    if (p.crashing || (!p.dead && !p.hidden)) return;   // Absturz zeigt das HUD ("SCHIFF STÜRZT AB!")
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    if (!hadShip_) {
        const std::string t = "Dein Raumschiff wird bereitgestellt ...";
        const float tw = Width(t, 28 * s);
        Panel({W * 0.5f - tw * 0.5f - 30 * s, H * 0.40f, tw + 60 * s, 64 * s});
        TextCentered(t, W * 0.5f, H * 0.40f + 18 * s, 28 * s, TEXT);
        return;
    }
    Rectangle r{W * 0.5f - 340 * s, H * 0.30f, 680 * s, 190 * s};
    Panel(r, Color{26, 20, 24, 235}, BAD);
    TextCentered("SCHIFF ZERSTÖRT", W * 0.5f, r.y + 24 * s, 50 * s, BAD);
    const float t = w.Me().respawnT;
    std::string line = t > 0.05f ? Fmt("Neues Raumschiff in %d s.", static_cast<int>(ceilf(t))) : std::string("Neues Raumschiff wird bereitgestellt ...");
    TextCentered(line, W * 0.5f, r.y + 92 * s, 28 * s, TEXT);
    TextCentered("Dein Fortschritt beginnt von vorn.", W * 0.5f, r.y + 134 * s, 22 * s, MUTED);
}

// Große Abschlussanimation nach dem eigenen Ziel (ca. 6 s, Enter/Klick überspringt)
void MultiplayerScreen::DrawGoalAnimation() {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const float t = goalAnimT_;
    // Ein- und Ausblenden: 0,4 s hinein, 0,8 s hinaus
    const float a = std::clamp(std::min(t / 0.4f, (kGoalAnimTime - t) / 0.8f), 0.0f, 1.0f);
    auto fade = [&](Color c, float k = 1.0f) {
        c.a = static_cast<unsigned char>(c.a * a * k);
        return c;
    };
    // Das Band liegt im unteren Bilddrittel: Das eigene Schiff steht in der
    // Bildmitte, das Feuerwerk darüber – beides soll sichtbar bleiben.
    const float bandH = 270 * s;
    const float y0 = std::min(H * 0.60f, H - bandH - 30 * s);
    DrawRectangleGradientV(0, static_cast<int>(y0 - 40 * s), static_cast<int>(W), static_cast<int>(40 * s), fade(Color{10, 12, 20, 0}),
                           fade(Color{10, 12, 20, 180}));
    DrawRectangle(0, static_cast<int>(y0), static_cast<int>(W), static_cast<int>(bandH), fade(Color{10, 12, 20, 180}));
    DrawRectangleGradientV(0, static_cast<int>(y0 + bandH), static_cast<int>(W), static_cast<int>(40 * s), fade(Color{10, 12, 20, 180}),
                           fade(Color{10, 12, 20, 0}));

    // Pulsierender Titel, Farbe wechselt zwischen Grün und Orange
    const float pulse = 1.0f + 0.06f * sinf(t * 7.0f);
    const float ts = 88 * s * pulse;
    const float mix = 0.5f + 0.5f * sinf(t * 2.3f);
    Color tc = ColorLerp(GOOD, ACCENT, mix);
    TextCentered("ZIEL ERREICHT!", W * 0.5f, y0 + 18 * s + (88 * s - ts) * 0.5f, ts, fade(tc));

    const std::string kind = goalRound_ <= 1 ? "mit neuem Schiff" : "mit Aufrüstung";
    TextCentered(Fmt("Zeit %s   ·   Runde %d (%s)", Hms(goalSeconds_).c_str(), goalRound_, kind.c_str()), W * 0.5f, y0 + 122 * s, 30 * s,
                 fade(TEXT));
    TextCentered(Fmt("Dein Schiff behält seine Aufrüstungen - weiter geht's mit Runde %d.", goalRound_ + 1), W * 0.5f, y0 + 168 * s, 24 * s,
                 fade(Color{200, 220, 255, 255}));
    TextCentered("Dein Ergebnis steht jetzt in der Bestenliste.", W * 0.5f, y0 + 204 * s, 20 * s, fade(MUTED));
    if (t > 1.0f) TextCentered(Fmt("%s: weiter", controls::KeyName(controls::Act::Confirm)), W * 0.5f, y0 + bandH - 32 * s, 16 * s, fade(MUTED));
}

// Aufbau von oben: Überschrift, Hinweis, drei Knöpfe, Hinweis zum Abmelden,
// Tastenhilfe. Die Fensterhöhe ergibt sich aus diesen Teilen (ControlsMaxHeight
// aus Hud.hpp), damit Knöpfe und Tastenhilfe sich nie überlappen und die
// Knöpfe beim Wechsel zwischen Maus und Controller nicht verrutschen.
void MultiplayerScreen::DrawPause() {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 150});
    std::vector<std::string> items = {"Weiterspielen", "Optionen", "Abmelden & zum Hauptmenü"};
    const float bh = 54 * s, gap = 12 * s, listTop = 126 * s;   // Knöpfe (listTop relativ zur Oberkante)
    // n Knöpfe belegen n * (bh + gap) - gap; darunter 18 px Abstand, Hinweiszeile (16 px) und 16 px Abstand
    const float noteTop = listTop + static_cast<float>(items.size()) * (bh + gap) - gap + 18 * s;
    const float helpTop = noteTop + 32 * s;
    const float helpSize = 18 * s;
    const float ph = helpTop + ControlsMaxHeight(helpSize) + 16 * s;
    Rectangle r{W * 0.5f - 300 * s, H * 0.5f - ph * 0.5f, 600 * s, ph};
    Panel(r);
    TextCentered("PAUSE", W * 0.5f, r.y + 26 * s, 50 * s, ACCENT);
    TextCentered("Das Spiel läuft auf dem Server weiter!", W * 0.5f, r.y + 86 * s, 20 * s, MUTED);
    int c = ButtonList(items, {true, true, true}, pauseSel_, W * 0.5f, r.y + listTop, 440 * s, bh, gap, 24 * s);
    // ESC im selben Frame, in dem die Pause geöffnet wurde, darf sie nicht gleich schließen.
    // Controller: B oder die Menü-Taste (BackPressed) schließen sie wie ESC.
    if (pauseFrames_ > 1 && (BackPressed() || IsKeyPressed(KEY_P))) c = 0;
    if (c == 0) {
        paused_ = false;
        g_audio.Play(Sfx::PauseClose);
    } else if (c == 1) {
        // Optionen: ab dem nächsten Frame statt des Pausenmenüs (DrawPlaying)
        g_audio.Play(Sfx::UiConfirm);
        optionsOpen_ = true;
        options_.Open();
        return;
    } else if (c == 2) {
        net_.Disconnect();   // Leave: der Server speichert den Fortschritt sofort
        wantsMenu_ = true;
    }
    // Die Pause gibt es nur im freien Flug, also gilt hier immer: Das Schiff
    // bleibt nach dem Abmelden noch 10 s im All (GameServer::LogoutPlayer).
    TextCentered("Abmelden im All: Dein Schiff bleibt noch 10 s schutzlos stehen.", W * 0.5f, r.y + noteTop, 16 * s,
                 MUTED);
    DrawControls(r.x + 70 * s, r.y + helpTop, helpSize);
}

// --- (e) Zuschauen ---------------------------------------------------------------------------
void MultiplayerScreen::DrawSpectating() {
    if (!net_.WorldReady()) {
        DrawBackdrop();
        TextCentered("Lade Karte ...", static_cast<float>(GetScreenWidth()) * 0.5f, static_cast<float>(GetScreenHeight()) * 0.45f,
                     34 * Scale(), TEXT);
        return;
    }
    const float s = Scale();
    const float H = static_cast<float>(GetScreenHeight());
    World& w = net_.GetWorld();
    tiltshift::Begin(Color{14, 17, 26, 255});   // Kameraeffekt (TiltShift.hpp)
    BeginMode3D(cam_);
    w.Draw(cam_);
    EndMode3D();
    tiltshift::End();
    DrawSpectatorMarkers(w);

    const Rectangle area = SpectatorMapArea();
    const float cx = area.x + area.width * 0.5f;

    // Titelleiste über der Karte
    DrawRectangleRec({area.x, 12 * s, area.width, 38 * s}, Color{10, 12, 20, 200});
    std::string title = "ZUSCHAUER  ·  " + w.settings.serverName;
    TextCentered(Fit(title, 22 * s, area.width - 20 * s), cx, 20 * s, 22 * s, ACCENT);
    DrawNotices(w, area.y + 8 * s);

    // Legende und Bedienhilfe unter der Karte
    const float ly = H - 58 * s;
    struct L { const char* t; Color c; bool square; } legend[] = {
        {"Spieler", GOOD, false}, {"Pirat", BAD, false}, {"Händler", kTraderColor, false}, {"Handel", ACCENT, true}, {"Werft", kYardColor, true},
    };
    float lw = 0;
    for (const L& l : legend) lw += 20 * s + Width(l.t, 16 * s) + 18 * s;
    float lx = cx - lw * 0.5f;
    DrawRectangleRec({area.x, ly - 6 * s, area.width, 54 * s}, Color{10, 12, 20, 170});
    for (const L& l : legend) {
        if (l.square) DrawRectangleV({lx, ly + 3 * s}, {10 * s, 10 * s}, l.c);
        else DrawCircleV({lx + 5 * s, ly + 8 * s}, 5 * s, l.c);
        Text(l.t, lx + 16 * s, ly, 16 * s, TEXT);
        lx += 20 * s + Width(l.t, 16 * s) + 18 * s;
    }
    const char* specHelp = controls::ActiveDevice() == controls::Device::KeyboardMouse
                               ? "Mausrad: Zoom  ·  Ziehen: verschieben  ·  R: ganze Karte  ·  ESC: zurück"
                               : controls::ActiveDevice() == controls::Device::PlayStation
                                     ? "Rechter Stick: Zoom  ·  Kreuz halten + linker Stick: verschieben  ·  Kreis: zurück"
                                     : "Rechter Stick: Zoom  ·  A halten + linker Stick: verschieben  ·  B: zurück";
    TextCentered(Fit(specHelp, 15 * s, area.width - 10 * s), cx, H - 32 * s, 15 * s, MUTED);

    DrawPlayerList(SpectatorLeftPanel());
    DrawHighscores(SpectatorRightPanel());
}

// Markierungen auf der Zuschauerkarte: Die 3D-Modelle sind aus dieser Höhe nur
// wenige Pixel groß, deshalb bekommt jedes Schiff ein farbiges Symbol.
void MultiplayerScreen::DrawSpectatorMarkers(const World& w) {
    const float s = Scale();
    // Spielfeldrand
    Vector2 a = GetWorldToScreen({-cfg::WORLD_HALF, 0, -cfg::WORLD_HALF}, cam_);
    Vector2 b = GetWorldToScreen({cfg::WORLD_HALF, 0, cfg::WORLD_HALF}, cam_);
    DrawRectangleLinesEx({a.x, a.y, b.x - a.x, b.y - a.y}, std::max(1.0f, 2 * s), Color{255, 255, 255, 60});

    for (const Deposit& d : w.deposits) {
        Vector2 p = GetWorldToScreen(d.pos, cam_);
        DrawCircleV(p, 2.6f * s, Color{110, 240, 170, 170});
    }
    for (const Wreck& wr : w.wrecks) {
        Vector2 p = GetWorldToScreen(wr.pos, cam_);
        DrawRectangleV({p.x - 2.5f * s, p.y - 2.5f * s}, {5 * s, 5 * s}, Color{255, 170, 60, 220});
    }
    for (const Station& st : w.stations) {
        const bool trade = st.type == StationType::Trade;
        Vector2 p = GetWorldToScreen(st.pos, cam_);
        Color c = trade ? ACCENT : (st.type == StationType::Shipyard ? kYardColor : Color{150, 255, 90, 255});
        float ty = p.y + 30 * s / std::max(0.4f, specZoom_ * 1.6f);
        TextCentered(st.name, p.x, ty, 17 * s, c);
        if (trade) TextCentered(Fmt("Stufe %d", w.StationStage()), p.x, ty + 19 * s, 15 * s, MUTED);
    }
    // Erst Gegner und Händler, zuletzt die Spieler (sollen obenauf liegen)
    for (int pass = 0; pass < 2; ++pass) {
        for (const Ship& sh : w.ships) {
            if (sh.dead || sh.hidden || &sh == &w.Player()) continue;
            const bool isPlayer = sh.playerId >= 0 || sh.kind == ShipKind::Player;
            if (isPlayer != (pass == 1)) continue;
            Vector2 p = GetWorldToScreen(sh.pos, cam_);
            float alpha = sh.crashing ? (0.4f + 0.4f * sinf(elapsed_ * 20.0f)) : 1.0f;
            if (isPlayer) {
                Color c = Fade(GOOD, alpha);
                DrawCircleLinesV(p, 12 * s, c);
                ShipArrow(p, sh.yaw, 9 * s, c);
                const std::string name = NameForShip(sh);
                if (!name.empty()) {
                    const float ts = 17 * s, tw = Width(name, ts);
                    DrawRectangleRec({p.x - tw * 0.5f - 5 * s, p.y - 36 * s, tw + 10 * s, ts + 6 * s}, Color{10, 12, 20, 170});
                    TextCentered(name, p.x, p.y - 33 * s, ts, c);
                }
            } else if (sh.faction == Faction::Pirate) {
                ShipArrow(p, sh.yaw, 7 * s, Fade(BAD, alpha));
            } else {
                ShipArrow(p, sh.yaw, 6 * s, Fade(kTraderColor, alpha));
            }
        }
    }
}

// Meldungen (z. B. "Anna hat das Ziel erreicht!") für den Zuschauer; im Spiel
// zeichnet sie das HUD.
void MultiplayerScreen::DrawNotices(const World& w, float top) {
    const float s = Scale();
    const Rectangle area = SpectatorMapArea();
    const float cx = area.x + area.width * 0.5f;
    float ny = top;
    for (const Notice& n : w.Me().notices) {
        float a = std::clamp(n.time / 0.6f, 0.0f, 1.0f);
        Color c = n.color;
        c.a = static_cast<unsigned char>(255 * a);
        std::string t = Fit(n.count > 1 ? n.text + Fmt("  (%dx)", n.count) : n.text, 22 * s, area.width - 40 * s);
        float tw = Width(t, 22 * s);
        DrawRectangle(static_cast<int>(cx - tw * 0.5f - 12 * s), static_cast<int>(ny - 6 * s), static_cast<int>(tw + 24 * s),
                      static_cast<int>(34 * s), Color{15, 18, 26, static_cast<unsigned char>(170 * a)});
        TextCentered(t, cx, ny, 22 * s, c);
        ny += 40 * s;
    }
}

// Linke Leiste: alle angemeldeten Spieler, sortiert nach Fortschritt
void MultiplayerScreen::DrawPlayerList(Rectangle r) {
    const float s = Scale();
    Panel(r);
    const float x = r.x + 16 * s, bw = r.width - 32 * s;
    float y = r.y + 14 * s;
    const std::vector<net::PlayerEntry> list = SortedPlayers(net_.Players());
    const int n = static_cast<int>(list.size());
    Text("SPIELER ONLINE", x, y, 22 * s, ACCENT);
    const int maxPlayers = net_.HasInfo() ? net_.Info().maxPlayers : 0;
    TextRight(maxPlayers > 0 ? Fmt("%d / %d", n, maxPlayers) : std::to_string(n), x + bw, y + 3 * s, 18 * s, MUTED);
    y += 40 * s;
    if (n == 0) {
        TextBlock("Gerade ist niemand angemeldet.", x, y, 18 * s, bw, MUTED);
        return;
    }

    // Wie viele Zeilen passen? Bei Überlänge wird geblättert (Mausrad über der
    // Liste) und unten steht, wie viele noch folgen.
    const float rowH = 64 * s;
    const float bottom = r.y + r.height - 12 * s;
    int visible = std::max(1, static_cast<int>((bottom - y) / rowH));
    if (visible < n) visible = std::max(1, static_cast<int>((bottom - 26 * s - y) / rowH));   // Platz für die Fußzeile
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, n - visible));

    for (int i = listScroll_; i < std::min(n, listScroll_ + visible); ++i) {
        const net::PlayerEntry& e = list[i];
        const Color nc = e.alive ? TEXT : MUTED;   // ohne Schiff (Absturz) grau
        Text(Fmt("%d.", i + 1), x, y, 18 * s, MUTED);
        const std::string pct = Percent(e.balance);
        TextRight(pct, x + bw, y, 19 * s, ACCENT);
        // Zustand (angedockt / kein Schiff) klein zwischen Name und Prozent
        const std::string state = !e.alive ? "kein Schiff" : (e.docked ? "angedockt" : "");
        float stateW = 0.0f;
        if (!state.empty()) {
            stateW = Width(state, 15 * s) + 10 * s;
            TextRight(state, x + bw - Width(pct, 19 * s) - 10 * s, y + 3 * s, 15 * s, e.alive ? kYardColor : BAD);
        }
        Text(Fit(e.name, 19 * s, bw - 34 * s - Width(pct, 19 * s) - stateW - 14 * s), x + 34 * s, y, 19 * s, nc);
        Bar({x + 34 * s, y + 25 * s, bw - 34 * s, 8 * s}, Progress(e.balance), ACCENT);
        Text(Fmt("Runde %d", e.round), x + 34 * s, y + 39 * s, 15 * s, MUTED);
        TextRight("Schiffszeit " + Time(e.shipTime), x + bw, y + 39 * s, 15 * s, MUTED);
        y += rowH;
    }
    const int below = n - (listScroll_ + visible);
    if (below > 0) Text(Fmt("+%d weitere (Mausrad)", below), x, bottom - 20 * s, 17 * s, MUTED);
    else if (listScroll_ > 0) Text(Fmt("%d weiter oben (Mausrad)", listScroll_), x, bottom - 20 * s, 17 * s, MUTED);
}

// Rechte Leiste: insgesamt abgelieferte Ressourcen und Bestenliste
void MultiplayerScreen::DrawHighscores(Rectangle r) {
    const float s = Scale();
    Panel(r);
    const float x = r.x + 16 * s, bw = r.width - 32 * s;
    float y = r.y + 14 * s;
    Text("INSGESAMT ABGELIEFERT", x, y, 20 * s, MUTED);
    y += 26 * s;
    Text(Num(net_.TotalDelivered()), x, y, 40 * s, ACCENT);
    y += 50 * s;
    // Ausbaustufe der Handelsstationen (wächst mit der Gesamtmenge)
    if (net_.WorldReady()) {
        const World& w = net_.GetWorld();
        const int stage = w.StationStage();
        const int next = w.NextStageThreshold();
        Text(Fmt("Handelsstationen: Stufe %d von %d", stage, cfg::STATION_STAGES - 1), x, y, 16 * s, TEXT);
        y += 22 * s;
        float f = 1.0f;
        if (next > 0) {
            const int cur = cfg::STATION_THRESHOLDS[stage];
            f = static_cast<float>(w.totalDelivered - cur) / static_cast<float>(std::max(1, next - cur));
        }
        Bar({x, y, bw, 8 * s}, f, Color{255, 210, 120, 255});
        y += 24 * s;
    }
    DrawLineEx({x, y}, {x + bw, y}, 1.0f, Color{255, 255, 255, 40});
    y += 14 * s;

    Text("BESTENLISTE", x, y, 22 * s, ACCENT);
    TextRight("kürzeste Zeit zuerst", x + bw, y + 5 * s, 15 * s, MUTED);
    y += 36 * s;
    const std::vector<net::HighscoreEntry>& hs = net_.Highscores();
    const int n = static_cast<int>(hs.size());
    if (n == 0) {
        TextBlock("Noch hat niemand das Ziel (100.000 Punkte) erreicht.", x, y, 18 * s, bw, MUTED);
        return;
    }
    const float rowH = 48 * s;
    const float bottom = r.y + r.height - 12 * s;
    int visible = std::max(1, static_cast<int>((bottom - y) / rowH));
    if (visible < n) visible = std::max(1, static_cast<int>((bottom - 24 * s - y) / rowH));
    // Reihenfolge wie vom Server geliefert (kürzeste Zeit zuerst)
    for (int i = 0; i < std::min(n, visible); ++i) {
        const net::HighscoreEntry& e = hs[i];
        Text(Fmt("%d.", i + 1), x, y, 19 * s, i < 3 ? ACCENT : MUTED);
        TextRight(Hms(e.seconds), x + bw, y, 19 * s, GOOD);
        Text(Fit(e.name, 19 * s, bw - 36 * s - 100 * s), x + 36 * s, y, 19 * s, TEXT);
        // Runde 1 = mit einem neuen Schiff geschafft; ab Runde 2 hatte das Schiff
        // seine Aufrüstungen aus den vorigen Runden behalten (nicht direkt vergleichbar).
        std::string kind = e.round <= 1 ? std::string("neues Schiff") : Fmt("Runde %d (mit Aufrüstung)", e.round);
        Text(kind, x + 36 * s, y + 23 * s, 15 * s, MUTED);
        y += rowH;
    }
    if (n > visible) Text(Fmt("+%d weitere", n - visible), x, bottom - 20 * s, 17 * s, MUTED);
}
