// Mondschürfer – ein kleines Weltraum-Bergbau-Spiel mit dem Kenney Space Kit.
//
// Steuerung: W/S Schub, A/D drehen, Leertaste schießen, E Scanner, F abbauen/andocken,
//            Mausrad Zoom, ESC Pause, M Ton an/aus, F11 Vollbild.
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "Assets.hpp"
#include "Audio.hpp"
#include "Hud.hpp"
#include "Save.hpp"
#include "Ui.hpp"
#include "World.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

using namespace ui;

namespace {

enum class Screen { Menu, Playing, Paused, Simulation };

// Optionen für automatisierte Tests (Kommandozeile), im normalen Spiel ohne Bedeutung
struct TestOptions {
    std::string screenshot;
    float shotAt = -1.0f;
    float quitAfter = -1.0f;
    std::string mode;
    int steps = 1;
    unsigned int seed = 0;
    bool report = false;
    long long deliver = -1;
    float posX = 0, posZ = 0;
    bool hasPos = false;
    float zoom = 0.0f;
    bool scan = false;
    bool hasCam = false;
    Vector3 camPos{}, camTarget{};
    bool noHud = false;
    bool noRender = false;
    float shotEvery = 0.0f;
    bool thrustTest = false;
    int pirates = 0;
    float hp = -1.0f;
    long long balance = -1;
};

ShipInput ReadPlayerInput() {
    ShipInput in;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) in.thrust += 1.0f;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) in.thrust -= 1.0f;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) in.turn += 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) in.turn -= 1.0f;
    in.fire = IsKeyDown(KEY_SPACE);
    in.scanToggle = IsKeyPressed(KEY_E);
    in.interact = IsKeyPressed(KEY_F);

    // Gamepad (optional)
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

class App {
public:
    explicit App(const TestOptions& opt) : opt_(opt) {}

    void Start() {
        hasSave_ = SaveExists();
        StartBackground();
        if (opt_.mode == "sim") StartSimulation();
        else if (opt_.mode == "game" || opt_.mode == "dock0" || opt_.mode == "dock1" || opt_.mode == "pause") {
            NewGame();
            if (opt_.mode == "pause") screen_ = Screen::Paused;
        } else if (opt_.mode == "continue") {
            Continue();
        }
    }

    bool Frame(float dt) {
        elapsed_ += dt;
        ui::BeginFrame();
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
        if (IsKeyPressed(KEY_M)) {
            g_audio.ToggleMute();
            if (screen_ == Screen::Playing) world_.Notify(g_audio.Muted() ? "Ton aus  [M]" : "Ton an  [M]", MUTED);
        }
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f && screen_ != Screen::Menu) zoom_ = Clamp(zoom_ - wheel * 0.08f, 0.55f, 1.6f);

        switch (screen_) {
            case Screen::Menu: UpdateMenuWorld(dt); break;
            case Screen::Playing: UpdatePlaying(dt); break;
            case Screen::Paused: ++pauseFrames_; break;
            case Screen::Simulation: UpdateSimulation(dt); break;
        }
        UpdateCamera(dt);
        UpdateAudio(dt);
        if (opt_.noRender) { BeginDrawing(); ClearBackground(BLACK); EndDrawing(); }
        else Draw();
        HandleTestOptions();
        return !quit_;
    }

    // Beim Schließen des Fensters automatisch speichern
    void OnExit() {
        if ((screen_ == Screen::Playing || screen_ == Screen::Paused) && world_.outcome == Outcome::None &&
            world_.PlayerAlive())
            SaveGame(world_);
    }

