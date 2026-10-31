// ---------------------------------------------------------------------------
// main.cpp – Einstiegspunkt des Spiels Mondschürfer (das Programm mit Fenster).
//
// Mondschürfer ist ein kleines Weltraum-Bergbau-Spiel mit dem Kenney Space Kit:
// Man fliegt über die Mondoberfläche, sucht mit dem Scanner Kristalle, baut sie
// ab, verkauft sie an Handelsstationen und rüstet das Schiff in Werften auf.
// Ziel im Einzelspieler: 100.000 Ressourcenpunkte (cfg::GOAL_POINTS) auf dem Konto.
//
// Steuerung (Tabellen in README.md, Umsetzung in src/Controls.cpp):
//   Tastatur:   W/S Schub/Bremsen, A/D drehen, Leertaste schießen, E Scanner,
//               F abbauen/andocken, Mausrad Zoom, ESC/P Pause, M Ton an/aus,
//               F11 Vollbild.
//   Maus:       Linksklick auf den Boden = dorthin fliegen und anhalten,
//               linke Taste halten = das Schiff folgt dem Zeiger, rechte
//               Taste = Flugziel löschen (Schießen, Scanner, Andocken bleiben
//               auf der Tastatur).
//   Controller: linker Stick = Flugrichtung im Bild, RT = Tempo, LT = bremsen,
//               A schießen, B Scanner, Y abbauen/andocken, Menü-Taste Pause,
//               rechter Stick hoch/runter Zoom. PlayStation an denselben
//               Stellen: Kreuz, Kreis, Dreieck, R2/L2, Options.
//               In allen Menüs: linker Stick bewegt den Zeiger, A klickt,
//               Steuerkreuz wählt, B = zurück.
//
// Wofür ist die Datei da?
//   - main() (ganz unten) liest die Kommandozeile (nur für automatische Tests),
//     öffnet das Fenster, lädt Grafik (g_assets) und Ton (g_audio) und läuft
//     dann in der Hauptschleife, bis das Fenster geschlossen wird.
//   - Die Klasse App verwaltet die Bildschirme (enum Screen): Hauptmenü,
//     Rollenwahl, Einzelspiel, Command Center (src/CommandCenter.hpp),
//     Pause, Optionen, Simulation, Mehrspieler und
//     Karteneditor (src/MapEditor.hpp). Sie
//     entscheidet jedes Frame, was gerechnet, abgespielt (Effekte, Musik)
//     und gezeichnet wird.
//
// Einzelspieler und Mehrspieler:
//   - Hauptmenü, Einzelspiel, Pause und Simulation rechnet App selbst mit einer
//     lokalen World (shared/World.hpp) – derselben Spiellogik, die auch der
//     Server benutzt. Im Hauptmenü fliegt im Hintergrund der Autopilot in einer
//     eigenen Zufallswelt (stumm, wird nie gespeichert).
//   - Für den Mehrspieler erzeugt App ein MultiplayerScreen-Objekt
//     (src/Multiplayer.hpp) und gibt ihm jedes Frame Update, Kamera, Zeichnen
//     und Ton ab. Es lebt nur, solange man im Mehrspieler ist (Verbindung über
//     NetClient, Spiegelwelt, Menüs). Die lokale World von App ruht so lange.
//
// Wer benutzt die Datei?
//   Niemand ruft etwas daraus auf: Sie enthält main() und wird im Build
//   (CMakeLists.txt, Programm "Mondschuerfer") mit den anderen src/- und
//   shared/-Dateien zusammengebaut. Automatische Tests starten das Programm mit
//   Optionen wie --mode, --screenshot, --report (Liste in main()).
//
// Abhängigkeiten:
//   - raylib.h (Fenster, Tastatur/Maus/Gamepad, Zeichnen, Kamera),
//     raymath.h (Vektor-Rechnung wie Vector3Lerp, Clamp),
//     rlgl.h (die OpenGL-nahe Schicht unter raylib; hier nur für die
//     Clip-Ebenen der Kamera gebraucht).
//   - shared/World.hpp: Spiellogik und Autopilot (shared/Ai.cpp).
//     World::Draw ist in src/Render.cpp definiert.
//   - src/Hud.hpp (Spieloberfläche, Stationsfenster, Endbildschirme),
//     src/Controls.hpp (Flugsteuerung mit Tastatur, Maus und Controller),
//     src/Ui.hpp (Knöpfe, Texte, Skalierung, Controller in Menüs),
//     src/Audio.hpp (g_audio),
//     src/Assets.hpp (g_assets: Modelle, Shader, Mondfarbe),
//     src/Save.hpp (Spielstand-Datei), src/Multiplayer.hpp,
//     src/Options.hpp (Bildschirm "Optionen"),
//     src/ClientConfig.hpp (g_clientConfig: Einstellungsdatei, u. a. Musik).
//
// Ablauf eines Frames (App::Frame) – alles läuft im einzigen Thread des Spiels:
//   1. Tasten, die überall gelten (F11, M, Mausrad)
//   2. Spielzustand des aktuellen Bildschirms weiterrechnen (UpdatePlaying ...)
//   3. Kamera nachführen, Töne abspielen
//   4. Zeichnen: erst die 3D-Welt, dann die 2D-Oberfläche darüber.
//      Die Oberfläche arbeitet im "Immediate Mode": Ein Knopf wird jedes Frame
//      neu gezeichnet und meldet dabei sofort, ob er angeklickt wurde. Deshalb
//      stehen Bildschirmwechsel (Neues Spiel, Pause beenden ...) auch in den
//      Draw...()-Funktionen und nicht nur in den Update...()-Funktionen.
//   5. Test-Optionen (Bildschirmfotos, Berichte, automatisches Beenden)
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "Assets.hpp"
#include "Audio.hpp"
#include "ClientConfig.hpp"
#include "ClipPlayer.hpp"
#include "CommandCenter.hpp"
#include "Controls.hpp"
#include "Effects.hpp"
#include "Hud.hpp"
#include "MapEditor.hpp"
#include "Multiplayer.hpp"
#include "Options.hpp"
#include "Save.hpp"
#include "TiltShift.hpp"
#include "Ui.hpp"
#include "World.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// Text, Button, Scale, die Farben ACCENT/MUTED/TEXT ... stammen aus src/Ui.hpp
using namespace ui;

// Anonymer Namensraum: alles darin ist nur in dieser Datei sichtbar
// (keine Namenskonflikte mit gleichnamigen Hilfen in anderen .cpp-Dateien).
namespace {

// Die Bildschirme des Spiels. Pro Frame ist genau einer aktiv (App::screen_).
//   Menu        Hauptmenü, dahinter fliegt der Autopilot in einer Zufallswelt
//   Roles       Rollenwahl nach "Neues Spiel"/"Fortsetzen": Pilot oder SG-Team
//               (die neue Welt steht still, die Kamera kreist wie im Menü)
//   Playing     Einzelspiel (auch angedockt, auf den Endbildschirmen und als
//               SG-Team im Einsatz)
//   Command     Command Center des SG-Teams (CommandCenterScreen), die Welt
//               steht still
//   Paused      Einzelspiel angehalten, Pausenmenü
//   Options     Optionen (Musik, Steuerung), geöffnet aus dem Hauptmenü oder
//               aus der Pause; App::optionsReturn_ merkt sich, wohin "Zurück"
//               führt. Aus dem Hauptmenü läuft dahinter die Menü-Welt weiter,
//               aus der Pause bleibt das Spiel angehalten.
//   Simulation  der Autopilot spielt eine ganze Partie vor (mit HUD)
//   Multiplayer alles erledigt das MultiplayerScreen-Objekt (die Optionen
//               der Mehrspieler-Pause auch)
//   Editor      Karteneditor, alles erledigt das MapEditorScreen-Objekt
enum class Screen { Menu, Playing, Paused, Options, Simulation, Multiplayer, Editor, Roles, Command };

// Optionen für automatisierte Tests (Kommandozeile), im normalen Spiel ohne Bedeutung.
// Zeiten beziehen sich auf die Spielzeit seit dem Start (App::elapsed_).
struct TestOptions {
    std::string screenshot;        // --screenshot DATEI: ein Bildschirmfoto ...
    float shotAt = -1.0f;          // --shot-at SEK: ... zu diesem Zeitpunkt
    float quitAfter = -1.0f;       // --quit-after SEK: danach beenden
    // --mode: direkt starten mit "sim" (Simulation), "game" (neues Spiel),
    // "dock0" (angedockt an Handelsstation 0 mit 240 Fracht, also Verkauf),
    // "dock1" (angedockt an Werft 1 mit 4.200 Guthaben, also Aufrüsten),
    // "pause" (neues Spiel im Pausenmenü), "continue" (Spielstand laden),
    // "options" (Optionen, wie aus dem Hauptmenü geöffnet)
    std::string mode;
    int steps = 1;                 // --steps N: Simulationsschritte pro Frame (Zeitraffer)
    unsigned int seed = 0;         // --seed N: feste Welt; 0 = Zufall aus der Uhrzeit
    bool report = false;           // --report: alle 10 s eine Zustandszeile nach stderr
    long long deliver = -1;        // --deliver N: Gesamtablieferung vorgeben (Ausbaustufe der Stationen)
    float posX = 0, posZ = 0;      // --pos X Z: Startposition des Schiffs
    bool hasPos = false;
    float zoom = 0.0f;             // --zoom Z: Kameraabstand (siehe zoom_)
    bool scan = false;             // --scan: Scanner beim Start eingeschaltet
    bool hasCam = false;           // --cam PX PY PZ TX TY TZ: feste Kamera (Position, Ziel)
    int winW = 1600, winH = 900;   // --size B H: Fenstergröße (Tests der Bildschirmaufteilung, z. B. 960 540)
    Vector3 camPos{}, camTarget{};
    bool noHud = false;            // --nohud: nur die 3D-Welt zeichnen
    bool noRender = false;         // --norender: nichts zeichnen, Bildrate unbegrenzt (schnelle Läufe)
    float shotEvery = 0.0f;        // --shot-every SEK: Bildserie seq_000.png, seq_001.png ...
    bool thrustTest = false;       // --thrust-test: Vollschub nach rechts (+x) und Dauerfeuer
    int pirates = 0;               // --pirates N: N Piraten dicht beim Spieler erzeugen
    float hp = -1.0f;              // --hp H: Schildstand vorgeben (z. B. Warnton unter 30 %)
    bool dumpModelBounds = false;  // --dump-model-bounds: Tabelle für shared/ModelBounds.inc ausgeben und beenden
    long long balance = -1;        // --balance N: Kontostand vorgeben
    // --music-test: prüft die Musik (braucht ein Audiogerät). Spielt einen
    // festen Ablauf durch – Hauptmenü, neues Spiel, Musik aus, wieder an,
    // Lautstärke 20 %, zurück ins Menü, Optionen – und schreibt alle 0,25 s
    // eine Zeile MUSICTEST mit dem Zustand der Musik nach stderr (siehe
    // App::MusicTest). Achtung: speichert die Musik-Einstellungen wie ein
    // Spieler in den Optionen (Lautstärke danach 20 %).
    bool musicTest = false;
    // --mouse-test X Z: im Einzelspiel ein Mausziel bei (X, Z) setzen, wie
    // ein Linksklick auf diese Stelle am Boden. Mehrmals angegeben: Ist ein
    // Ziel erreicht (oder aufgegeben), kommt 4 s später das nächste – so
    // sieht man auch, ob das Schiff am Ziel stehen bleibt. Alle 2 s und bei
    // jedem Zielende eine Zeile MOUSETEST nach stderr (App::MouseTest).
    std::vector<Vector2> mouseTargets;   // x = Welt-x, y = Welt-z
    // --pad-test WINKEL TRIGGER: einen Controller vortäuschen – linker Stick
    // dauerhaft in Richtung WINKEL (Grad im Bild: 0 = oben, 90 = rechts),
    // rechter Trigger = TRIGGER (0..1). Jede Sekunde eine Zeile PADTEST
    // (App::PadTest). Die Hilfetexte zeigen dann die Controller-Belegung.
    bool padTest = false;
    float padAngle = 0.0f, padTrigger = 0.0f;
    // --pad-name NAME: Name des vorgetäuschten Controllers (z. B.
    // "DualSense Wireless Controller" -> PlayStation-Namen in den
    // Hilfetexten). Für Bildschirmfotos; auch ohne --pad-test nutzbar.
    std::string padName;
    // Mehrspieler (--mp-connect ADRESSE startet direkt im Mehrspieler-Bildschirm,
    // --mp-name/--mp-pass melden automatisch an, --mp-register legt das Konto an,
    // --mp-spectate schaut zu, --mp-autopilot lässt den Autopiloten fliegen)
    MultiplayerOptions mp;
};

// ===========================================================================
// App: Bildschirme, Spielablauf, Kamera, Ton und Zeichnen (siehe Dateikopf)
// ===========================================================================
class App {
public:
    explicit App(const TestOptions& opt) : opt_(opt) {}

