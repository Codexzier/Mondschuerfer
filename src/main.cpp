// ---------------------------------------------------------------------------
// main.cpp – Einstiegspunkt des Spiels Mondschürfer (das Programm mit Fenster).
//
// Mondschürfer ist ein kleines Weltraum-Bergbau-Spiel mit dem Kenney Space Kit:
// Man fliegt über die Mondoberfläche, sucht mit dem Scanner Kristalle, baut sie
// ab, verkauft sie an Handelsstationen und rüstet das Schiff in Werften auf.
// Ziel im Einzelspieler: 100.000 Ressourcenpunkte (cfg::GOAL_POINTS) auf dem Konto.
//
// Steuerung: W/S Schub, A/D drehen, Leertaste schießen, E Scanner, F abbauen/andocken,
//            Mausrad Zoom, ESC Pause, M Ton an/aus, F11 Vollbild.
//            Gamepad: linker Stick fliegen, A oder rechter Trigger schießen,
//            X Scanner, Y abbauen/andocken.
//
// Wofür ist die Datei da?
//   - main() (ganz unten) liest die Kommandozeile (nur für automatische Tests),
//     öffnet das Fenster, lädt Grafik (g_assets) und Ton (g_audio) und läuft
//     dann in der Hauptschleife, bis das Fenster geschlossen wird.
//   - Die Klasse App verwaltet die Bildschirme (enum Screen): Hauptmenü,
//     Einzelspiel, Pause, Simulation und Mehrspieler. Sie entscheidet jedes
//     Frame, was gerechnet, abgespielt und gezeichnet wird.
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
//     src/Ui.hpp (Knöpfe, Texte, Skalierung), src/Audio.hpp (g_audio),
//     src/Assets.hpp (g_assets: Modelle, Shader, Mondfarbe),
//     src/Save.hpp (Spielstand-Datei), src/Multiplayer.hpp.
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
#include "Hud.hpp"
#include "Multiplayer.hpp"
#include "Save.hpp"
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
//   Playing     Einzelspiel (auch angedockt und auf den Endbildschirmen)
//   Paused      Einzelspiel angehalten, Pausenmenü
//   Simulation  der Autopilot spielt eine ganze Partie vor (mit HUD)
//   Multiplayer alles erledigt das MultiplayerScreen-Objekt
enum class Screen { Menu, Playing, Paused, Simulation, Multiplayer };

// Optionen für automatisierte Tests (Kommandozeile), im normalen Spiel ohne Bedeutung.
// Zeiten beziehen sich auf die Spielzeit seit dem Start (App::elapsed_).
struct TestOptions {
    std::string screenshot;        // --screenshot DATEI: ein Bildschirmfoto ...
    float shotAt = -1.0f;          // --shot-at SEK: ... zu diesem Zeitpunkt
    float quitAfter = -1.0f;       // --quit-after SEK: danach beenden
    // --mode: direkt starten mit "sim" (Simulation), "game" (neues Spiel),
    // "dock0" (angedockt an Handelsstation 0 mit 240 Fracht, also Verkauf),
    // "dock1" (angedockt an Werft 1 mit 4.200 Guthaben, also Aufrüsten),
    // "pause" (neues Spiel im Pausenmenü), "continue" (Spielstand laden)
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
    // Mehrspieler (--mp-connect ADRESSE startet direkt im Mehrspieler-Bildschirm,
    // --mp-name/--mp-pass melden automatisch an, --mp-register legt das Konto an,
    // --mp-spectate schaut zu, --mp-autopilot lässt den Autopiloten fliegen)
    MultiplayerOptions mp;
};

