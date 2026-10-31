// ---------------------------------------------------------------------------
// CommandCenter.cpp – Bildschirm "Command Center" (Schnittstelle und Ablauf
// siehe CommandCenter.hpp).
//
// Kulisse (Meter, Blick nach -z):
//   - Halle modular-space/room-large (20 x 20 m, 4,25 m hoch, oben offen),
//     um PI gedreht, damit ihre Säule links vorn steht und nicht im Team.
//     Darüber eine dunkle Decke mit Lichtleisten (DrawCube).
//   - In den Wandöffnungen hinten, links und rechts je ein Tor
//     modular-space/gate-door.
//   - Das Heimat-Stargate (gate_complex, 3,6-fach) mit Treppe, davor das
//     Team SG1: der Spieler (Haut 0) und 0 bis 3 Kameraden (Häute 1..3, wie
//     World::SpawnMate) als 1,8 m große Figuren.
//   - Konsolen und Wandbildschirme aus dem Space Station Kit (2-fach).
//   - Licht: die feste Sonnenrichtung des Shaders und vier Punktlichter
//     (Assets::SetFlashes), eins davon blau am Tor, so hell wie es leuchtet.
// Anwahl: Das Team dreht sich zum Tor, das Tor leuchtet auf und öffnet sich
// (dieselbe Zeitfolge wie in der Welt, GateLookAt), dann geht das Team los.
// ---------------------------------------------------------------------------
#include "CommandCenter.hpp"
#include "Assets.hpp"
#include "Audio.hpp"
#include "Effects.hpp"
#include "Ui.hpp"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace ui;

namespace {

constexpr float kGateScale = 3.6f;            // Heimat-Stargate: 3,6 m breit
const Vector3 kGatePos{0.0f, 0.0f, -6.6f};
constexpr float kFigure = 1.8f;               // Größe der Figuren (m)
constexpr float kWalkStart = cfg::GATE_DIAL - 0.3f;   // ab hier geht das Team zum Tor
constexpr float kDialDone = cfg::GATE_DIAL + 0.4f;    // dann beginnt der Einsatz

// Aufstellung vor dem Tor: Spieler, Kamerad 1..3 (x, z)
const Vector2 kSpots[4] = {{-0.5f, -2.3f}, {0.6f, -2.4f}, {-1.6f, -2.8f}, {1.7f, -2.9f}};

// Das Heimat-Stargate als Stargate-Wert (für GateStairTop, GateNormal)
Stargate HomeGate() {
    Stargate g;
    g.pos = kGatePos;
    g.yaw = 0.0f;
    g.scale = kGateScale;
    return g;
}

// Aussehen des Tors zur Anwahlzeit t (-1 = Ruhe)
GateLook HomeLook(float t) {
    if (t < 0.0f) return GateLook{};
    if (t < cfg::GATE_DIAL) return GateLookAt(GateState::Dialing, t);
    return GateLookAt(GateState::Open, t - cfg::GATE_DIAL);
}

} // namespace

// Das zuletzt gewählte Ziel bleibt, solange es die Einsatzkarte noch gibt.
void CommandCenterScreen::Open(World& w) {
    w.BuildStargates();
    moonGate_ = !w.stargates.empty();
    const std::string last = TargetStem();
    targets_.clear();
    for (const MapFileInfo& m : ListMaps())
        if (m.kind == MapKind::Mission) targets_.push_back(m);
    target_ = 0;
    for (size_t i = 0; i < targets_.size(); ++i)
        if (!last.empty() && targets_[i].stem == last) target_ = static_cast<int>(i) + 1;
    hasGate_ = target_ > 0 || moonGate_;
    PlayerState& me = w.Me();
    ended_ = me.sgEnded;
    me.sgEnded = 0;
    me.sgMates = std::clamp(me.sgMates, 0, cfg::SG_MATES_MAX);
    dialT_ = -1.0f;
    frames_ = 0;
    sel_ = 0;
}

void CommandCenterScreen::Update(float dt) {
    time_ += dt;
    ++frames_;
    if (dialT_ >= 0.0f) {
        // Ton beim Öffnen des Wurmlochs (wie in der Welt)
        const float before = dialT_, open = cfg::GATE_DIAL - 0.6f;
        dialT_ += dt;
        if (before < open && dialT_ >= open) g_audio.Play(Sfx::Discover);
    }
}