    // Einmal nach dem Laden der Assets. Normalerweise beginnt das Spiel im
    // Hauptmenü; die Test-Optionen können direkt in einen anderen Bildschirm
    // springen. Die Hintergrundwelt wird immer erzeugt, damit World::Player()
    // usw. von Anfang an gültige Daten liefern.
    void Start() {
        hasSave_ = SaveExists();
        StartBackground();
        if (!opt_.mp.connect.empty()) EnterMultiplayer(opt_.mp);
        else if (opt_.mode == "sim") StartSimulation();
        else if (opt_.mode == "game" || opt_.mode == "dock0" || opt_.mode == "dock1" || opt_.mode == "pause") {
            NewGame();
            if (opt_.mode == "pause") screen_ = Screen::Paused;
        } else if (opt_.mode == "continue") {
            Continue();
        } else if (opt_.mode == "options") {
            OpenOptions(Screen::Menu);
        }
    }

    // Ein Frame (Reihenfolge siehe Dateikopf). dt = Sekunden seit dem letzten
    // Frame, von main() auf höchstens 1/20 s begrenzt; rawDt = dieselbe Zeit
    // ungekürzt (nur für den Mehrspieler, siehe unten). Liefert false, wenn das
    // Spiel beendet werden soll ("Beenden" im Menü oder --quit-after).
    bool Frame(float dt, float rawDt) {
        elapsed_ += dt;
        ui::BeginFrame();
        // F11: randloses Fenster in Bildschirmgröße <-> normales Fenster.
        // (Echtes Vollbild würde die Auflösung des Monitors umschalten.)
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
        // M schaltet überall den Ton um – außer während in einem Textfeld
        // (Mehrspieler: Name, Adresse) getippt wird, dann ist M ein Buchstabe.
        // TextInputActive() kennt den Fokus aus dem letzten Frame, denn die
        // Textfelder melden ihn erst beim Zeichnen (also nach dieser Stelle).
        if (IsKeyPressed(KEY_M) && !ui::TextInputActive()) {
            g_audio.ToggleMute();
            if (screen_ == Screen::Playing) world_.Notify(g_audio.Muted() ? "Ton aus  [M]" : "Ton an  [M]", MUTED);
            if (screen_ == Screen::Multiplayer && mp_) mp_->OnMuteToggled(g_audio.Muted());
        }
        // Mausrad: Kamera näher/weiter. Eine Raste ändert zoom_ um 0,08;
        // 0,55 (nah) .. 1,6 (weit) hält Schiff und Umgebung gut erkennbar.
        // In den Optionen nicht: Dort bleibt das Bild dahinter, wie es ist.
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f && screen_ != Screen::Menu && screen_ != Screen::Options &&
            screen_ != Screen::Multiplayer && screen_ != Screen::Editor)   // Mehrspieler und Editor zoomen selbst
            zoom_ = Clamp(zoom_ - wheel * 0.08f, 0.55f, 1.6f);

        // Zu Fuß ist der Mauszeiger im Spiel eingefangen (Blick mit der Maus),
        // in Pause und Menüs wieder frei (vor dem Lesen der Eingaben).
        const bool capture = screen_ == Screen::Playing && OnFoot() && world_.outcome == Outcome::None;
        if (capture != cursorCaptured_) {
            if (capture) DisableCursor();
            else EnableCursor();
            cursorCaptured_ = capture;
        }

        switch (screen_) {
            case Screen::Menu: UpdateMenuWorld(dt); break;
            case Screen::Roles:
                menuOrbit_ += dt * 0.06f;
                ++roleFrames_;
                break;
            case Screen::Playing: UpdatePlaying(dt); break;
            case Screen::Command: command_.Update(dt); break;
            // Pause: die Welt steht still. pauseFrames_ zählt nur, damit der
            // ESC-Druck, der die Pause geöffnet hat, sie nicht sofort wieder
            // schließt (siehe DrawPause).
            case Screen::Paused: ++pauseFrames_; break;
            // Optionen: aus dem Hauptmenü läuft die Menü-Welt weiter, aus der
            // Pause steht das Spiel still (wie in der Pause selbst).
            case Screen::Options:
                if (optionsReturn_ == Screen::Menu) UpdateMenuWorld(dt);
                break;
            case Screen::Simulation: UpdateSimulation(dt); break;
            case Screen::Multiplayer:
                // Netzwerk, Eingaben und Kamera des Mehrspielers. Will der Spieler
                // zurück ins Hauptmenü, wird das Objekt in BackToMenu zerstört
                // (das trennt die Verbindung).
                // Hier die UNGEKÜRZTE Bildzeit: Die Eingabeuhr im NetClient muss
                // mit der Uhr des Servers Schritt halten (30 Eingaben pro
                // Sekunde, auch bei weniger als 20 Bildern pro Sekunde). Sonst
                // fehlen dem Server Eingaben, und das eigene Schiff wird ständig
                // zurechtgerückt. Gegen zu große Schritte schützt sich NetClient
                // selbst (feste 1/30-s-Schritte, höchstens 0,25 s pro Frame).
                mp_->Update(rawDt);
                if (mp_->WantsMenu()) BackToMenu();
                break;
            case Screen::Editor:
                editor_->Update(dt);
                break;
        }
        UpdateCamera(dt);
        UpdateAudio(dt);
        // --norender: trotzdem BeginDrawing/EndDrawing aufrufen, denn
        // EndDrawing holt in raylib auch neue Tastatur-/Fenster-Ereignisse ab
        // und misst die Frame-Zeit.
        if (opt_.noRender) { BeginDrawing(); ClearBackground(BLACK); EndDrawing(); }
        else Draw();
        HandleTestOptions();
        return !quit_;
    }

    // Beim Schließen des Fensters (Hauptschleife in main() ist beendet).
    // Einzelspieler: automatisch speichern, aber nur ein laufendes Spiel –
    // nach Sieg/Absturz ist der Spielstand schon gelöscht, und während das
    // Schiff abstürzt, ist PlayerAlive() false (dann wird nicht gespeichert).
    // Optionen über der Pause zählen wie die Pause (das Spiel läuft noch).
    // Zum Schluss die Einstellungsdatei, falls in den Optionen etwas geändert
    // und noch nicht gespeichert wurde (z. B. Fenster geschlossen, während
    // der Lautstärkeregler noch gezogen wurde).
    void OnExit() {
        mp_.reset();   // Mehrspieler: beim Server abmelden (er speichert den Fortschritt)
        const bool inGame = screen_ == Screen::Playing || screen_ == Screen::Paused || screen_ == Screen::Command ||
                            (screen_ == Screen::Options && optionsReturn_ == Screen::Paused);
        if (inGame && world_.outcome == Outcome::None && world_.PlayerAlive()) {
            // Ein laufender SG-Einsatz wird verworfen (der Spielstand kennt nur
            // die Rolle und die beendeten Einsätze)
            world_.EndSgMission(world_.Me(), false, false);
            SaveGame(world_);
        }
        g_clientConfig.SaveIfChanged();
    }

private:
    // ========================================================================
    // Bildschirmwechsel: Spiel starten, laden, Simulation, Mehrspieler, Menü
    // ========================================================================

    // Hintergrund des Hauptmenüs: eine neue Zufallswelt im Simulationsmodus
    // (simulation = true: keine Meldungen, kein Speichern), in der der
    // Autopilot fliegt. Der Seed kommt aus der Uhrzeit; das XOR mit 0x5eed
    // sorgt nur dafür, dass er sich von einem gleichzeitig gestarteten
    // "Neuen Spiel" unterscheidet.
    void StartBackground() {
        ResetAutopilot();
        controls_.Reset();
        world_.NewGame(static_cast<unsigned int>(std::time(nullptr)) ^ 0x5eedu, true);
        menuOrbit_ = 0.0f;
        camTarget_ = world_.Player().pos;
        simDock_ = 0.0f;
    }

    // Neues Einzelspiel. World::NewGame ersetzt die ganze Welt (auch die
    // Menü-Hintergrundwelt), legt Stationen, Mond, Lagerstätten, Piraten und
    // Händler an und stellt das eigene Schiff neben die Handelsstation in der
    // Mitte. Danach werden – nur bei Testläufen – Werte aus der Kommandozeile
    // eingesetzt. Zur Begrüßung läuft der Funkspruch clip_newstart.
    // mapStem: eigene Karte aus der Kartenauswahl (Dateistamm, MapFiles.hpp);
    // leer = Zufallskarte wie bisher. Die Karte ersetzt nur die Kulisse.
    void NewGame(const std::string& mapStem = std::string()) {
        unsigned int seed = opt_.seed ? opt_.seed : static_cast<unsigned int>(std::time(nullptr));
        world_.NewGame(seed, false);
        GameMap map;
        if (!mapStem.empty() && LoadMapFile(mapStem, map)) {
            world_.ApplyMap(map);
            world_.mapFile = mapStem;
        }
        world_.Me().cue = cfg::CUE_NEWSTART;
        world_.Me().cueT = cfg::CUE_TIME;
        screen_ = Screen::Playing;
        camTarget_ = world_.Player().pos;
        endHandled_ = false;
        zoom_ = 1.1f;
        controls_.Reset();
        if (opt_.deliver >= 0) { world_.totalDelivered = opt_.deliver; world_.lastStage = world_.StationStage(); }
        if (opt_.hasPos) { world_.Player().pos.x = opt_.posX; world_.Player().pos.z = opt_.posZ; }
        if (opt_.zoom > 0.0f) zoom_ = opt_.zoom;
        if (opt_.scan) { world_.Me().scanOn = true; }
        // Blickrichtung -90°: vorwärts zeigt dann nach +x, im Bild nach rechts
        if (opt_.thrustTest) world_.Player().yaw = -PI * 0.5f;
        if (opt_.hp > 0.0f) world_.Player().hp = opt_.hp;
        if (opt_.balance >= 0) world_.Me().balance = opt_.balance;
        // --pirates: in einer Reihe, 6 Einheiten auseinander, 22 Einheiten
        // oberhalb des Schiffs im Bild (Richtung -z), damit sie sofort angreifen
        for (int i = 0; i < opt_.pirates; ++i) {
            Ship pr = world_.MakeShip(ShipKind::Pirate, Vector3Add(world_.Player().pos, {-20.0f + i * 6.0f, 0, -22.0f}));
            pr.ai = AiState::Wander;
            world_.ships.push_back(pr);
        }
        camTarget_ = world_.Player().pos;
        // Station 0 ist immer die Handelsstation in der Mitte, Station 1 die
        // erste Werft im Ring (Reihenfolge aus World::BuildStations).
        // --mouse-test / --pad-test: erstes Ziel bzw. vorgetäuschter Controller
        if (!opt_.mouseTargets.empty()) {
            mouseTestIdx_ = 0;
            mouseTestEndT_ = -1.0f;
            controls_.SetTestTarget({opt_.mouseTargets[0].x, 0.0f, opt_.mouseTargets[0].y});
            mouseTestLastTgt_ = controls_.Target();
            // Gleich als "hatte ein Ziel" merken: Liegt das erste Ziel genau
            // am Startplatz, ist es schon im ersten Bild erreicht – ohne
            // diese Zeile käme dann nie "ende=" und nie das nächste Ziel.
            mouseTestHad_ = true;
        }
        if (opt_.padTest) controls_.SetTestPad(opt_.padAngle, opt_.padTrigger);
        if (opt_.mode == "dock0") { world_.Player().cargo = 240; world_.Dock(0); }
        if (opt_.mode == "dock1") { world_.Me().balance = 4200; world_.Dock(1); }
        dockFrames_ = 0;
    }