// ---------------------------------------------------------------------------
// Tastatur und Gamepad -> ShipInput (nur Einzelspieler; der Mehrspieler liest
// seine Eingaben selbst in MultiplayerScreen::ReadGameInput).
//
// raylib unterscheidet zwei Abfragen:
//   IsKeyDown    true, solange die Taste gehalten wird  -> Schub, Drehen, Feuer
//   IsKeyPressed true nur in dem einen Frame, in dem sie heruntergeht
//                -> Scanner umschalten und Interagieren sollen pro Druck genau
//                   einmal passieren (ShipInput::scanToggle/interact).
// Vorzeichen wie in ShipInput: thrust +1 = vorwärts, turn +1 = links.
// ---------------------------------------------------------------------------
ShipInput ReadPlayerInput() {
    ShipInput in;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) in.thrust += 1.0f;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) in.thrust -= 1.0f;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) in.turn += 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) in.turn -= 1.0f;
    in.fire = IsKeyDown(KEY_SPACE);
    in.scanToggle = IsKeyPressed(KEY_E);
    in.interact = IsKeyPressed(KEY_F);

    // Gamepad (optional). Der Stick liefert -1..1 je Achse; Ausschläge unter
    // 0,2 gelten als "Totzone" (ein Stick in Ruhelage steht nie genau auf 0)
    // und lassen die Tastatur-Werte stehen. Die Achsen werden umgedreht:
    // Stick nach rechts (+x) = rechts drehen (turn negativ), Stick nach oben
    // liefert -y = Schub vorwärts. raylib benennt Knöpfe nach ihrer Lage,
    // nicht nach der Beschriftung: RIGHT_FACE_DOWN = unterer Knopf der
    // rechten Vierergruppe (Xbox: A), _LEFT = X, _UP = Y;
    // RIGHT_TRIGGER_2 = rechter Trigger (der hintere Schalter).
    if (IsGamepadAvailable(0)) {
        float ax = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        float ay = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (fabsf(ax) > 0.2f) in.turn = -ax;
        if (fabsf(ay) > 0.2f) in.thrust = -ay;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) || IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_2)) in.fire = true;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) in.scanToggle = true;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_UP)) in.interact = true;
    }
    return in;
}

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
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f && screen_ != Screen::Menu && screen_ != Screen::Multiplayer)   // Mehrspieler zoomt selbst
            zoom_ = Clamp(zoom_ - wheel * 0.08f, 0.55f, 1.6f);

        switch (screen_) {
            case Screen::Menu: UpdateMenuWorld(dt); break;
            case Screen::Playing: UpdatePlaying(dt); break;
            // Pause: die Welt steht still. pauseFrames_ zählt nur, damit der
            // ESC-Druck, der die Pause geöffnet hat, sie nicht sofort wieder
            // schließt (siehe DrawPause).
            case Screen::Paused: ++pauseFrames_; break;
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
    void OnExit() {
        mp_.reset();   // Mehrspieler: beim Server abmelden (er speichert den Fortschritt)
        if ((screen_ == Screen::Playing || screen_ == Screen::Paused) && world_.outcome == Outcome::None &&
            world_.PlayerAlive())
            SaveGame(world_);
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
        world_.NewGame(static_cast<unsigned int>(std::time(nullptr)) ^ 0x5eedu, true);
        menuOrbit_ = 0.0f;
        camTarget_ = world_.Player().pos;
        simDock_ = 0.0f;
    }

    // Neues Einzelspiel. World::NewGame ersetzt die ganze Welt (auch die
    // Menü-Hintergrundwelt), legt Stationen, Mond, Lagerstätten, Piraten und
    // Händler an und stellt das eigene Schiff neben die Handelsstation in der
    // Mitte. Danach werden – nur bei Testläufen – Werte aus der Kommandozeile
    // eingesetzt.
    void NewGame() {
        unsigned int seed = opt_.seed ? opt_.seed : static_cast<unsigned int>(std::time(nullptr));
        world_.NewGame(seed, false);
        screen_ = Screen::Playing;
        camTarget_ = world_.Player().pos;
        endHandled_ = false;
        zoom_ = 1.1f;
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
        if (opt_.mode == "dock0") { world_.Player().cargo = 240; world_.Dock(0); }
        if (opt_.mode == "dock1") { world_.Me().balance = 4200; world_.Dock(1); }
        dockFrames_ = 0;
    }

    // "Fortsetzen": Spielstand aus der Datei neben der .exe laden (Save.cpp).
    // Ist die Datei kaputt, wird der Knopf gesperrt und ein Fehlerton kommt.
    void Continue() {
        if (LoadGame(world_)) {
            screen_ = Screen::Playing;
            camTarget_ = world_.Player().pos;
            endHandled_ = false;
            dockFrames_ = 0;
        } else {
            hasSave_ = false;
            g_audio.Play(Sfx::Denied);
        }
    }

    // Simulation: wie ein neues Spiel, aber der Autopilot fliegt (simulation =
    // true, wird nie gespeichert). Nach Sieg oder Absturz beginnt automatisch
    // eine neue Partie (UpdateSimulation).
    void StartSimulation() {
        ResetAutopilot();
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

    // Zurück ins Hauptmenü (aus Spiel, Pause, Simulation oder Mehrspieler).
    // Speichert NICHT – das entscheidet der Aufrufer (z. B. DrawPause).
    void BackToMenu() {
        g_audio.Play(Sfx::UiBack);
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
    //   - fliegen: ESC/P öffnet die Pause, sonst Eingaben lesen und die Welt
    //     einen Schritt weiterrechnen.
    void UpdatePlaying(float dt) {
        if (world_.outcome == Outcome::Won) {
            world_.outcomeT += dt;
            world_.UpdateAmbient(dt);
            if (!endHandled_) { DeleteSave(); endHandled_ = true; }
            return;
        }
        if (world_.outcome == Outcome::Dead) {
            world_.Update(dt, ShipInput{});
            if (!endHandled_) { DeleteSave(); endHandled_ = true; }
            return;
        }
        if (world_.Me().dockedAt >= 0) {
            world_.UpdateAmbient(dt);
            ++dockFrames_;
        } else {
            dockFrames_ = 0;
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_P)) {
                screen_ = Screen::Paused;
                pauseSel_ = 0;
                pauseFrames_ = 0;
                g_audio.Play(Sfx::PauseOpen);
                return;
            }
            ShipInput in = ReadPlayerInput();
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
        g_audio.Update(dt, loops);
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
        if (screen_ == Screen::Multiplayer) return;   // eigene Kameras in MultiplayerScreen::Update
        const Ship& p = world_.Player();
        Vector3 target = p.pos;
        target.y = 0.0f;
        if (p.dead) target = camTarget_;   // nach der Explosion stehen bleiben
        float k = 1.0f - expf(-5.0f * dt);
        camTarget_ = Vector3Lerp(camTarget_, target, k);

        cam_.up = {0, 1, 0};
        cam_.fovy = 45.0f;
        cam_.projection = CAMERA_PERSPECTIVE;
        if (screen_ == Screen::Menu) {
            // Hauptmenü: Kreisbahn mit Radius 52 in 34 Einheiten Höhe
            float r = 52.0f;
            cam_.target = camTarget_;
            cam_.position = {camTarget_.x + sinf(menuOrbit_) * r, 34.0f, camTarget_.z + cosf(menuOrbit_) * r};
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
            EndDrawing();
            return;
        }
        BeginDrawing();
        ClearBackground(Color{14, 17, 26, 255});
        BeginMode3D(cam_);
        world_.Draw(cam_);
        EndMode3D();

        if (opt_.noHud) { EndDrawing(); return; }
        switch (screen_) {
            case Screen::Menu: DrawMenu(); break;
            case Screen::Playing: DrawPlaying(); break;
            case Screen::Paused:
                DrawHud(world_, cam_, false);
                DrawPause();
                break;
            case Screen::Simulation: DrawSimulation(); break;
            case Screen::Multiplayer: break;   // oben behandelt
        }
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
        // Fünf Knöpfe: etwas höher anfangen und etwas flacher als früher (vier
        // Knöpfe), damit auch bei 960x540 alles über die Fußzeile passt.
        float x = 90 * s;
        float y = H * 0.12f;
        Text("MONDSCHÜRFER", x, y, 80 * s, ACCENT);
        Text("Kristallabbau über der Mondoberfläche", x + 4 * s, y + 92 * s, 26 * s, TEXT);

        // ButtonList zeichnet die Knöpfe, verarbeitet Maus und Pfeiltasten/Enter
        // (menuSel_ = per Tastatur gewählter Knopf) und liefert den Index des
        // gedrückten Knopfs oder -1. "Fortsetzen" ist nur mit Spielstand aktiv.
        std::vector<std::string> items = {"Neues Spiel", "Fortsetzen", "Mehrspieler", "Simulation (spielt selbst)", "Beenden"};
        std::vector<bool> enabled = {true, hasSave_, true, true, true};
        float bw = 420 * s, bh = 54 * s, gap = 12 * s;
        int choice = ButtonList(items, enabled, menuSel_, x + bw * 0.5f, y + 160 * s, bw, bh, gap, 26 * s);
        if (choice >= 0 && choice <= 3) g_audio.Play(Sfx::UiConfirm);
        switch (choice) {
            case 0: NewGame(); break;
            case 1: Continue(); break;
            case 2: EnterMultiplayer(MultiplayerOptions{}); break;
            case 3: StartSimulation(); break;
            case 4: quit_ = true; break;
            default: break;
        }

        // Kurze Spielerklärung unter den fünf Knöpfen
        float iy = y + 160 * s + 5 * (bh + gap) + 24 * s;
        const char* info[] = {
            "Scanne den Mond nach Kristallen, baue sie ab und verkaufe sie an der Handelsstation.",
            "Rüste dein Schiff in der Werft auf. Vorsicht vor Piraten!",
            "Ziel: 100.000 Ressourcenpunkte auf dem Konto.",
        };
        for (int i = 0; i < 3; ++i) Text(info[i], x, iy + i * 28 * s, 20 * s, Color{200, 206, 220, 230});

        Text("Grafik und Ton: Kenney (CC0)  -  www.kenney.nl", x, H - 44 * s, 18 * s, MUTED);
        TextRight(g_audio.Muted() ? "M Ton aus    F11 Vollbild" : "M Ton an    F11 Vollbild", W - 24 * s, H - 44 * s, 18 * s, MUTED);
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
            DockRequest req = DrawDockPanel(world_, dockFrames_ > 1);
            if (req.repair) world_.Repair();
            if (req.buy >= 0) world_.BuyUpgrade(req.buy);
            if (req.undock) {
                world_.Undock();
                if (world_.outcome == Outcome::None) SaveGame(world_);
            }
        }
    }

    // Pausenmenü über dem angehaltenen Spiel (halbdurchsichtig abgedunkelt).
    // ESC/P schließt die Pause wieder, aber erst zwei Frames nach dem Öffnen
    // (siehe pauseFrames_ in Frame), sonst würde derselbe Tastendruck sie
    // öffnen und gleich wieder schließen.
    void DrawPause() {
        const float s = Scale();
        const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
        DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 150});
        Rectangle r{W * 0.5f - 300 * s, H * 0.5f - 280 * s, 600 * s, 560 * s};
        Panel(r);
        TextCentered("PAUSE", W * 0.5f, r.y + 26 * s, 50 * s, ACCENT);
        std::vector<std::string> items = {"Weiterspielen", "Speichern & zum Hauptmenü", "Hauptmenü ohne Speichern"};
        int c = ButtonList(items, {true, true, true}, pauseSel_, W * 0.5f, r.y + 100 * s, 440 * s, 54 * s, 12 * s, 24 * s);
        if (pauseFrames_ > 1 && (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_P))) c = 0;
        if (c == 0) {
            screen_ = Screen::Playing;
            g_audio.Play(Sfx::PauseClose);
        } else if (c == 1) { SaveGame(world_); BackToMenu(); return; }
        else if (c == 2) BackToMenu();
        DrawControls(r.x + 70 * s, r.y + 320 * s, 18 * s);
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
            fprintf(stderr, "REPORT t=%.0f play=%.0f bal=%lld deliv=%lld stage=%d upg=%d%d%d%d hp=%.0f/%.0f cargo=%d/%d kills=%d pirates=%d deps=%d revealed=%d wrecks=%d outcome=%d\n",
                     elapsed_, world_.Me().playTime, world_.Me().balance, world_.totalDelivered, world_.StationStage(),
                     world_.Me().upgrades[0], world_.Me().upgrades[1], world_.Me().upgrades[2], world_.Me().upgrades[3], p.hp,
                     p.maxHp, p.cargo, p.cargoMax, world_.Me().kills, pirates, static_cast<int>(world_.deposits.size()),
                     static_cast<int>(std::count_if(world_.deposits.begin(), world_.deposits.end(), [](const Deposit& d) { return d.revealed; })),
                     static_cast<int>(world_.wrecks.size()), static_cast<int>(world_.outcome));
            fflush(stderr);
        }
        if (opt_.quitAfter > 0.0f && elapsed_ >= opt_.quitAfter) quit_ = true;
    }

    // ========================================================================
    // Zustand
    // ========================================================================
    TestOptions opt_;
    World world_;                             // Einzelspiel, Simulation oder Menü-Hintergrund
    std::unique_ptr<MultiplayerScreen> mp_;   // nur im Mehrspieler vorhanden
    Screen screen_ = Screen::Menu;
    Camera3D cam_{};
    Vector3 camTarget_{};      // geglätteter Blickpunkt am Boden (folgt dem Schiff)
    float zoom_ = 1.1f;        // Faktor für den Kameraabstand (Mausrad)
    float menuOrbit_ = 0.0f;   // Winkel der Menü-Kamera auf ihrer Kreisbahn (rad)
    float elapsed_ = 0.0f;     // Sekunden seit Programmstart (Summe der dt)
    float simDock_ = 0.0f;     // Autopilot: Sekunden seit dem Andocken
    float lastReport_ = 0.0f;
    float simStarted_ = 0.0f;
    float lastShot_ = 0.0f;
    int shotIndex_ = 0;
    int pauseFrames_ = 0;      // Frames seit dem Öffnen der Pause
    int menuSel_ = 0;          // per Tastatur gewählter Knopf im Hauptmenü
    int pauseSel_ = 0;         // ... im Pausenmenü
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

    g_audio.Init(g_assets.AssetDir()); // ohne Audiogerät läuft das Spiel stumm weiter

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
        if (!opt.screenshot.empty() || opt.report || opt.shotEvery > 0.0f) dt = rawDt = 1.0f / 60.0f; // deterministisch für Tests
        if (!app.Frame(dt, rawDt)) break;
    }
    // Aufräumen in umgekehrter Reihenfolge des Ladens: erst speichern bzw.
    // beim Server abmelden, dann Ton, Grafik und zuletzt das Fenster
    // (Texturen/Shader können nur freigegeben werden, solange es den
    // OpenGL-Kontext noch gibt).
    app.OnExit();
    g_audio.Shutdown();
    g_assets.Shutdown();
    CloseWindow();
    return 0;
}