// Die 3D-Kulisse, ohne Tilt-Shift (eigene Kamera, nahe Clip-Ebene 10 cm).
void CommandCenterScreen::DrawScene(const World& w) {
    Camera3D cam{};
    cam.position = {0.0f, 2.0f, 5.5f};
    cam.target = {0.0f, 1.9f, -7.0f};
    cam.up = {0, 1, 0};
    cam.fovy = 55.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    rlSetClipPlanes(0.1, 100.0);
    BeginMode3D(cam);

    const GateLook look = HomeLook(dialT_);
    // Beleuchtung: keine Schatten, kein Sandsturm, vier Punktlichter
    g_assets.SetCamera(cam.position);
    g_assets.SetShadow(MatrixIdentity(), fx::kShadowSlot, 0.0f, 0.0f);
    fx::SetFog(cam.target, 0.0f, time_);
    g_assets.SetLight({1.0f, 1.0f, 1.0f});
    fx::SetScreenFlash(0.0f);
    const Vector4 lightPos[4] = {{0.0f, 2.1f, -5.6f, 11.0f},
                                 {0.0f, 4.0f, 1.0f, 15.0f},
                                 {-8.0f, 1.6f, -4.5f, 6.0f},
                                 {8.0f, 1.6f, -4.5f, 6.0f}};
    const float g = look.glow;
    const Vector3 lightCol[4] = {{0.05f + 0.35f * g, 0.10f + 0.75f * g, 0.20f + 1.5f * g},
                                 {0.35f, 0.33f, 0.30f},
                                 {0.10f, 0.30f, 0.25f},
                                 {0.10f, 0.30f, 0.25f}};
    g_assets.SetFlashes(lightPos, lightCol, 4);

    // Halle, Decke mit Lichtleisten, Tore in den Wandöffnungen
    static const int room = g_assets.Id("modular-space/room-large");
    static const int door = g_assets.Id("modular-space/gate-door");
    g_assets.Draw(room, Vector3{0, 0, 0}, PI, 1.0f);
    DrawCube({0.0f, 4.55f, 0.0f}, 20.0f, 0.6f, 20.0f, Color{30, 34, 44, 255});
    for (int i = -1; i <= 1; ++i) DrawCube({0.0f, 4.24f, i * 4.0f}, 7.0f, 0.02f, 0.35f, Color{200, 215, 240, 255});
    g_assets.Draw(door, Vector3{0.0f, 0.0f, -9.55f}, 0.0f, 1.0f);
    g_assets.Draw(door, Vector3{-9.55f, 0.0f, 0.0f}, PI * 0.5f, 1.0f);
    g_assets.Draw(door, Vector3{9.55f, 0.0f, 0.0f}, PI * 0.5f, 1.0f);

    // Konsolen an den Seitenwänden (Bildschirm zur Hallenmitte) und
    // Wandbildschirme hinten neben dem Tor
    static const int compWide = g_assets.Id("space-station/computer-wide");
    static const int compSys = g_assets.Id("space-station/computer-system");
    static const int compScreen = g_assets.Id("space-station/computer-screen");
    static const int display = g_assets.Id("space-station/display-wall-wide");
    for (int side = -1; side <= 1; side += 2) {
        const float x = 8.6f * side, yaw = -PI * 0.5f * side;
        g_assets.Draw(compWide, Vector3{x, 0.0f, -6.0f}, yaw, 2.0f);
        g_assets.Draw(compWide, Vector3{x, 0.0f, -4.2f}, yaw, 2.0f);
        g_assets.Draw(compSys, Vector3{x, 0.0f, 3.0f}, yaw, 2.0f);
        g_assets.Draw(compSys, Vector3{x, 0.0f, 4.9f}, yaw, 2.0f);
        g_assets.Draw(display, Vector3{3.6f * side, 1.3f, -8.8f}, 0.0f, 2.0f);
        g_assets.Draw(display, Vector3{5.4f * side, 1.3f, -8.8f}, 0.0f, 2.0f);
        // Anwahlpulte vorn, zum Tor gedreht
        g_assets.Draw(compScreen, Vector3{3.4f * side, 0.0f, 0.5f}, PI - 0.35f * side, 2.0f);
    }

    // Stargate mit Treppe (wie Render.cpp), leuchtet bei der Anwahl
    static const int gateModel = g_assets.Id("gate_complex");
    static const int stairModel = g_assets.Id("stairs");
    const Stargate home = HomeGate();
    g_assets.Draw(gateModel, home.pos, home.yaw, home.scale, WHITE, Vector3{0.10f * g, 0.25f * g, 0.55f * g});
    if (const float top = GateStairTop(home); top > 0.0f) {
        const Vector3 n = GateNormal(home);
        const float sz = cfg::GATE_STAIR_DEPTH / 0.7f, along = 0.25f * home.scale + 0.5f * sz;
        Matrix m = MatrixMultiply(MatrixScale(cfg::GATE_STAIR_WIDTH / 0.3f, top / 0.5f, sz), MatrixRotateY(home.yaw + PI));
        m = MatrixMultiply(m, MatrixTranslate(home.pos.x + n.x * along, home.pos.y, home.pos.z + n.z * along));
        g_assets.Draw(stairModel, m);
    }

    // Team SG1: schaut zur Kamera, bei der Anwahl dreht es sich zum Tor und
    // geht zuletzt los (walk läuft nach dem Weg, siehe Render.cpp)
    static const int blaster = g_assets.Id("blaster/blaster-a");
    const float scale = kFigure / 3.77f;
    const float turn = dialT_ < 0.0f ? 0.0f : std::clamp(dialT_ / 0.8f, 0.0f, 1.0f);
    const float yaw = PI * turn * turn * (3.0f - 2.0f * turn);
    const float walkT = dialT_ - kWalkStart;
    const int count = 1 + w.Me().sgMates;
    for (int i = 0; i < count && i < 4; ++i) {
        Vector3 pos{kSpots[i].x, 0.0f, kSpots[i].y};
        const char* anim = "idle";
        float at = time_ + 0.37f * static_cast<float>(i);
        if (walkT > 0.0f) {
            const float walked = 1.5f * scale * walkT;
            pos.z -= walked;
            anim = "walk";
            at = walkT + 0.2f * static_cast<float>(i);
        }
        const Vector3 hand = g_assets.DrawCharacter(i, anim, at, true, TRS(pos, yaw, scale));
        g_assets.Draw(blaster, TRS(hand, yaw - PI, 0.0f, 0.0f, 1.15f * scale));
    }

    // Wurmloch (additiv, ohne Schreiben in den Tiefenpuffer)
    if (look.glow > 0.0f) {
        rlDrawRenderBatchActive();
        rlDisableDepthMask();
        BeginBlendMode(BLEND_ADDITIVE);
        DrawGateWormhole(home.pos, home.yaw, home.scale, look, time_);
        EndBlendMode();
        rlDrawRenderBatchActive();
        rlEnableDepthMask();
    }
    EndMode3D();
    g_assets.SetFlashes(nullptr, nullptr, 0);
    rlSetClipPlanes(0.5, 2500.0);
}