    // "Fortsetzen": Spielstand aus der Datei neben der .exe laden (Save.cpp)
    // und Funkspruch clip_return. Ist die Datei kaputt, wird der Knopf
    // gesperrt und ein Fehlerton kommt.
    void Continue() {
        if (LoadGame(world_)) {
            world_.Me().cue = cfg::CUE_RETURN;
            world_.Me().cueT = cfg::CUE_TIME;
            controls_.Reset();
            screen_ = Screen::Playing;
            camTarget_ = world_.Player().pos;
            endHandled_ = false;
            dockFrames_ = 0;
        } else {
            hasSave_ = false;
            g_audio.Play(Sfx::Denied);
        }
    }

    // Rollenwahl öffnen (nach NewGame bzw. Continue aus dem Hauptmenü; die
    // Testläufe mit --mode starten ohne sie als Pilot). Vorausgewählt ist die
    // Rolle beim letzten Speichern.
    void OpenRoles() {
        screen_ = Screen::Roles;
        roleSel_ = world_.Me().lastRole == PlayerRole::Trooper ? 1 : 0;
        roleFrames_ = 0;
    }

    // Ins Command Center (Rollenwahl SG-Team, nach einem Einsatz, Knopf an
    // der Handelsstation). Die Rolle ist dann schon SG-Team (World::SetRole).
    void OpenCommand() {
        screen_ = Screen::Command;
        command_.Open(world_);
        controls_.Reset();
    }

    // Was im Command Center gewählt wurde (CommandCenterScreen::Draw)
    void HandleCommand(CommandAction a) {
        PlayerState& me = world_.Me();
        if (a == CommandAction::Mission) {
            // Durch das Stargate: Ankunft an einem Tor der Mondkarte
            if (world_.StartSgMission(me, me.sgMates)) {
                screen_ = Screen::Playing;
                if (const Trooper* t = world_.FindTrooper(me.trooperId)) camTarget_ = t->pos;
            } else {
                g_audio.Play(Sfx::Denied);
            }
        } else if (a == CommandAction::Pilot) {
            g_audio.Play(Sfx::UiConfirm);
            world_.SetRole(me, PlayerRole::Pilot);
            screen_ = Screen::Playing;
            camTarget_ = world_.Player().pos;
        } else if (a == CommandAction::SaveAndMenu) {
            SaveGame(world_);
            BackToMenu();
        }
    }

    // Simulation: wie ein neues Spiel, aber der Autopilot fliegt (simulation =
    // true, wird nie gespeichert). Nach Sieg oder Absturz beginnt automatisch
    // eine neue Partie (UpdateSimulation).
    void StartSimulation() {
        ResetAutopilot();
        controls_.Reset();
        unsigned int seed = opt_.seed ? opt_.seed : static_cast<unsigned int>(std::time(nullptr));
        world_.NewGame(seed, true);
        screen_ = Screen::Simulation;
        camTarget_ = world_.Player().pos;
        simDock_ = 0.0f;
        zoom_ = 1.15f;
        simStarted_ = elapsed_;
    }

    // Mehrspieler-Bildschirm öffnen. opt enthält nur bei Testläufen etwas
    // (--mp-connect ...); aus dem Hauptmenü heraus ist es leer.
    void EnterMultiplayer(const MultiplayerOptions& opt) {
        mp_ = std::make_unique<MultiplayerScreen>(opt);
        screen_ = Screen::Multiplayer;
    }

    // Karteneditor öffnen (eigenes Objekt mit eigener Welt, wie der
    // Mehrspieler). "Zurück" aus seiner Kartenliste führt über BackToMenu
    // hierher zurück.
    void EnterEditor() {
        editor_ = std::make_unique<MapEditorScreen>();
        screen_ = Screen::Editor;
    }

    // Optionen öffnen. from = Bildschirm, zu dem "Zurück" führt (Menu oder
    // Paused). Das OptionsScreen-Objekt liest beim Zeichnen selbst die
    // aktuellen Werte aus g_audio; Open setzt nur Auswahl und Zähler zurück.
    void OpenOptions(Screen from) {
        optionsReturn_ = from;
        screen_ = Screen::Options;
        options_.Open();
    }

    // Optionen schließen (OptionsScreen::Draw hat true geliefert; es hat
    // dabei schon gespeichert und den Ton "zurück" gespielt). Zurück in die
    // Pause: pauseFrames_ = 0, damit dieselbe ESC-Taste, die die Optionen
    // geschlossen hat, nicht auch gleich die Pause beendet.
    void CloseOptions() {
        screen_ = optionsReturn_;
        if (screen_ == Screen::Paused) pauseFrames_ = 0;
    }

    // Läuft hinter dem aktuellen Bildschirm die Menü-Welt (kreisende Kamera,
    // Menümusik)? Im Hauptmenü und in den daraus geöffneten Optionen.
    bool MenuBackground() const {
        return screen_ == Screen::Menu || (screen_ == Screen::Options && optionsReturn_ == Screen::Menu);
    }

    // Spieler ist zu Fuß unterwegs (Trupp, über die Pause ausgestiegen)?
    bool OnFoot() const { return world_.Me().role == PlayerRole::Trooper; }

    // Zurück ins Hauptmenü (aus Spiel, Pause, Simulation oder Mehrspieler).
    // Speichert NICHT – das entscheidet der Aufrufer (z. B. DrawPause).
    void BackToMenu() {
        g_audio.Play(Sfx::UiBack);
        editor_.reset();
        pickMap_ = false;
        if (mp_) {
            mp_.reset();                               // trennt die Verbindung
            // Der Server kann einen orangen oder blauen Mond eingestellt haben.
            // (Der Destruktor von MultiplayerScreen setzt das ebenfalls zurück.)
            g_assets.SetMoonColor(MoonColor::Grey);    // Einzelspieler: immer grauer Mond
        }
        hasSave_ = SaveExists();
        screen_ = Screen::Menu;
        menuSel_ = 0;
        StartBackground();
    }

    // ========================================================================
    // Spielzustand weiterrechnen (Einzelspieler, Menü-Hintergrund, Simulation)
    //
    // Der Einzelspieler rechnet mit dem variablen dt des Frames (ein
    // World::Update pro Frame). Der Mehrspieler-Server dagegen rechnet in
    // festen Schritten von 1/30 s (net::TICK_DT) – die Regeln sind dieselben.
    // ========================================================================

    // Gemeinsame Logik für den Autopiloten (Menü-Hintergrund und Simulation).
    // Angedockt "überlegt" der Autopilot 1,6 s: einmal beim Andocken
    // reparieren bzw. aufrüsten (AutopilotDockActions), dann abdocken. In der
    // Zeit laufen nur Effekte und Meldungen weiter (UpdateAmbient), die
    // restliche Welt steht – genau wie bei einem Menschen im Stationsfenster.
    void StepAutopilot(float dt) {
        if (world_.Me().dockedAt >= 0) {
            if (simDock_ == 0.0f) AutopilotDockActions(world_);
            simDock_ += dt;
            world_.UpdateAmbient(dt);
            if (simDock_ > 1.6f) {
                world_.Undock();
                simDock_ = 0.0f;
            }
            return;
        }
        ShipInput in = AutopilotInput(world_, dt);
        world_.Update(dt, in);
    }

    // Hauptmenü: Hintergrundwelt weiterrechnen. Ist das Autopilot-Schiff
    // abgestürzt (oder hat gewonnen), nach 3 s eine neue Welt beginnen.
    // Die Kamera kreist langsam um das Schiff: 0,06 rad/s = eine Runde in
    // etwa 105 s.
    void UpdateMenuWorld(float dt) {
        StepAutopilot(dt);
        if (world_.outcome != Outcome::None && world_.outcomeT > 3.0f) StartBackground();
        menuOrbit_ += dt * 0.06f;
    }

    // Simulation: beliebige Taste/Klick führt zurück ins Menü (erst nach
    // 0,5 s, damit der Klick auf "Simulation" nicht sofort wieder beendet;
    // bei --mode sim gar nicht, Testläufe sollen durchlaufen).
    // --steps N rechnet N Schritte pro Frame (Zeitraffer für lange Tests).
    // Nach Sieg/Absturz läuft die Welt noch 7 s (Endbildschirm sichtbar),
    // dann startet eine neue Partie.
    void UpdateSimulation(float dt) {
        bool leave = AnyKeyOrClick();
        if (leave && elapsed_ - simStarted_ > 0.5f && opt_.mode != "sim") {
            BackToMenu();
            return;
        }
        for (int i = 0; i < opt_.steps; ++i) {
            // Beim Absturz läuft die Welt normal weiter (Piraten fliegen,
            // World::Update zählt outcomeT selbst hoch); nach dem Sieg
            // erstarrt sie und nur die Effekte laufen.
            if (world_.outcome == Outcome::None || world_.outcome == Outcome::Dead) {
                StepAutopilot(dt);
            } else {
                world_.UpdateAmbient(dt);
                world_.outcomeT += dt;
            }
        }
        if (world_.outcome != Outcome::None && world_.outcomeT > 7.0f) StartSimulation();
    }