private:
    // ------------------------------------------------------------------------
    void StartBackground() {
        ResetAutopilot();
        world_.NewGame(static_cast<unsigned int>(std::time(nullptr)) ^ 0x5eedu, true);
        menuOrbit_ = 0.0f;
        camTarget_ = world_.Player().pos;
        simDock_ = 0.0f;
    }

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
        if (opt_.thrustTest) world_.Player().yaw = -PI * 0.5f;
        if (opt_.hp > 0.0f) world_.Player().hp = opt_.hp;
        if (opt_.balance >= 0) world_.Me().balance = opt_.balance;
        for (int i = 0; i < opt_.pirates; ++i) {
            Ship pr = world_.MakeShip(ShipKind::Pirate, Vector3Add(world_.Player().pos, {-20.0f + i * 6.0f, 0, -22.0f}));
            pr.ai = AiState::Wander;
            world_.ships.push_back(pr);
        }
        camTarget_ = world_.Player().pos;
        if (opt_.mode == "dock0") { world_.Player().cargo = 240; world_.Dock(0); }
        if (opt_.mode == "dock1") { world_.Me().balance = 4200; world_.Dock(1); }
        dockFrames_ = 0;
    }

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

    void BackToMenu() {
        g_audio.Play(Sfx::UiBack);
        hasSave_ = SaveExists();
        screen_ = Screen::Menu;
        menuSel_ = 0;
        StartBackground();
    }

    // ------------------------------------------------------------------------
    // Gemeinsame Logik für den Autopiloten (Menü-Hintergrund und Simulation)
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

    void UpdateMenuWorld(float dt) {
        StepAutopilot(dt);
        if (world_.outcome != Outcome::None && world_.outcomeT > 3.0f) StartBackground();
        menuOrbit_ += dt * 0.06f;
    }

    void UpdateSimulation(float dt) {
        bool leave = AnyKeyOrClick();
        if (leave && elapsed_ - simStarted_ > 0.5f && opt_.mode != "sim") {
            BackToMenu();
            return;
        }
        for (int i = 0; i < opt_.steps; ++i) {
            if (world_.outcome == Outcome::None || world_.outcome == Outcome::Dead) {
                StepAutopilot(dt);
            } else {
                world_.UpdateAmbient(dt);
                world_.outcomeT += dt;
            }
        }
        if (world_.outcome != Outcome::None && world_.outcomeT > 7.0f) StartSimulation();
    }

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
        if (world_.wantAutosave) {
            world_.wantAutosave = false;
            if (world_.outcome == Outcome::None) SaveGame(world_);
        }
    }

    // ------------------------------------------------------------------------
    // Geräusche der Spielwelt abspielen und Dauergeräusche steuern.
    // Die Hintergrund-Simulation im Hauptmenü bleibt stumm.
    void UpdateAudio(float dt) {
        LoopState loops;
        if (screen_ == Screen::Playing || screen_ == Screen::Simulation) {
            for (const auto& e : world_.sounds) g_audio.PlayAt(e, camTarget_);
            const Ship& p = world_.Player();
            const PlayerState& me = world_.Me();
            if (world_.PlayerAlive() && !p.hidden && me.dockedAt < 0 && world_.outcome == Outcome::None) {
                loops.engine = 1.0f;
                loops.enginePitch = 0.9f + 0.25f * Vector3Length(p.vel) / std::max(1.0f, p.maxSpeed);
                loops.thrust = p.thrustVisual;
                loops.scanner = me.scanOn ? 1.0f : 0.0f;
                loops.mining = me.mining ? 1.0f : 0.0f;
                loops.alarm = p.hp / p.maxHp < 0.3f;
            }
        }
        world_.sounds.clear();
        g_audio.Update(dt, loops);
    }

    // ------------------------------------------------------------------------
    void UpdateCamera(float dt) {
        const Ship& p = world_.Player();
        Vector3 target = p.pos;
        target.y = 0.0f;
        if (p.dead) target = camTarget_;
        float k = 1.0f - expf(-5.0f * dt);
        camTarget_ = Vector3Lerp(camTarget_, target, k);

        cam_.up = {0, 1, 0};
        cam_.fovy = 45.0f;
        cam_.projection = CAMERA_PERSPECTIVE;
        if (screen_ == Screen::Menu) {
            float r = 52.0f;
            cam_.target = camTarget_;
            cam_.position = {camTarget_.x + sinf(menuOrbit_) * r, 34.0f, camTarget_.z + cosf(menuOrbit_) * r};
        } else {
            Vector3 off = Vector3Scale({0.0f, 30.0f, 27.0f}, zoom_);
            Vector3 jitter{0, 0, 0};
            if (world_.shake > 0.0f) {
                float a = world_.shake * 0.5f;
                jitter = {sinf(elapsed_ * 71.0f) * a, 0, cosf(elapsed_ * 53.0f) * a};
            }
            cam_.target = Vector3Add(camTarget_, jitter);
            cam_.position = Vector3Add(Vector3Add(camTarget_, off), jitter);
        }
        if (opt_.hasCam) { cam_.position = opt_.camPos; cam_.target = opt_.camTarget; }
    }

    // ------------------------------------------------------------------------
    void Draw() {
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
        }
        EndDrawing();
    }

    void DrawMenu() {
        const float s = Scale();
        const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
        DrawRectangleGradientH(0, 0, static_cast<int>(W * 0.55f), static_cast<int>(H), Color{10, 12, 20, 235},
                               Color{10, 12, 20, 0});
        float x = 90 * s;
        float y = H * 0.16f;
        Text("MONDSCHÜRFER", x, y, 80 * s, ACCENT);
        Text("Kristallabbau über der Mondoberfläche", x + 4 * s, y + 92 * s, 26 * s, TEXT);

        std::vector<std::string> items = {"Neues Spiel", "Fortsetzen", "Simulation (spielt selbst)", "Beenden"};
        std::vector<bool> enabled = {true, hasSave_, true, true};
        float bw = 420 * s, bh = 60 * s;
        int choice = ButtonList(items, enabled, menuSel_, x + bw * 0.5f, y + 170 * s, bw, bh, 16 * s, 28 * s);
        if (choice >= 0 && choice <= 2) g_audio.Play(Sfx::UiConfirm);
        switch (choice) {
            case 0: NewGame(); break;
            case 1: Continue(); break;
            case 2: StartSimulation(); break;
            case 3: quit_ = true; break;
            default: break;
        }

        float iy = y + 170 * s + 4 * (bh + 16 * s) + 30 * s;
        const char* info[] = {
            "Scanne den Mond nach Kristallen, baue sie ab und verkaufe sie an der Handelsstation.",
            "Rüste dein Schiff in der Werft auf. Vorsicht vor Piraten!",
            "Ziel: 100.000 Ressourcenpunkte auf dem Konto.",
        };
        for (int i = 0; i < 3; ++i) Text(info[i], x, iy + i * 28 * s, 20 * s, Color{200, 206, 220, 230});

        Text("Grafik und Ton: Kenney (CC0)  -  www.kenney.nl", x, H - 44 * s, 18 * s, MUTED);
        TextRight(g_audio.Muted() ? "M Ton aus    F11 Vollbild" : "M Ton an    F11 Vollbild", W - 24 * s, H - 44 * s, 18 * s, MUTED);
    }

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
            DockRequest req = DrawDockPanel(world_, dockFrames_ > 1);
            if (req.repair) world_.Repair();
            if (req.buy >= 0) world_.BuyUpgrade(req.buy);
            if (req.undock) {
                world_.Undock();
                if (world_.outcome == Outcome::None) SaveGame(world_);
            }
        }
    }

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

    // ------------------------------------------------------------------------
    void HandleTestOptions() {
        if (!opt_.screenshot.empty() && opt_.shotAt >= 0.0f && elapsed_ >= opt_.shotAt && !shotTaken_) {
            TakeScreenshot(opt_.screenshot.c_str());
            shotTaken_ = true;
        }
        if (opt_.shotEvery > 0.0f && elapsed_ - lastShot_ >= opt_.shotEvery) {
            lastShot_ = elapsed_;
            TakeScreenshot(TextFormat("seq_%03d.png", shotIndex_++));
        }
        if (opt_.report && elapsed_ - lastReport_ >= 10.0f) {
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

    TestOptions opt_;
    World world_;
    Screen screen_ = Screen::Menu;
    Camera3D cam_{};
    Vector3 camTarget_{};
    float zoom_ = 1.1f;
    float menuOrbit_ = 0.0f;
    float elapsed_ = 0.0f;
    float simDock_ = 0.0f;
    float lastReport_ = 0.0f;
    float simStarted_ = 0.0f;
    float lastShot_ = 0.0f;
    int shotIndex_ = 0;
    int pauseFrames_ = 0;
    int menuSel_ = 0;
    int pauseSel_ = 0;
    int dockFrames_ = 0;
    bool hasSave_ = false;
    bool quit_ = false;
    bool endHandled_ = false;
    bool shotTaken_ = false;
};

} // namespace