CommandAction CommandCenterScreen::Draw(World& w) {
    DrawScene(w);
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    PlayerState& me = w.Me();
    const bool dialing = Dialing();
    const bool live = frames_ > 1;   // der Klick, der hierher geführt hat, zählt nicht

    TextCentered("COMMAND CENTER", W * 0.5f, 20 * s, 44 * s, ACCENT);
    TextCentered("Stargate-Kommando  ·  Team SG1", W * 0.5f, 66 * s, 20 * s, MUTED);

    // --- links: Team, Auftrag, Knöpfe ---------------------------------------
    const float pw = 440 * s, top = 100 * s;
    const Rectangle lp{24 * s, top, pw, H - top - 24 * s};
    Panel(lp);
    float x = lp.x + 22 * s, y = lp.y + 18 * s;
    const float iw = pw - 44 * s;
    Text("TEAM SG1", x, y, 24 * s, ACCENT);
    y += 36 * s;
    for (int i = 0; i <= cfg::SG_MATES_MAX; ++i) {
        const bool on = i <= me.sgMates;
        Text(TextFormat("SG1-%d", i + 1), x, y, 20 * s, on ? TEXT : MUTED);
        Text(i == 0 ? "Du (Anführer)" : on ? "Kamerad (KI)" : "-", x + 90 * s, y, 20 * s, on ? TEXT : MUTED);
        y += 28 * s;
    }
    y += 12 * s;
    Text("AUFTRAG", x, y, 18 * s, MUTED);
    y += 26 * s;
    const MapFileInfo* mission = target_ > 0 ? &targets_[static_cast<size_t>(target_ - 1)] : nullptr;
    const std::string brief[] = {
        mission ? TextFormat("Durch das Stargate nach %s.", mission->name.c_str()) : "Durch das Stargate auf den Mond.",
        TextFormat("Sammle %d Proben an gelben Kristallen.", cfg::SAMPLE_GOAL),
        "Dann wählt ein Stargate in der Nähe an:",
        "die Treppe hinauf und hindurch.",
        "Vorsicht vor Bodenpiraten!",
    };
    for (const std::string& line : brief) {
        Text(line, x, y, 18 * s, TEXT);
        y += 24 * s;
    }
    y += 6 * s;
    Text(TextFormat("Punkte: Probe %d · Pirat %d · Rückkehr %d", cfg::SG_POINTS_SAMPLE, cfg::SG_POINTS_PIRATE,
                    cfg::SG_POINTS_RETURN),
         x, y, 16 * s, MUTED);
    y += 24 * s;
    if (mission)
        Text(TextFormat("Einsatzkarte: %s · %d m · %s", MapThemeName(mission->theme), mission->size, MapSkyName(mission->sky)),
             x, y, 16 * s, ACCENT);
    else if (!targets_.empty())
        Text("Mondkarte: gemeinsam mit den Piloten", x, y, 16 * s, MUTED);

    // Ziel: Mondkarte, dann die Einsatzkarten der Reihe nach (wie "Kameraden")
    const float bh = 50 * s, gap = 10 * s;
    const float listTop = lp.y + lp.height - 18 * s - 5 * bh - 4 * gap;
    if (!hasGate_) Text("Auf dieser Karte steht kein Stargate.", x, listTop - 30 * s, 18 * s, BAD);
    const std::string goal = mission ? mission->name : std::string("Mondkarte (gemeinsam)");
    const std::vector<std::string> items = {"Tor anwählen", "Ziel: " + goal, TextFormat("Kameraden: %d", me.sgMates),
                                            "Als Pilot spielen", "Speichern & zum Hauptmenü"};
    const std::vector<bool> enabled = {hasGate_ && !dialing, !dialing && !targets_.empty(), !dialing, !dialing, !dialing};
    int c = ButtonList(items, enabled, sel_, lp.x + pw * 0.5f, listTop, iw, bh, gap, 22 * s);
    if (!live) c = -1;
    CommandAction action = CommandAction::None;
    if (c == 0) {
        dialT_ = 0.0f;
        ended_ = 0;
        g_audio.Play(Sfx::ScanOn);
    } else if (c == 1) {
        target_ = (target_ + 1) % (static_cast<int>(targets_.size()) + 1);
        hasGate_ = target_ > 0 || moonGate_;
        g_audio.Play(Sfx::UiConfirm);
    } else if (c == 2) {
        me.sgMates = (me.sgMates + 1) % (cfg::SG_MATES_MAX + 1);
        g_audio.Play(Sfx::UiConfirm);
    } else if (c == 3) {
        action = CommandAction::Pilot;
    } else if (c == 4) {
        action = CommandAction::SaveAndMenu;
    }

    // --- rechts: Scoreboard -------------------------------------------------
    const Rectangle rp{W - 24 * s - 460 * s, top, 460 * s, H - top - 24 * s};
    Panel(rp);
    x = rp.x + 20 * s;
    y = rp.y + 18 * s;
    const float rx = rp.x + rp.width - 20 * s;
    Text("SCOREBOARD", x, y, 24 * s, ACCENT);
    y += 32 * s;
    Text("Eigene Einsätze, beste zuerst", x, y, 16 * s, MUTED);
    y += 34 * s;
    // Spalten: Rang, Einsatz, Ergebnis, Proben, Piraten, Dauer, Punkte (rechtsbündig)
    const float cx[6] = {x, x + 34 * s, x + 104 * s, x + 214 * s, x + 270 * s, x + 330 * s};
    const char* head[6] = {"#", "EINSATZ", "ERGEBNIS", "PROBEN", "PIRATEN", "DAUER"};
    for (int i = 0; i < 6; ++i) Text(head[i], cx[i], y, 14 * s, MUTED);
    TextRight("PUNKTE", rx, y, 14 * s, MUTED);
    y += 24 * s;
    std::vector<const SgRecord*> rows;
    for (const SgRecord& r : me.sgLog) rows.push_back(&r);
    std::stable_sort(rows.begin(), rows.end(), [](const SgRecord* a, const SgRecord* b) {
        return a->points != b->points ? a->points > b->points : a->number > b->number;
    });
    const int newest = me.sgLog.empty() ? -1 : me.sgLog.back().number;
    int best = 0;
    for (const SgRecord* r : rows) best = std::max(best, r->points);
    if (rows.empty()) Text("Noch keine Einsätze.", x, y + 6 * s, 18 * s, MUTED);
    for (size_t i = 0; i < rows.size() && i < 10; ++i) {
        const SgRecord& r = *rows[i];
        if (ended_ != 0 && r.number == newest)
            DrawRectangleRec({rp.x + 8 * s, y - 5 * s, rp.width - 16 * s, 28 * s}, PANEL_LIGHT);
        const Color tc = r.number == newest ? ACCENT : TEXT;
        Text(TextFormat("%d", static_cast<int>(i) + 1), cx[0], y, 18 * s, tc);
        Text(TextFormat("%d", r.number), cx[1], y, 18 * s, tc);
        Text(r.success ? "zurück" : "gescheitert", cx[2], y, 18 * s, r.success ? GOOD : BAD);
        Text(TextFormat("%d", r.samples), cx[3], y, 18 * s, tc);
        Text(TextFormat("%d", r.pirates), cx[4], y, 18 * s, tc);
        Text(ui::Time(r.seconds), cx[5], y, 18 * s, tc);
        TextRight(ui::Num(r.points), rx, y, 18 * s, tc);
        y += 30 * s;
    }
    Text(TextFormat("Einsätze: %d  ·  Bestwert: %s", static_cast<int>(me.sgLog.size()), ui::Num(best).c_str()), x,
         rp.y + rp.height - 34 * s, 16 * s, MUTED);

    // --- Mitte: Ergebnis des letzten Einsatzes, Anwahl ------------------------
    const float midW = std::min(500 * s, W - 2 * (24 * s + 460 * s) - 24 * s);
    if (ended_ != 0 && !me.sgLog.empty() && midW > 100 * s) {
        const SgRecord& r = me.sgLog.back();
        const Rectangle br{W * 0.5f - midW * 0.5f, top, midW, 70 * s};
        Panel(br);
        TextCentered(r.success ? TextFormat("Einsatz %d: zurück mit %d Proben", r.number, r.samples)
                               : TextFormat("Einsatz %d gescheitert", r.number),
                     W * 0.5f, br.y + 12 * s, 22 * s, r.success ? GOOD : BAD);
        TextCentered(TextFormat("%s Punkte  ·  %d Piraten  ·  %s", ui::Num(r.points).c_str(), r.pirates,
                                ui::Time(r.seconds).c_str()),
                     W * 0.5f, br.y + 42 * s, 18 * s, TEXT);
    }
    if (dialing && midW > 100 * s) {
        const Rectangle dr{W * 0.5f - midW * 0.5f, H - 110 * s, midW, 80 * s};
        Panel(dr);
        TextCentered(dialT_ < cfg::GATE_DIAL ? "Stargate wird angewählt ..." : "Wurmloch offen. Team SG1, los!", W * 0.5f,
                     dr.y + 14 * s, 22 * s, dialT_ < cfg::GATE_DIAL ? TEXT : GOOD);
        Bar({dr.x + 20 * s, dr.y + 50 * s, dr.width - 40 * s, 12 * s}, dialT_ / cfg::GATE_DIAL, SHIELD);
    }
    if (dialT_ >= kDialDone) {
        dialT_ = -1.0f;
        action = CommandAction::Mission;
    }
    return action;
}