    // Einzelspiel, ein Frame. Fälle:
    //   - gewonnen: Welt erstarrt, nur Effekte; Spielstand löschen (einmal).
    //   - abgestürzt: Welt läuft ohne Eingaben weiter (Explosion, Piraten);
    //     Spielstand löschen – es gibt kein "Weiterspielen" nach dem Tod.
    //   - angedockt: Welt steht, das Stationsfenster (DrawPlaying) ist offen.
    //     dockFrames_ zählt die Frames seit dem Andocken (siehe DrawPlaying).
    //     Ein Mausziel ist mit dem Andocken erledigt (controls_.Reset).
    //   - fliegen: ESC/P (oder die Menü-Taste des Controllers) öffnet die
    //     Pause, sonst Eingaben lesen (controls_, siehe Controls.hpp) und die
    //     Welt einen Schritt weiterrechnen.
    void UpdatePlaying(float dt) {
        if (world_.outcome == Outcome::Won) {
            controls_.Reset();
            world_.outcomeT += dt;
            world_.UpdateAmbient(dt);
            if (!endHandled_) { DeleteSave(); g_audio.PlayGoalMusic(); endHandled_ = true; }
            return;
        }
        if (world_.outcome == Outcome::Dead) {
            controls_.Reset();
            world_.Update(dt, ShipInput{});
            if (!endHandled_) { DeleteSave(); endHandled_ = true; }
            return;
        }
        if (world_.Me().dockedAt >= 0) {
            controls_.Reset();
            world_.UpdateAmbient(dt);
            ++dockFrames_;
        } else {
            dockFrames_ = 0;
            if (PilotControls::PausePressed()) {
                screen_ = Screen::Paused;
                pauseSel_ = 0;
                pauseFrames_ = 0;
                g_audio.Play(Sfx::PauseOpen);
                return;
            }
            // Zu Fuß (SG-Einsatz): Ego-Steuerung (TrooperControls), das Schiff
            // wartet versteckt. Ist der Einsatz zu Ende (zurück durch das
            // Stargate oder gescheitert), automatisch speichern und zurück ins
            // Command Center.
            if (OnFoot()) {
                const Trooper* t = world_.FindTrooper(world_.Me().trooperId);
                world_.Me().trooperInput = t ? trooperControls_.Read(t->yaw, t->pitch, dt) : TrooperInput{};
                world_.Update(dt, ShipInput{});
                if (world_.Me().trooperId < 0) {
                    if (world_.outcome == Outcome::None) SaveGame(world_);
                    OpenCommand();
                }
                return;
            }
            // Tastatur, Maus und Controller. Scanner und Interagieren kommen
            // getrennt (FlightInput); der Einzelspieler gibt sie als
            // scanToggle/interact in dieselbe ShipInput.
            FlightInput fi = controls_.Read(world_.Player(), cam_, dt);
            // Rechter Stick: Zoom wie das Mausrad (0,08 je Raste, siehe Frame).
            // Nur bei einer Änderung rechnen – ohne Controller bleibt zoom_
            // unberührt (auch ein per --zoom gesetzter Wert außerhalb der Grenzen).
            if (fi.zoom != 0.0f) zoom_ = Clamp(zoom_ - fi.zoom * 0.08f, 0.55f, 1.6f);
            ShipInput in = fi.held;
            in.scanToggle = fi.scan;
            in.interact = fi.interact;
            if (opt_.thrustTest) { in.thrust = 1.0f; in.fire = true; }
            world_.Update(dt, in);
        }
        // Automatisch speichern: World::Dock setzt wantAutosave nach jedem
        // Andocken (an einer Handelsstation also nach jedem Verkauf). Der
        // Wunsch wird hier erfüllt, weil World selbst keine Dateien schreibt.
        if (world_.wantAutosave) {
            world_.wantAutosave = false;
            if (world_.outcome == Outcome::None) SaveGame(world_);
        }
    }

    // ========================================================================
    // Ton
    // ========================================================================
    // Geräusche der Spielwelt abspielen und Dauergeräusche steuern.
    // Die Hintergrund-Simulation im Hauptmenü bleibt stumm.
    //
    // Zwei Arten von Tönen (Umsetzung in src/Audio.cpp):
    //   - Einzelne Effekte: World::Update sammelt sie als SoundEvent in
    //     world_.sounds (mit Ort). PlayAt macht sie leiser, je weiter sie vom
    //     Zuhörer entfernt sind, und verschiebt sie im Stereobild nach links/
    //     rechts. Zuhörer ist das Kameraziel (die Bildmitte), nicht die Kamera.
    //   - Dauergeräusche (Triebwerk, Schub, Scanner, Abbau-Laser) sind
    //     raylib-Music-Streams: Endlosschleifen, die stückweise nachgeladen
    //     werden. LoopState gibt nur an, wie laut jede Schleife gerade sein
    //     soll; g_audio.Update blendet weich dorthin über und muss deshalb
    //     JEDES Frame laufen (auch im Menü, dann mit allem auf 0 = ausblenden).
    //   - Musik: SetMusicTrack wählt die Musik für den aktuellen Bildschirm,
    //     g_audio.Update blendet beim Wechsel über (Audio.cpp, MusicMix.hpp).
    //     Menümusik im Hauptmenü, in den Optionen daraus und in der
    //     Rollenwahl, Spielmusik im Spiel, in der Pause (auch in deren
    //     Optionen), auf den Endbildschirmen und in der Simulation – dort,
    //     solange die eigene Piraten-Mission läuft, die Piratenmusik. Die
    //     Musik "Stargate" im Command Center und im Einsatz rund um das Tor
    //     (StargateMusic). Die Zielmusik stößt UpdatePlaying beim Sieg an
    //     (PlayGoalMusic).
    // ========================================================================
    void UpdateAudio(float dt) {
        if (screen_ == Screen::Multiplayer) {
            // Töne des Mehrspielers kommen vom Server (über NetClient).
            // Die ruhende Menü-Welt erzeugt keine; leeren nur zur Sicherheit.
            world_.sounds.clear();
            mp_->UpdateAudio(dt);
            return;
        }
        LoopState loops;
        if (screen_ == Screen::Playing || screen_ == Screen::Simulation) {
            for (const auto& e : world_.sounds) g_audio.PlayAt(e, camTarget_);
            const Ship& p = world_.Player();
            const PlayerState& me = world_.Me();
            if (world_.PlayerAlive() && !p.hidden && me.dockedAt < 0 && world_.outcome == Outcome::None) {
                loops.engine = 1.0f;
                // Tonhöhe des Triebwerks: 0,9 im Stand bis 1,15 bei Höchstgeschwindigkeit
                // (max(1, ...) schützt vor einer Division durch 0)
                loops.enginePitch = 0.9f + 0.25f * Vector3Length(p.vel) / std::max(1.0f, p.maxSpeed);
                loops.thrust = p.thrustVisual;
                loops.scanner = me.scanOn ? 1.0f : 0.0f;
                loops.mining = me.mining ? 1.0f : 0.0f;
                loops.alarm = p.hp / p.maxHp < 0.3f;   // Warnton unter 30 % Schild
            }
        }
        // Ereignisse sind "einmalig": nach dem Abspielen (oder Verwerfen im
        // Menü/in der Pause) leeren, sonst kämen sie im nächsten Frame wieder.
        world_.sounds.clear();
        MusicTrack track = MusicTrack::Menu;
        if (screen_ == Screen::Command || StargateMusic()) {
            track = MusicTrack::Stargate;
        } else if (!MenuBackground() && screen_ != Screen::Editor && screen_ != Screen::Roles) {
            const Mission* m = world_.FindMission(world_.Me().mission);
            track = m && m->type == MissionType::Pirates ? MusicTrack::Pirates : MusicTrack::Game;
        }
        g_audio.SetMusicTrack(track);
        g_audio.Update(dt, loops);
        // Funkspruch-Clip der Mission (Bild zeichnet Hud.cpp): läuft nur im
        // Flug, hält in der Pause und angedockt an.
        int cue = -1;
        bool showClip = false;
        if (screen_ == Screen::Playing || screen_ == Screen::Paused ||
            (screen_ == Screen::Options && optionsReturn_ == Screen::Paused)) {
            const PlayerState& me = world_.Me();
            if (me.cueT > 0.0f) cue = me.cue;
            showClip = screen_ == Screen::Playing && me.dockedAt < 0;
        }
        g_clip.Update(dt, cue, showClip);
    }

    // Musik "Stargate" im Einsatz: in den ersten 10 s nach der Ankunft und
    // solange ein Stargate für die eigene Rückkehr angewählt wird oder offen ist.
    bool StargateMusic() const {
        if (!OnFoot() || MenuBackground()) return false;
        const PlayerState& me = world_.Me();
        const Trooper* t = world_.FindTrooper(me.trooperId);
        if (!t) return false;
        if (t->age < 10.0f) return true;
        for (const Stargate& g : world_.stargates)
            if (g.toCommand && g.playerId == me.id && (g.state == GateState::Dialing || g.state == GateState::Open)) return true;
        return false;
    }

    // ========================================================================
    // Kamera
    // ========================================================================
    // raylib beschreibt eine 3D-Kamera mit Camera3D:
    //   position  wo die Kamera steht, target  wohin sie schaut,
    //   up        welche Richtung im Bild "oben" ist (hier die Welt-y-Achse),
    //   fovy      senkrechter Blickwinkel in Grad (45° = leichtes Tele, wenig
    //             Verzerrung am Bildrand), projection  perspektivisch.
    // Daraus berechnet BeginMode3D (in Draw) die View- und Projektionsmatrix.
    //
    // Die Kamera folgt dem Schiff nicht starr, sondern gleitet ihm nach:
    // camTarget_ bewegt sich pro Frame um den Anteil k = 1 - e^(-5·dt) auf das
    // Schiff zu. Diese Formel ist unabhängig von der Bildrate (bei 30 und bei
    // 144 Bildern/s gleich schnell); nach 0,2 s sind ~63 % des Abstands
    // aufgeholt. Gezielt wird auf den Boden (y = 0) unter dem Schiff.
    // ========================================================================
    void UpdateCamera(float dt) {
        // eigene Kameras in MultiplayerScreen::Update, MapEditorScreen::Update
        // bzw. im Command Center (CommandCenter.cpp)
        if (screen_ == Screen::Multiplayer || screen_ == Screen::Editor || screen_ == Screen::Command) return;
        const Ship& p = world_.Player();
        // Zu Fuß folgt camTarget_ der Figur (Zuhörer für die Töne, Kamera beim Einsteigen)
        const Trooper* foot = OnFoot() ? world_.FindTrooper(world_.Me().trooperId) : nullptr;
        Vector3 target = foot ? foot->pos : p.pos;
        target.y = 0.0f;
        if (p.dead) target = camTarget_;   // nach der Explosion stehen bleiben
        float k = 1.0f - expf(-5.0f * dt);
        camTarget_ = Vector3Lerp(camTarget_, target, k);

        cam_.up = {0, 1, 0};
        cam_.fovy = 45.0f;
        cam_.projection = CAMERA_PERSPECTIVE;
        if (MenuBackground() || screen_ == Screen::Roles) {
            // Hauptmenü und Rollenwahl: Kreisbahn mit Radius 52 in 34 Einheiten Höhe
            float r = 52.0f;
            cam_.target = camTarget_;
            cam_.position = {camTarget_.x + sinf(menuOrbit_) * r, 34.0f, camTarget_.z + cosf(menuOrbit_) * r};
        } else if (foot) {
            // Zu Fuß: Ego-Kamera in Augenhöhe mit weitem Blickwinkel
            cam_.fovy = 70.0f;
            cam_.position = TrooperEye(*foot);
            cam_.target = Vector3Add(cam_.position, TrooperAim(*foot));
        } else {
            // Spiel: schräg von hinten oben (30 hoch, 27 nach +z, also im Bild
            // "unten"); zoom_ skaliert den Abstand.
            Vector3 off = Vector3Scale({0.0f, 30.0f, 27.0f}, zoom_);
            Vector3 jitter{0, 0, 0};
            // Wackeln bei Treffern: world_.shake (0..1) klingt in der Welt ab.
            // Zwei Sinusschwingungen mit "krummen" Frequenzen (71 und 53)
            // ergeben ein unregelmäßiges Zittern von höchstens 0,5 Einheiten.
            if (world_.shake > 0.0f) {
                float a = world_.shake * 0.5f;
                jitter = {sinf(elapsed_ * 71.0f) * a, 0, cosf(elapsed_ * 53.0f) * a};
            }
            cam_.target = Vector3Add(camTarget_, jitter);
            cam_.position = Vector3Add(Vector3Add(camTarget_, off), jitter);
        }
        if (opt_.hasCam) { cam_.position = opt_.camPos; cam_.target = opt_.camTarget; }
    }