int main(int argc, char** argv) {
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
        else if (a == "--cam") {
            opt.hasCam = true;
            opt.camPos.x = static_cast<float>(atof(next().c_str())); opt.camPos.y = static_cast<float>(atof(next().c_str())); opt.camPos.z = static_cast<float>(atof(next().c_str()));
            opt.camTarget.x = static_cast<float>(atof(next().c_str())); opt.camTarget.y = static_cast<float>(atof(next().c_str())); opt.camTarget.z = static_cast<float>(atof(next().c_str()));
        }
    }

    SetTraceLogLevel(LOG_ERROR);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1600, 900, "Mondschürfer - Kenney Space Kit");
    SetWindowMinSize(960, 540);
    SetExitKey(KEY_NULL);
    SetTargetFPS(144);
    rlSetClipPlanes(0.5, 2500.0); // bessere Tiefengenauigkeit

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

    g_audio.Init(g_assets.AssetDir()); // ohne Audiogerät läuft das Spiel stumm weiter

    if (opt.noRender) SetTargetFPS(0);
    App app(opt);
    app.Start();
    while (!WindowShouldClose()) {
        float dt = std::min(GetFrameTime(), 1.0f / 20.0f);
        if (!opt.screenshot.empty() || opt.report || opt.shotEvery > 0.0f) dt = 1.0f / 60.0f; // deterministisch für Tests
        if (!app.Frame(dt)) break;
    }
    app.OnExit();
    g_audio.Shutdown();
    g_assets.Shutdown();
    CloseWindow();
    return 0;
}