    // ========================================================================
    // Zeichnen
    // ========================================================================
    // raylib zeichnet jedes Frame komplett neu:
    //   BeginDrawing / EndDrawing    Rahmen des Frames; EndDrawing zeigt das
    //                                fertige Bild an (Puffertausch) und wartet
    //                                ggf. auf die Ziel-Bildrate.
    //   ClearBackground              Bild löschen (dunkles Weltraum-Blau).
    //   BeginMode3D / EndMode3D      dazwischen sind Koordinaten Weltkoordinaten
    //                                und werden durch die Kamera projiziert
    //                                (World::Draw in Render.cpp). Danach gilt
    //                                wieder 2D in Pixeln – für HUD und Menüs,
    //                                die so immer über der 3D-Szene liegen.
    // ========================================================================
    void Draw() {
        if (screen_ == Screen::Multiplayer) {
            // Der Mehrspieler zeichnet alles selbst (3D-Welt, HUD, Menüs)
            BeginDrawing();
            ClearBackground(Color{14, 17, 26, 255});
            mp_->Draw();
            ui::DrawPointer();   // Controller-Zeiger über allem (Ui.hpp)
            EndDrawing();
            return;
        }
        if (screen_ == Screen::Editor) {
            // Der Editor zeichnet Karte und Oberfläche selbst (ohne Tilt-Shift,
            // damit beim Bauen nichts unscharf ist)
            BeginDrawing();
            ClearBackground(Color{14, 17, 26, 255});
            editor_->Draw();
            ui::DrawPointer();
            EndDrawing();
            if (editor_->WantsMenu()) BackToMenu();
            return;
        }
        if (screen_ == Screen::Command) {
            // Das Command Center zeichnet seine eigene Kulisse (ohne Tilt-Shift)
            BeginDrawing();
            ClearBackground(Color{8, 10, 14, 255});
            const CommandAction a = command_.Draw(world_);
            ui::DrawPointer();
            EndDrawing();
            HandleCommand(a);
            return;
        }
        BeginDrawing();
        ClearBackground(Color{14, 17, 26, 255});
        // Ego-Ansicht zu Fuß: ohne Tilt-Shift und Retro-Pixel, nahe Clip-Ebene
        // 2 cm (die Waffe steht dicht vor der Kamera); sonst die Werte aus main().
        const bool ego = OnFoot() && !MenuBackground();
        tiltshift::SetEgoView(ego);
        rlSetClipPlanes(ego ? 0.02 : 0.5, ego ? 1000.0 : 2500.0);
        tiltshift::Begin(Color{14, 17, 26, 255});   // Kameraeffekt (TiltShift.hpp)
        BeginMode3D(cam_);
        world_.Draw(cam_);
        // Mausziel: pulsierender Ring am Boden (nur im Einzelspiel, auch in der
        // Pause und in deren Optionen, damit man sieht, wohin es weitergeht)
        if (screen_ == Screen::Playing || screen_ == Screen::Paused || (screen_ == Screen::Options && !MenuBackground()))
            controls_.DrawTarget3D(elapsed_);
        EndMode3D();
        tiltshift::End();

        if (opt_.noHud) { EndDrawing(); return; }
        switch (screen_) {
            case Screen::Menu: DrawMenu(); break;
            case Screen::Roles: DrawRoles(); break;
            case Screen::Playing: DrawPlaying(); break;
            case Screen::Command: break;       // oben behandelt
            case Screen::Paused:
                DrawHud(world_, cam_, false);
                DrawPause();
                break;
            case Screen::Options:
                // Hinter den Optionen: aus der Pause das HUD des angehaltenen
                // Spiels, aus dem Hauptmenü nur die Menü-Welt (schon gezeichnet).
                if (optionsReturn_ == Screen::Paused) DrawHud(world_, cam_, false);
                if (options_.Draw()) CloseOptions();
                break;
            case Screen::Simulation: DrawSimulation(); break;
            case Screen::Multiplayer: break;   // oben behandelt
            case Screen::Editor: break;        // oben behandelt
        }
        ui::DrawPointer();   // Controller-Zeiger über allem (Ui.hpp)
        EndDrawing();
    }

    // Hauptmenü über der kreisenden Hintergrundwelt. Alle Größen werden mit
    // s = Scale() multipliziert (1,0 bei 900 Pixel Fensterhöhe), damit das
    // Menü bei jeder Fenstergröße gleich aussieht.
    void DrawMenu() {
        const float s = Scale();
        const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
        // Links ein Farbverlauf von fast deckend (Alpha 235) zu durchsichtig
        // bis 55 % der Breite: der Text bleibt lesbar, rechts sieht man die Welt.
        DrawRectangleGradientH(0, 0, static_cast<int>(W * 0.55f), static_cast<int>(H), Color{10, 12, 20, 235},
                               Color{10, 12, 20, 0});
        if (pickMap_) { DrawMapPicker(); return; }
        // Sieben Knöpfe: Titel bei 12 % der Höhe, Knöpfe 54 px hoch mit 12 px
        // Abstand. Platzrechnung (alles mal s, und s ist höchstens Höhe / 900):
        // 160 (Titel) + 7 * 66 (Knöpfe) + 24 + 3 * 28 (Erklärtext) = 730 * s,
        // also höchstens 82 % der Höhe; mit den 12 % oben endet der Text bei
        // spätestens 94 % – die Fußzeile beginnt erst bei 95 % (H - 44 * s).
        // So passt es bei jeder Fenstergröße ab 960x540.
        float x = 90 * s;
        float y = H * 0.12f;
        Text("MONDSCHÜRFER", x, y, 80 * s, ACCENT);
        Text("Kristallabbau über der Mondoberfläche", x + 4 * s, y + 92 * s, 26 * s, TEXT);

        // ButtonList zeichnet die Knöpfe, verarbeitet Maus und Pfeiltasten/Enter
        // (menuSel_ = per Tastatur gewählter Knopf) und liefert den Index des
        // gedrückten Knopfs oder -1. "Fortsetzen" ist nur mit Spielstand aktiv.
        std::vector<std::string> items = {"Neues Spiel", "Fortsetzen", "Mehrspieler", "Karteneditor", "Simulation (spielt selbst)",
                                          "Optionen", "Beenden"};
        std::vector<bool> enabled = {true, hasSave_, true, true, true, true, true};
        float bw = 420 * s, bh = 54 * s, gap = 12 * s;
        int choice = ButtonList(items, enabled, menuSel_, x + bw * 0.5f, y + 160 * s, bw, bh, gap, 26 * s);
        if (choice >= 0 && choice <= 5) g_audio.Play(Sfx::UiConfirm);
        switch (choice) {
            case 0:
                // Gibt es eigene Karten, erst die Karte wählen (DrawMapPicker)
                mapList_.Refresh();
                if (mapList_.Maps().empty()) { NewGame(); OpenRoles(); }
                else pickMap_ = true;
                break;
            case 1:
                Continue();
                if (screen_ == Screen::Playing) OpenRoles();
                break;
            case 2: EnterMultiplayer(MultiplayerOptions{}); break;
            case 3: EnterEditor(); break;
            case 4: StartSimulation(); break;
            case 5: OpenOptions(Screen::Menu); break;
            case 6: quit_ = true; break;
            default: break;
        }

        // Kurze Spielerklärung unter den Knöpfen
        float iy = y + 160 * s + static_cast<float>(items.size()) * (bh + gap) + 24 * s;
        const char* info[] = {
            "Scanne den Mond nach Kristallen, baue sie ab und verkaufe sie an der Handelsstation.",
            "Rüste dein Schiff in der Werft auf. Vorsicht vor Piraten!",
            "Ziel: 100.000 Ressourcenpunkte auf dem Konto.",
        };
        for (int i = 0; i < 3; ++i) Text(info[i], x, iy + i * 28 * s, 20 * s, Color{200, 206, 220, 230});

        // Mitwirkende, kurz (ausführlich in README.md, "Lizenzen und Mitwirkende").
        // "·" (Mittelpunkt) ist in der eingebauten Schrift enthalten.
        Text("Grafik und Ton: Kenney (CC0)  ·  Musik: dekker187", x, H - 44 * s, 18 * s, MUTED);
        TextRight(g_audio.Muted() ? "M Ton aus    F11 Vollbild" : "M Ton an    F11 Vollbild", W - 24 * s, H - 44 * s, 18 * s, MUTED);
    }

    // "Neues Spiel", wenn es eigene Karten gibt (Ordner "karten", MapFiles.hpp):
    // links statt der Menüknöpfe eine Liste mit der Standardkarte (Zufall wie
    // bisher) und allen gespeicherten Karten. Doppelklick oder Enter startet
    // wie "Spielen"; ESC bzw. "Zurück" führt zu den Menüknöpfen.
    void DrawMapPicker() {
        const float s = Scale();
        const float H = static_cast<float>(GetScreenHeight());
        const float x = 90 * s, y = H * 0.12f, w = 520 * s;
        Text("NEUES SPIEL", x, y, 56 * s, ACCENT);
        Text("Auf welcher Karte?", x + 4 * s, y + 70 * s, 26 * s, TEXT);
        const float bh = 54 * s, by = H * 0.86f - bh;
        const bool start = mapList_.Draw({x, y + 116 * s, w, by - 20 * s - (y + 116 * s)}, s, "Standardkarte (Zufall)");
        const float gap = 14 * s, bw = (w - gap) * 0.5f;
        const bool play = Button({x, by, bw, bh}, "Spielen", 26 * s, false, mapList_.HasSelection());
        const bool back = Button({x + bw + gap, by, bw, bh}, "Zurück", 26 * s, false);
        if ((start || play) && mapList_.HasSelection()) {
            const int i = mapList_.SelectedMap();
            pickMap_ = false;
            g_audio.Play(Sfx::UiConfirm);
            NewGame(i >= 0 ? mapList_.Maps()[static_cast<size_t>(i)].stem : std::string());
            OpenRoles();
        } else if (back || ui::BackPressed()) {
            pickMap_ = false;
            g_audio.Play(Sfx::UiBack);
        }
    }

    // Rollenwahl über der stehenden neuen Welt: zwei Karten nebeneinander,
    // Pilot und SG-Team. Klick, Pfeiltasten/A/D bzw. Steuerkreuz und Enter
    // bzw. Controller-A; ESC führt zurück ins Hauptmenü (ohne zu speichern).
    // Bedienbar erst zwei Frames nach dem Öffnen (der Klick aus dem Menü).
    void DrawRoles() {
        const float s = Scale();
        const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
        DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 150});
        const bool live = roleFrames_ > 1;
        TextCentered("WIE WILLST DU SPIELEN?", W * 0.5f, H * 0.14f, 52 * s, ACCENT);
        TextCentered("Die Rolle lässt sich später wechseln: im Command Center und an der Handelsstation.", W * 0.5f,
                     H * 0.14f + 66 * s, 20 * s, MUTED);
        struct Card {
            const char* title;
            const char* sub;
            std::string lines[3];
        };
        const Card cards[2] = {
            {"PILOT", "Mit dem Raumschiff",
             {"Kristalle scannen, abbauen und verkaufen,", "das Schiff aufrüsten, Missionen fliegen.",
              "Ziel: 100.000 Ressourcenpunkte."}},
            {"SG-TEAM", "Zu Fuß durch das Stargate",
             {"Start im Command Center mit Team SG1.",
              TextFormat("%d Proben an gelben Kristallen sammeln", cfg::SAMPLE_GOAL),
              "und durch ein Stargate zurückkehren."}},
        };
        const float cw = 460 * s, ch = 250 * s, gap = 40 * s;
        const float y = H * 0.5f - ch * 0.5f;
        int pick = -1;
        for (int i = 0; i < 2; ++i) {
            const Rectangle r{W * 0.5f - cw - gap * 0.5f + i * (cw + gap), y, cw, ch};
            if (Button(r, "", 20 * s, roleSel_ == i) && live) pick = i;
            const Card& c = cards[i];
            Text(c.title, r.x + 28 * s, r.y + 26 * s, 40 * s, roleSel_ == i ? ACCENT : TEXT);
            Text(c.sub, r.x + 28 * s, r.y + 76 * s, 22 * s, MUTED);
            for (int l = 0; l < 3; ++l) Text(c.lines[l], r.x + 28 * s, r.y + 124 * s + l * 30 * s, 20 * s, TEXT);
        }
        if (live) {
            if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT)) roleSel_ = 0;
            if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) roleSel_ = 1;
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) pick = roleSel_;
        }
        TextCentered("Klick oder Pfeiltasten und Enter: wählen    ESC: zurück", W * 0.5f, y + ch + 30 * s, 20 * s, MUTED);
        if (pick == 0) {
            g_audio.Play(Sfx::UiConfirm);
            world_.SetRole(world_.Me(), PlayerRole::Pilot);
            controls_.Reset();
            screen_ = Screen::Playing;
        } else if (pick == 1) {
            g_audio.Play(Sfx::UiConfirm);
            world_.SetRole(world_.Me(), PlayerRole::Trooper);
            OpenCommand();
        } else if (live && BackPressed()) {
            BackToMenu();
        }
    }

    // Einzelspiel: HUD, dazu je nach Lage Endbildschirm oder Stationsfenster.
    // Der Endbildschirm nach einem Absturz erscheint erst nach 1,8 s, damit
    // man die Explosion noch sieht.
    void DrawPlaying() {
        if (world_.outcome == Outcome::Won) {
            DrawHud(world_, cam_, false);
            if (DrawVictory(world_)) BackToMenu();
            return;
        }
        if (world_.outcome == Outcome::Dead) {
            DrawHud(world_, cam_, false);
            if (world_.outcomeT > 1.8f && DrawGameOver(world_)) BackToMenu();
            return;
        }
        DrawHud(world_, cam_, false);
        if (world_.Me().dockedAt >= 0) {
            // Das Stationsfenster meldet nur, was angeklickt wurde; ausgeführt wird es hier
            // (im Mehrspieler würde stattdessen eine DockAction an den Server gehen).
            // Bedienbar erst zwei Frames nach dem Andocken: Der F-Druck, der
            // angedockt hat, gilt im ganzen Frame als "gedrückt" und würde
            // sonst sofort wieder abdocken (F = Abdocken im Fenster).
            DockRequest req = DrawDockPanel(world_, dockFrames_ > 1, true);
            if (req.repair) world_.Repair();
            if (req.buy >= 0) world_.BuyUpgrade(req.buy);
            if (req.buyShip >= 0) world_.BuyShip(req.buyShip, req.tradeIn);
            if (req.switchShip >= 0) world_.SwitchShip(req.switchShip);
            if (req.acceptMission >= 0) world_.AcceptMission(req.acceptMission);
            if (req.undock) {
                world_.Undock();
                if (world_.outcome == Outcome::None) SaveGame(world_);
            }
            // Handelsstation: "Als SG-Team spielen" (World::SetRole dockt ab)
            if (req.toTeam) {
                g_audio.Play(Sfx::UiConfirm);
                world_.SetRole(world_.Me(), PlayerRole::Trooper);
                OpenCommand();
            }
        }
    }

    // Pausenmenü über dem angehaltenen Spiel (halbdurchsichtig abgedunkelt).
    // ESC/P schließt die Pause wieder (am Controller B oder die Menü-Taste,
    // BackPressed), aber erst zwei Frames nach dem Öffnen (siehe
    // pauseFrames_ in Frame), sonst würde derselbe Tastendruck sie öffnen und
    // gleich wieder schließen.
    // Aufbau von oben: Überschrift, die Knöpfe, darunter die Tastenhilfe.
    // Im SG-Einsatz gibt es den zweiten Knopf "Einsatz abbrechen": Er zählt
    // als gescheitert (Scoreboard) und führt ins Command Center.
    // "Speichern & zum Hauptmenü" verwirft einen laufenden Einsatz (der
    // Spielstand kennt nur die Rolle und die beendeten Einsätze).
    // Die Höhe des Fensters ergibt sich aus diesen Teilen (ControlsMaxHeight
    // aus Hud.hpp: Platz für die längste Tastenhilfe), damit Knöpfe und
    // Tastenhilfe sich nie überlappen und die Knöpfe beim Gerätewechsel
    // nicht verrutschen.
    void DrawPause() {
        const float s = Scale();
        const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
        DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 150});
        const bool foot = OnFoot();
        std::vector<std::string> items = {"Weiterspielen"};
        if (foot) items.push_back("Einsatz abbrechen");
        items.insert(items.end(), {"Optionen", "Speichern & zum Hauptmenü", "Hauptmenü ohne Speichern"});
        const float bh = 54 * s, gap = 12 * s, listTop = 100 * s;   // Knöpfe (listTop relativ zur Oberkante)
        const float helpSize = 18 * s;
        // Tastenhilfe 26 px unter dem letzten Knopf: n Knöpfe belegen n * (bh + gap) - gap
        const float helpTop = listTop + static_cast<float>(items.size()) * (bh + gap) - gap + 26 * s;
        const float ph = helpTop + ControlsMaxHeight(helpSize) + 16 * s;
        Rectangle r{W * 0.5f - 300 * s, H * 0.5f - ph * 0.5f, 600 * s, ph};
        Panel(r);
        TextCentered("PAUSE", W * 0.5f, r.y + 26 * s, 50 * s, ACCENT);
        int c = ButtonList(items, {}, pauseSel_, W * 0.5f, r.y + listTop, 440 * s, bh, gap, 24 * s);
        if (pauseFrames_ > 1 && (BackPressed() || IsKeyPressed(KEY_P))) c = 0;
        if (!foot && c > 0) ++c;   // ohne "Einsatz abbrechen": dieselben Nummern wie im Einsatz
        if (c == 0) {
            screen_ = Screen::Playing;
            g_audio.Play(Sfx::PauseClose);
        } else if (c == 1) {
            g_audio.Play(Sfx::UiConfirm);
            world_.EndSgMission(world_.Me(), false);
            if (world_.outcome == Outcome::None) SaveGame(world_);
            OpenCommand();
            return;
        } else if (c == 2) {
            g_audio.Play(Sfx::UiConfirm);
            OpenOptions(Screen::Paused);
            return;
        } else if (c == 3) { world_.EndSgMission(world_.Me(), false, false); SaveGame(world_); BackToMenu(); return; }
        else if (c == 4) BackToMenu();
        DrawControls(r.x + 70 * s, r.y + helpTop, helpSize, OnFoot());
    }

    // Simulation: HUD im Simulationsmodus, Stationsfenster nur zum Ansehen
    // (interactive = false, der Autopilot entscheidet), oben ein Hinweisbalken.
    // Die Endbildschirme werden nur gezeigt; ihr Rückgabewert ("zurück zum
    // Menü") wird ignoriert, weil UpdateSimulation selbst neu startet.
    void DrawSimulation() {
        const float s = Scale();
        const float W = static_cast<float>(GetScreenWidth());
        DrawHud(world_, cam_, true);
        if (world_.Me().dockedAt >= 0 && world_.outcome == Outcome::None) DrawDockPanel(world_, false);
        DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(52 * s), Color{10, 12, 20, 200});
        TextCentered("SIMULATION  -  der Autopilot spielt.  Beliebige Taste: zurück zum Menü", W * 0.5f, 15 * s, 22 * s, ACCENT);
        if (world_.outcome == Outcome::Won) DrawVictory(world_);
        else if (world_.outcome == Outcome::Dead && world_.outcomeT > 1.8f) DrawGameOver(world_);
    }

    // ========================================================================
    // Test-Optionen (nach dem Zeichnen, damit Fotos das fertige Bild zeigen)
    // ========================================================================
    // TakeScreenshot liest das zuletzt gezeichnete Bild aus der Grafikkarte
    // und speichert es als PNG. Der Bericht (--report) geht nach stderr, damit
    // Testskripte ihn getrennt von anderen Ausgaben mitlesen können; fflush
    // sorgt dafür, dass die Zeile sofort erscheint und nicht im Puffer wartet.
    void HandleTestOptions() {
        if (opt_.musicTest) MusicTest();
        if (!opt_.mouseTargets.empty()) MouseTest();
        if (opt_.padTest) PadTest();
        if (!opt_.screenshot.empty() && opt_.shotAt >= 0.0f && elapsed_ >= opt_.shotAt && !shotTaken_) {
            TakeScreenshot(opt_.screenshot.c_str());
            shotTaken_ = true;
        }
        if (opt_.shotEvery > 0.0f && elapsed_ - lastShot_ >= opt_.shotEvery) {
            lastShot_ = elapsed_;
            TakeScreenshot(TextFormat("seq_%03d.png", shotIndex_++));
        }
        if (opt_.report && elapsed_ - lastReport_ >= 10.0f && screen_ == Screen::Multiplayer && mp_) {
            // Mehrspieler-Testlauf: Zustand des Mehrspielers statt der Menü-Welt
            lastReport_ = elapsed_;
            fprintf(stderr, "%s\n", mp_->Report().c_str());
            fflush(stderr);
        } else if (opt_.report && elapsed_ - lastReport_ >= 10.0f) {
            // Einzelspieler/Simulation: Zeit, Spielzeit, Kontostand, Gesamtablieferung,
            // Ausbaustufe, Aufrüstungen (Antrieb, Panzerung, Waffen, Lager), Schild,
            // Fracht, Abschüsse, lebende Piraten, Lagerstätten (davon entdeckt),
            // Wracks, Ergebnis (0 läuft, 1 abgestürzt, 2 gewonnen)
            lastReport_ = elapsed_;
            const Ship& p = world_.Player();
            int pirates = 0;
            for (const auto& sh : world_.ships) if (sh.faction == Faction::Pirate && !sh.dead) ++pirates;
            fprintf(stderr, "REPORT t=%.0f play=%.0f bal=%lld deliv=%lld stage=%d upg=%d%d%d%d%d hp=%.0f/%.0f cargo=%d/%d kills=%d pirates=%d deps=%d revealed=%d wrecks=%d outcome=%d\n",
                     elapsed_, world_.Me().playTime, world_.Me().balance, world_.totalDelivered, world_.StationStage(),
                     world_.Me().upgrades[0], world_.Me().upgrades[1], world_.Me().upgrades[2], world_.Me().upgrades[3], world_.Me().upgrades[4], p.hp,
                     p.maxHp, p.cargo, p.cargoMax, world_.Me().kills, pirates, static_cast<int>(world_.deposits.size()),
                     static_cast<int>(std::count_if(world_.deposits.begin(), world_.deposits.end(), [](const Deposit& d) { return d.revealed; })),
                     static_cast<int>(world_.wrecks.size()), static_cast<int>(world_.outcome));
            fflush(stderr);
        }
        if (opt_.quitAfter > 0.0f && elapsed_ >= opt_.quitAfter) quit_ = true;
    }

    // --music-test (nur für Tests, siehe TestOptions): fester Ablauf, der
    // jeden Musik-Fall einmal auslöst. Die Zeitpunkte liegen so weit
    // auseinander, dass jede Blende (Einblenden 1,5 s, Ausblenden 1 s)
    // fertig ist, bevor die nächste Aktion kommt:
    //    3 s  neues Spiel           -> Menümusik aus-, Spielmusik einblenden
    //    6 s  Optionen: Musik aus   -> Spielmusik aus, beide pausiert
    //    8 s  Optionen: Musik an    -> Spielmusik setzt an derselben Stelle fort
    //   10 s  Optionen: 20 %        -> Lautstärke sofort niedriger
    //   11 s  zurück ins Hauptmenü  -> Menümusik setzt fort, wo sie bei 3 s war
    //   13 s  Optionen öffnen       -> Menümusik läuft weiter (kein Wechsel)
    // Die Aktionen gehen über dieselben Funktionen wie ein Spieler
    // (OptionsScreen::SetMusicOn/SetVolume, NewGame, BackToMenu, OpenOptions).
    // Alle 0,25 s eine Zeile "MUSICTEST t=... <Audio::MusicReport>".
    void MusicTest() {
        static const float kAt[] = {3.0f, 6.0f, 8.0f, 10.0f, 11.0f, 13.0f};
        static const char* kWhat[] = {"neues Spiel", "Musik aus", "Musik an", "Lautstärke 20 %", "Hauptmenü", "Optionen"};
        constexpr int kSteps = static_cast<int>(sizeof(kAt) / sizeof(kAt[0]));
        while (musicTestStep_ < kSteps && elapsed_ >= kAt[musicTestStep_]) {
            fprintf(stderr, "MUSICTEST t=%.2f aktion=\"%s\"\n", elapsed_, kWhat[musicTestStep_]);
            switch (musicTestStep_) {
                case 0: NewGame(); break;
                case 1: options_.SetMusicOn(false); break;
                case 2: options_.SetMusicOn(true); break;
                case 3: options_.SetVolume(0.2f, true); break;
                case 4: BackToMenu(); break;
                case 5: OpenOptions(Screen::Menu); break;
                default: break;
            }
            ++musicTestStep_;
        }
        if (elapsed_ - lastMusicReport_ >= 0.25f) {
            lastMusicReport_ = elapsed_;
            fprintf(stderr, "MUSICTEST t=%.2f screen=%d %s\n", elapsed_, static_cast<int>(screen_), g_audio.MusicReport().c_str());
            fflush(stderr);
        }
    }

    // --mouse-test (nur für Tests, siehe TestOptions): Die Ziele werden wie
    // Mausklicks gesetzt (PilotControls::SetTestTarget) und von derselben
    // Steuerung angeflogen wie im Spiel. Ausgabe nach stderr:
    //   - bei jedem Zielende:  MOUSETEST ... ende=angekommen|aufgegeben|...
    //   - alle 2 s:            MOUSETEST t=.. ziel=Nr x=.. z=.. (Schiff)
    //                          tx=.. tz=.. (Ziel) dist=.. (Abstand am Boden)
    //                          speed=.. target=1 (Ziel aktiv) oder 0
    // tx/tz ist das gerade aktive Ziel – auch eins, das ein echter Mausklick
    // gesetzt hat (so prüft ein UI-Test, welche Klicks ein Flugziel setzen);
    // ohne aktives Ziel das zuletzt aktive (daran sieht man, ob das Schiff
    // dort stehen bleibt).
    // 4 s nach einem Zielende kommt das nächste Ziel der Liste.
    // Die REPORT-Zeile (--report) bleibt davon unberührt.
    void MouseTest() {
        if (screen_ != Screen::Playing) return;
        const Ship& p = world_.Player();
        const bool has = controls_.HasTarget();
        if (has) mouseTestLastTgt_ = controls_.Target();
        const Vector3 tgt = mouseTestLastTgt_;
        auto line = [&](const char* extra) {
            fprintf(stderr, "MOUSETEST t=%.2f ziel=%d x=%.2f z=%.2f tx=%.2f tz=%.2f dist=%.2f speed=%.2f target=%d%s\n", elapsed_,
                    static_cast<int>(mouseTestIdx_), p.pos.x, p.pos.z, tgt.x, tgt.z, Dist2D(p.pos, tgt), Vector3Length(p.vel),
                    has ? 1 : 0, extra);
            fflush(stderr);
        };
        if (mouseTestHad_ && !has) {
            mouseTestEndT_ = elapsed_;
            line(TextFormat(" ende=%s", controls_.LastEnd()));
        }
        mouseTestHad_ = has;
        if (!has && mouseTestEndT_ >= 0.0f && elapsed_ - mouseTestEndT_ >= 4.0f && mouseTestIdx_ + 1 < opt_.mouseTargets.size()) {
            ++mouseTestIdx_;
            mouseTestEndT_ = -1.0f;
            const Vector2 n = opt_.mouseTargets[mouseTestIdx_];
            controls_.SetTestTarget({n.x, 0.0f, n.y});
            mouseTestHad_ = true;
            fprintf(stderr, "MOUSETEST t=%.2f neues_ziel=%d tx=%.2f tz=%.2f\n", elapsed_, static_cast<int>(mouseTestIdx_), n.x, n.y);
        }
        if (elapsed_ - lastMouseReport_ >= 2.0f) {
            lastMouseReport_ = elapsed_;
            line("");
        }
    }

    // --pad-test (nur für Tests): jede Sekunde eine Zeile
    //   PADTEST t=.. soll_bild=WINKEL bild=.. abw=.. yaw=.. speed=.. soll_speed=..
    // bild = Flugrichtung des Schiffs IM BILD in Grad (0 = oben, 90 =
    // rechts; controls::ImageAngleDeg mit der aktuellen Kamera; 360 ist
    // dasselbe wie 0), abw = Abweichung von soll_bild in Grad (kürzester
    // Winkel, AngleDiff), yaw = der Drehwinkel in der Welt (Grad),
    // soll_speed = Trigger * Höchsttempo. Nach dem Eindrehen muss abw ≈ 0
    // sein und speed = soll_speed.
    void PadTest() {
        if (screen_ != Screen::Playing || elapsed_ - lastPadReport_ < 1.0f) return;
        lastPadReport_ = elapsed_;
        const Ship& p = world_.Player();
        const controls::GroundAxes ax = controls::CameraGroundAxes(cam_.position, cam_.target);
        const float img = controls::ImageAngleDeg(p.yaw, ax);
        const float dev = fabsf(AngleDiff(img * DEG2RAD, opt_.padAngle * DEG2RAD)) * RAD2DEG;
        fprintf(stderr, "PADTEST t=%.2f soll_bild=%.1f bild=%.1f abw=%.1f yaw=%.1f speed=%.2f soll_speed=%.2f x=%.1f z=%.1f\n",
                elapsed_, opt_.padAngle, img, dev, p.yaw * RAD2DEG, Vector3Length(p.vel),
                Clamp(opt_.padTrigger, 0.0f, 1.0f) * p.maxSpeed, p.pos.x, p.pos.z);
        fflush(stderr);
    }

    // ========================================================================
    // Zustand
    // ========================================================================
    TestOptions opt_;
    World world_;                             // Einzelspiel, Simulation oder Menü-Hintergrund
    std::unique_ptr<MultiplayerScreen> mp_;   // nur im Mehrspieler vorhanden
    std::unique_ptr<MapEditorScreen> editor_; // nur im Karteneditor vorhanden
    MapListView mapList_;                     // Kartenauswahl bei "Neues Spiel"
    bool pickMap_ = false;                    // Hauptmenü zeigt gerade die Kartenauswahl
    Screen screen_ = Screen::Menu;
    OptionsScreen options_;                   // Optionen aus Hauptmenü und Pause
    Screen optionsReturn_ = Screen::Menu;     // wohin "Zurück" aus den Optionen führt (Menu oder Paused)
    Camera3D cam_{};
    Vector3 camTarget_{};      // geglätteter Blickpunkt am Boden (folgt dem Schiff)
    float zoom_ = 1.1f;        // Faktor für den Kameraabstand (Mausrad)
    float menuOrbit_ = 0.0f;   // Winkel der Menü-Kamera auf ihrer Kreisbahn (rad)
    float elapsed_ = 0.0f;     // Sekunden seit Programmstart (Summe der dt)
    float simDock_ = 0.0f;     // Autopilot: Sekunden seit dem Andocken
    float lastReport_ = 0.0f;
    float simStarted_ = 0.0f;
    float lastShot_ = 0.0f;
    float lastMusicReport_ = 0.0f;   // --music-test: Zeit der letzten MUSICTEST-Zeile
    int musicTestStep_ = 0;          // --music-test: nächster Schritt des Ablaufs
    PilotControls controls_;         // Flugsteuerung: Tastatur, Maus, Controller (Controls.hpp)
    TrooperControls trooperControls_; // Steuerung zu Fuß (Ego-Ansicht)
    bool cursorCaptured_ = false;    // Mauszeiger eingefangen (zu Fuß, siehe Frame)
    size_t mouseTestIdx_ = 0;        // --mouse-test: Nummer des aktuellen Ziels
    float mouseTestEndT_ = -1.0f;    // --mouse-test: Zeit, als das aktuelle Ziel endete (-1 = läuft noch)
    bool mouseTestHad_ = false;      // --mouse-test: hatte im vorigen Frame ein Ziel
    Vector3 mouseTestLastTgt_{};     // --mouse-test: zuletzt aktives Ziel
    float lastMouseReport_ = -10.0f; // --mouse-test: Zeit der letzten MOUSETEST-Zeile
    float lastPadReport_ = -10.0f;   // --pad-test: Zeit der letzten PADTEST-Zeile
    int shotIndex_ = 0;
    int pauseFrames_ = 0;      // Frames seit dem Öffnen der Pause
    int menuSel_ = 0;          // per Tastatur gewählter Knopf im Hauptmenü
    int pauseSel_ = 0;         // ... im Pausenmenü
    int roleSel_ = 0;          // ... in der Rollenwahl (0 Pilot, 1 SG-Team)
    int roleFrames_ = 0;       // Frames seit dem Öffnen der Rollenwahl
    CommandCenterScreen command_;             // Command Center des SG-Teams
    int dockFrames_ = 0;       // Frames seit dem Andocken
    bool hasSave_ = false;     // Spielstand vorhanden -> "Fortsetzen" aktiv
    bool quit_ = false;
    bool endHandled_ = false;  // Spielstand nach Sieg/Absturz schon gelöscht
    bool shotTaken_ = false;
};

} // namespace

// ===========================================================================
// main: Kommandozeile, Fenster, Assets, Hauptschleife
// ===========================================================================
// --dump-model-bounds: Abmessungen aller Modelle in assets/models als Tabelle
// ausgeben, im Format von shared/ModelBounds.inc. Aufruf:
//   Mondschuerfer --dump-model-bounds > shared/ModelBounds.inc
// Nötig, wenn Modelle hinzukommen oder sich ändern: Der Server lädt keine
// Modelle und kennt die Größe der Stationen nur aus dieser Tabelle.
// Die Zahlen werden mit 9 gültigen Stellen geschrieben ("%.9g"). Das reicht,
// damit jeder float exakt wieder eingelesen wird; so rechnen Spiel und Server
// mit genau denselben Werten wie beim Laden der Modelle.
static void DumpModelBounds() {
    FilePathList files = LoadDirectoryFilesEx((g_assets.AssetDir() + "models").c_str(), ".glb", false);
    std::vector<std::string> names;
    for (unsigned i = 0; i < files.count; ++i) names.push_back(GetFileNameWithoutExt(files.paths[i]));
    UnloadDirectoryFiles(files);
    std::sort(names.begin(), names.end());
    auto num = [](float v) {
        std::string s = TextFormat("%.9g", v);
        if (s.find_first_of(".e") == std::string::npos) s += ".0";   // "0" -> "0.0f" (sonst kein float-Literal)
        return s + "f";
    };
    printf("// Automatisch erzeugt: Abmessungen (BoundingBox) aller Modelle des Kenney Space Kit.\n");
    printf("// Erzeugen mit:  Mondschuerfer --dump-model-bounds > shared/ModelBounds.inc\n");
    printf("// Format: {Dateiname ohne .glb, {min x, y, z}, {max x, y, z}} im Kit-Raster (Ursprung korrigiert).\n");
    for (const auto& n : names) {
        const BoundingBox b = g_assets.Bounds(g_assets.Id(n));
        printf("    {\"%s\", {%s, %s, %s}, {%s, %s, %s}},\n", n.c_str(), num(b.min.x).c_str(), num(b.min.y).c_str(),
               num(b.min.z).c_str(), num(b.max.x).c_str(), num(b.max.y).c_str(), num(b.max.z).c_str());
    }
}

int main(int argc, char** argv) {
    // Kommandozeile auswerten. Alle Optionen sind für automatische Tests;
    // ohne Optionen startet das normale Spiel im Hauptmenü. next() holt den
    // Wert nach einer Option (oder "", wenn er fehlt) und rückt i weiter.
    TestOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--screenshot") opt.screenshot = next();
        else if (a == "--shot-at") opt.shotAt = static_cast<float>(atof(next().c_str()));
        else if (a == "--quit-after") opt.quitAfter = static_cast<float>(atof(next().c_str()));
        else if (a == "--mode") opt.mode = next();
        else if (a == "--steps") opt.steps = std::max(1, atoi(next().c_str()));
        else if (a == "--seed") opt.seed = static_cast<unsigned int>(atoi(next().c_str()));
        else if (a == "--report") opt.report = true;
        else if (a == "--deliver") opt.deliver = atoll(next().c_str());
        else if (a == "--pos") { opt.posX = static_cast<float>(atof(next().c_str())); opt.posZ = static_cast<float>(atof(next().c_str())); opt.hasPos = true; }
        else if (a == "--zoom") opt.zoom = static_cast<float>(atof(next().c_str()));
        else if (a == "--scan") opt.scan = true;
        else if (a == "--nohud") opt.noHud = true;
        else if (a == "--norender") opt.noRender = true;
        else if (a == "--shot-every") opt.shotEvery = static_cast<float>(atof(next().c_str()));
        else if (a == "--thrust-test") opt.thrustTest = true;
        else if (a == "--pirates") opt.pirates = atoi(next().c_str());
        else if (a == "--hp") opt.hp = static_cast<float>(atof(next().c_str()));
        else if (a == "--balance") opt.balance = atoll(next().c_str());
        else if (a == "--music-test") opt.musicTest = true;
        else if (a == "--mouse-test") {
            const float x = static_cast<float>(atof(next().c_str()));
            const float z = static_cast<float>(atof(next().c_str()));
            opt.mouseTargets.push_back({x, z});
        }
        else if (a == "--pad-test") {
            opt.padTest = true;
            opt.padAngle = static_cast<float>(atof(next().c_str()));
            opt.padTrigger = static_cast<float>(atof(next().c_str()));
        }
        else if (a == "--pad-name") opt.padName = next();
        else if (a == "--dump-model-bounds") opt.dumpModelBounds = true;
        else if (a == "--mp-connect") opt.mp.connect = next();
        else if (a == "--mp-name") opt.mp.name = next();
        else if (a == "--mp-pass") opt.mp.password = next();
        else if (a == "--mp-register") opt.mp.registerAccount = true;
        else if (a == "--mp-spectate") opt.mp.spectate = true;
        else if (a == "--mp-autopilot") opt.mp.autopilot = true;
        else if (a == "--size") { opt.winW = std::max(960, atoi(next().c_str())); opt.winH = std::max(540, atoi(next().c_str())); }
        else if (a == "--cam") {
            opt.hasCam = true;
            opt.camPos.x = static_cast<float>(atof(next().c_str())); opt.camPos.y = static_cast<float>(atof(next().c_str())); opt.camPos.z = static_cast<float>(atof(next().c_str()));
            opt.camTarget.x = static_cast<float>(atof(next().c_str())); opt.camTarget.y = static_cast<float>(atof(next().c_str())); opt.camTarget.z = static_cast<float>(atof(next().c_str()));
        }
    }

    // Fenster öffnen. Die Config-Flags müssen VOR InitWindow gesetzt werden,
    // weil sie beim Anlegen des OpenGL-Kontexts gebraucht werden. "HINT"
    // heißt: Wunsch an Grafiktreiber/Betriebssystem, der ignoriert werden darf.
    //   MSAA_4X    Kantenglättung (4 Abtastpunkte pro Pixel)
    //   VSYNC      Bildwechsel im Takt des Monitors (kein "Tearing")
    //   RESIZABLE  Fenstergröße frei änderbar (mindestens 960x540, darauf
    //              ist die Oberfläche ausgelegt)
    // SetTraceLogLevel(LOG_ERROR): raylib schreibt sonst viele Info-Zeilen.
    SetTraceLogLevel(LOG_ERROR);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(opt.winW, opt.winH, "Mondschürfer - Kenney Space Kit");
    SetGamepadMappings("030000005e040000120b000009050000,Xbox Series Controller,a:b0,b:b1,x:b2,y:b3,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b10,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5,platform:Linux,");
    SetWindowMinSize(960, 540);
    // raylib schließt das Fenster standardmäßig bei ESC. Das Spiel braucht
    // ESC aber für Pause und Abdocken, also keine "Beenden-Taste".
    SetExitKey(KEY_NULL);
    SetTargetFPS(144);
    // Nahe und ferne Clip-Ebene der Kamera (raylib-Standard: 0,01 und 1000).
    // Der Tiefenpuffer verteilt seine Genauigkeit sehr ungleich: das meiste
    // davon liegt dicht an der nahen Ebene. Mit 0,5 statt 0,01 gibt es kein
    // Flimmern sich überlappender Flächen ("Z-Fighting") mehr; 2500 reicht
    // auch für die Zuschauer-Kamera hoch über der ganzen Karte.
    rlSetClipPlanes(0.5, 2500.0); // bessere Tiefengenauigkeit

    // Modelle, Texturen und Shader laden (braucht den OpenGL-Kontext, also
    // nach InitWindow). Fehlt der Ordner "assets", statt abzustürzen einen
    // Hinweis im Fenster zeigen, bis es geschlossen wird.
    if (!g_assets.Init()) {
        while (!WindowShouldClose()) {
            BeginDrawing();
            ClearBackground(Color{20, 22, 30, 255});
            DrawText("Assets nicht gefunden!", 40, 40, 40, RED);
            DrawText("Der Ordner 'assets' (mit models/*.glb aus dem Kenney Space Kit)", 40, 100, 20, RAYWHITE);
            DrawText("muss neben der ausführbaren Datei liegen.", 40, 130, 20, RAYWHITE);
            EndDrawing();
        }
        CloseWindow();
        return 1;
    }

    if (opt.dumpModelBounds) {
        DumpModelBounds();
        g_assets.Shutdown();
        CloseWindow();
        return 0;
    }

    // Einstellungsdatei neben der .exe lesen (Serveradresse, Spielername,
    // Musik). Fehlt sie: Standardwerte (Musik an, Lautstärke 50 %).
    g_clientConfig.Load();
    g_audio.Init(g_assets.AssetDir()); // ohne Audiogerät läuft das Spiel stumm weiter
    g_clip.Init(g_assets.AssetDir());  // Funkspruch-Clips der Missionen (assets/clips)
    g_clip.SetVolume(g_clientConfig.clipVolume);
    // Musik-Einstellungen aus der Datei übernehmen. Auch ohne Audiogerät,
    // damit die Optionen die richtigen Werte anzeigen.
    g_audio.SetMusicEnabled(g_clientConfig.musicEnabled);
    g_audio.SetMusicVolume(g_clientConfig.musicVolume);

    // --pad-test / --pad-name: Controller vortäuschen (Hilfetexte zeigen
    // dann die Controller-Belegung, siehe ui::SetTestGamepad)
    if (opt.padTest || !opt.padName.empty()) ui::SetTestGamepad(opt.padName.empty() ? "Xbox Controller" : opt.padName.c_str());

    // --norender: so schnell wie möglich rechnen (0 = keine Bildraten-Grenze)
    if (opt.noRender) SetTargetFPS(0);
    App app(opt);
    app.Start();
    // Hauptschleife. WindowShouldClose wird true, wenn das Fenster
    // geschlossen wird (Kreuz, Alt+F4). GetFrameTime = Dauer des letzten
    // Frames; begrenzt auf 1/20 s, damit ein Ruckler (Fenster verschoben,
    // Laden) nicht einen riesigen Simulationsschritt erzeugt, in dem Schiffe
    // durch Hindernisse "tunneln" oder Geschosse übersprungen werden.
    while (!WindowShouldClose()) {
        float rawDt = GetFrameTime();
        float dt = std::min(rawDt, 1.0f / 20.0f);
        if (!opt.screenshot.empty() || opt.report || opt.shotEvery > 0.0f || !opt.mouseTargets.empty() || opt.padTest)
            dt = rawDt = 1.0f / 60.0f; // deterministisch für Tests
        if (!app.Frame(dt, rawDt)) break;
    }
    // Aufräumen in umgekehrter Reihenfolge des Ladens: erst speichern bzw.
    // beim Server abmelden, dann Ton, Grafik und zuletzt das Fenster
    // (Texturen/Shader können nur freigegeben werden, solange es den
    // OpenGL-Kontext noch gibt).
    app.OnExit();
    g_clip.Shutdown();
    g_audio.Shutdown();
    tiltshift::Shutdown();
    fx::Shutdown();
    g_assets.Shutdown();
    CloseWindow();
    return 0;
}
