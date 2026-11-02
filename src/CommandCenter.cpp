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
// Ist eine Einsatzkarte das Ziel, zeigt das rechte Feld unter dem
// Scoreboard ihre Vorschau: die Grundrisse der Teile von oben (DrawMapPreview).
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

// Vorschaubild einer Einsatzkarte im Rechteck r: Boden in seiner Farbe, darauf
// die Grundrisse der Teile (SegmentBounds, gedreht wie in der Welt) – erst
// Wege, dann alles andere ohne Dächer, zuletzt die Markierungen: Stargate
// blau, Kristallplätze gelb, Gegner-Startplätze rot.
void DrawMapPreview(const GameMap& m, Rectangle r) {
    static const Color grounds[] = {{92, 128, 68, 255}, {118, 94, 68, 255}, {118, 118, 122, 255}, {192, 174, 128, 255}};
    DrawRectangleRec(r, grounds[std::clamp(static_cast<int>(m.ground), 0, 3)]);
    const float half = MapHalf(m), k = r.width / (2.0f * half);
    auto has = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };
    for (int pass = 0; pass < 3; ++pass) {
        for (const MapPiece& q : m.pieces) {
            const bool flat = has(q.model, "road") || has(q.model, "planks");
            const bool marker = IsMarker(q.model);
            if ((pass == 0) != flat || (pass == 2) != marker || has(q.model, "roof") || has(q.model, "chimney")) continue;
            const Vector2 p{r.x + (q.x + half) * k, r.y + (q.z + half) * k};
            if (q.model == MARKER_CRYSTAL || q.model == MARKER_ENEMY) {
                const bool crystal = q.model == MARKER_CRYSTAL;
                DrawCircleV(p, std::max(3.0f, (crystal ? 1.4f : 1.1f) * k),
                            crystal ? Color{255, 220, 40, 255} : Color{230, 50, 40, 255});
                continue;
            }
            Color c{176, 120, 170, 255};
            if (q.model == MARKER_GATE) c = Color{60, 140, 255, 255};
            else if (flat) c = Color{150, 150, 150, 255};
            else if (has(q.model, "wall") || has(q.model, "pillar")) c = Color{92, 64, 44, 255};
            else if (has(q.model, "fence") || has(q.model, "hedge")) c = Color{140, 104, 50, 255};
            else if (has(q.model, "tree") || has(q.model, "pine")) c = Color{34, 92, 44, 255};
            else if (has(q.model, "grave") || has(q.model, "cross") || has(q.model, "crypt")) c = Color{130, 130, 142, 255};
            else if (has(q.model, "rock") || has(q.model, "trunk")) c = Color{108, 108, 108, 255};
            const BoundingBox b = SegmentBounds(q.model);
            const Vector2 off = MapRotate((b.min.x + b.max.x) * 0.5f * q.scale, (b.min.z + b.max.z) * 0.5f * q.scale, q.yaw);
            const float w = std::max(1.0f, (b.max.x - b.min.x) * q.scale * k), h = std::max(1.0f, (b.max.z - b.min.z) * q.scale * k);
            // MapRotate dreht in der Draufsicht (z nach unten) gegen den Uhrzeigersinn
            DrawRectanglePro({p.x + off.x * k, p.y + off.y * k, w, h}, {w * 0.5f, h * 0.5f}, -q.yaw * RAD2DEG, c);
        }
    }
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
void CommandCenterScreen::DrawScene(int figures) {
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
    for (int i = 0; i < figures && i < 4; ++i) {
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
    DrawScene(1 + w.Me().sgMates);
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
        mission && mission->theme == MapTheme::Graveyard ? "Vorsicht vor Zombies!" : "Vorsicht vor Bodenpiraten!",
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
    // Vorschau der gewählten Einsatzkarte (einmal gelesen, solange das Ziel bleibt)
    if (TargetStem() != previewStem_) {
        previewStem_ = TargetStem();
        preview_ = GameMap{};
        if (!previewStem_.empty() && !LoadMapFile(previewStem_, preview_)) preview_.pieces.clear();
    }
    if (!previewStem_.empty()) {
        const float ps = std::min(280 * s, rp.width - 40 * s);
        const Rectangle pr{rp.x + (rp.width - ps) * 0.5f, rp.y + rp.height - 46 * s - ps, ps, ps};
        Text(TextFormat("VORSCHAU: %s", preview_.name.c_str()), pr.x, pr.y - 24 * s, 16 * s, MUTED);
        DrawMapPreview(preview_, pr);
    }

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

// ===========================================================================
// Mehrspieler
// ===========================================================================
void CommandCenterScreen::OpenOnline() {
    dialT_ = -1.0f;
    frames_ = 0;
    sel_ = 0;
    ended_ = 0;
    joinTarget_ = 0;
}

// Aufbau wie im Einzelspieler: links Team, Auftrag und Knöpfe, rechts die
// Teams des Servers (Klick auf ein Team: Ziel für "Beitreten"), oben in der
// Mitte das Ergebnis des letzten Einsatzes, unten die Anwahl. Die Anwahl
// beginnt, wenn der Server sie meldet (Team im Zustand 1); die Uhr läuft dann
// hier weiter (Update), den Einsatz startet der Server.
OnlineCommand CommandCenterScreen::DrawOnline(const net::TeamsMsg& teams, int myId, bool fresh) {
    const net::TeamEntry* mine = nullptr;
    for (const net::TeamEntry& t : teams.teams)
        for (const net::TeamMember& mb : t.members)
            if (mb.playerId == myId) mine = &t;
    if (mine && mine->state == 1) {
        if (dialT_ < 0.0f) {
            dialT_ = mine->dialT;
            ended_ = 0;
            g_audio.Play(Sfx::ScanOn);
        }
    } else {
        dialT_ = -1.0f;
    }
    if (fresh && teams.hasResult) ended_ = teams.success ? 1 : 2;
    DrawScene(mine ? std::clamp(static_cast<int>(mine->members.size()), 1, 4) : 1);

    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const bool live = frames_ > 1;
    const bool busy = mine && mine->state != 0;
    const bool founder = mine && !mine->members.empty() && mine->members.front().playerId == myId;
    bool allReady = true, myReady = false;
    if (mine)
        for (size_t i = 0; i < mine->members.size(); ++i) {
            if (mine->members[i].playerId == myId) myReady = mine->members[i].ready;
            if (i > 0 && !mine->members[i].ready) allReady = false;
        }
    auto joinable = [](const net::TeamEntry& t) { return t.state == 0 && static_cast<int>(t.members.size()) < cfg::SG_TEAM_MAX; };
    const net::TeamEntry* join = nullptr;
    for (const net::TeamEntry& t : teams.teams)
        if (joinable(t) && (!join || t.number == joinTarget_)) join = &t;

    TextCentered("COMMAND CENTER", W * 0.5f, 20 * s, 44 * s, ACCENT);
    TextCentered(mine ? TextFormat("Stargate-Kommando  ·  Team SG%d", mine->number) : "Stargate-Kommando  ·  Mehrspieler", W * 0.5f,
                 66 * s, 20 * s, MUTED);

    // --- links: Team, Auftrag, Knöpfe ---------------------------------------
    const float pw = 440 * s, top = 100 * s;
    const Rectangle lp{24 * s, top, pw, H - top - 24 * s};
    Panel(lp);
    float x = lp.x + 22 * s, y = lp.y + 18 * s;
    const float iw = pw - 44 * s;
    Text(mine ? TextFormat("TEAM SG%d", mine->number) : "KEIN TEAM", x, y, 24 * s, ACCENT);
    y += 36 * s;
    if (!mine) {
        Text("Gründe ein Team oder tritt einem bei.", x, y, 18 * s, TEXT);
        y += 26 * s;
        Text(TextFormat("Bis zu %d Spieler je Team.", cfg::SG_TEAM_MAX), x, y, 18 * s, MUTED);
        y += 28 * s;
    } else {
        for (size_t i = 0; i < mine->members.size(); ++i) {
            const net::TeamMember& mb = mine->members[i];
            const Color c = mb.playerId == myId ? ACCENT : TEXT;
            Text(TextFormat("SG%d-%d", mine->number, static_cast<int>(i) + 1), x, y, 20 * s, c);
            Text(mb.name, x + 90 * s, y, 20 * s, c);
            const char* st = mb.out ? "im Einsatz" : i == 0 ? "Gründer" : mb.ready ? "bereit" : "-";
            TextRight(st, x + iw, y, 18 * s, mb.out ? SHIELD : (i == 0 || mb.ready) ? GOOD : MUTED);
            y += 28 * s;
        }
    }
    y += 12 * s;
    Text("AUFTRAG", x, y, 18 * s, MUTED);
    y += 26 * s;
    const char* brief[] = {"Durch das Stargate auf den Mond.", TextFormat("Sammelt %d Proben an gelben Kristallen", cfg::SAMPLE_GOAL),
                           "(sie zählen für das ganze Team).", "Dann ein Stargate in der Nähe anwählen:",
                           "die Treppe hinauf und hindurch.", "Vorsicht vor Bodenpiraten!"};
    for (const char* line : brief) {
        Text(line, x, y, 18 * s, TEXT);
        y += 24 * s;
    }

    std::vector<std::string> items;
    std::vector<bool> enabled;
    if (!mine) {
        items = {"Tor anwählen (allein)", "Team gründen", join ? TextFormat("Beitreten: SG%d", join->number) : "Beitreten",
                 "Als Pilot spielen", "Abmelden & zum Hauptmenü"};
        enabled = {teams.moonGate, true, join != nullptr, true, true};
    } else {
        items = {founder ? "Tor anwählen" : myReady ? "Nicht mehr bereit" : "Bereit", "Team verlassen", "Als Pilot spielen",
                 "Abmelden & zum Hauptmenü"};
        enabled = {!busy && (!founder || (teams.moonGate && allReady)), !busy, !busy, true};
    }
    sel_ = std::min(sel_, static_cast<int>(items.size()) - 1);
    const float bh = 50 * s, gap = 10 * s;
    const float n = static_cast<float>(items.size());
    const float listTop = lp.y + lp.height - 18 * s - n * bh - (n - 1) * gap;
    if (!teams.moonGate) Text("Auf der Mondkarte steht kein Stargate.", x, listTop - 30 * s, 18 * s, BAD);
    else if (founder && !busy && !allReady) Text("Warte, bis alle bereit sind.", x, listTop - 30 * s, 18 * s, MUTED);
    else if (mine && !founder && !busy) Text("Der Gründer wählt das Tor an.", x, listTop - 30 * s, 18 * s, MUTED);
    int c = ButtonList(items, enabled, sel_, lp.x + pw * 0.5f, listTop, iw, bh, gap, 22 * s);
    if (!live) c = -1;
    OnlineCommand cmd;
    if (c >= 0) g_audio.Play(Sfx::UiConfirm);
    if (!mine) {
        if (c == 0) cmd.kind = OnlineCommand::Kind::Dial;
        else if (c == 1) cmd.kind = OnlineCommand::Kind::Create;
        else if (c == 2 && join) cmd = {OnlineCommand::Kind::Join, join->number};
        else if (c == 3) cmd.kind = OnlineCommand::Kind::Pilot;
        else if (c == 4) cmd.kind = OnlineCommand::Kind::Menu;
    } else {
        if (c == 0) cmd = founder ? OnlineCommand{OnlineCommand::Kind::Dial, 0} : OnlineCommand{OnlineCommand::Kind::Ready, myReady ? 0 : 1};
        else if (c == 1) cmd.kind = OnlineCommand::Kind::Leave;
        else if (c == 2) cmd.kind = OnlineCommand::Kind::Pilot;
        else if (c == 3) cmd.kind = OnlineCommand::Kind::Menu;
    }

    // --- rechts: alle Teams ---------------------------------------------------
    const Rectangle rp{W - 24 * s - 460 * s, top, 460 * s, H - top - 24 * s};
    Panel(rp);
    x = rp.x + 20 * s;
    y = rp.y + 18 * s;
    const float rx = rp.x + rp.width - 20 * s;
    Text("SG-TEAMS", x, y, 24 * s, ACCENT);
    y += 32 * s;
    Text(mine ? "Alle Teams auf diesem Server" : "Klick auf ein Team: dort beitreten", x, y, 16 * s, MUTED);
    y += 30 * s;
    if (teams.teams.empty()) Text("Noch kein Team gegründet.", x, y + 6 * s, 18 * s, MUTED);
    static const char* states[] = {"im Command Center", "wählt an ...", "im Einsatz"};
    for (const net::TeamEntry& t : teams.teams) {
        if (y > rp.y + rp.height - 60 * s) break;
        const float rowH = 34 * s + 22 * s * static_cast<float>(t.members.size());
        const Rectangle row{rp.x + 8 * s, y - 6 * s, rp.width - 16 * s, rowH};
        const bool target = !mine && join == &t;
        if (&t == mine || target) DrawRectangleRec(row, PANEL_LIGHT);
        if (!mine && joinable(t) && live && PointerPressed() && CheckCollisionPointRec(PointerPos(), row)) {
            joinTarget_ = t.number;
            g_audio.Play(Sfx::UiMove);
        }
        Text(TextFormat("SG%d", t.number), x, y, 22 * s, &t == mine ? ACCENT : TEXT);
        Text(TextFormat("%d / %d", static_cast<int>(t.members.size()), cfg::SG_TEAM_MAX), x + 70 * s, y + 3 * s, 18 * s, MUTED);
        TextRight(states[std::min<int>(t.state, 2)], rx, y + 3 * s, 18 * s, t.state == 2 ? SHIELD : t.state == 1 ? ACCENT : GOOD);
        y += 30 * s;
        for (const net::TeamMember& mb : t.members) {
            Text(mb.name, x + 16 * s, y, 18 * s, mb.playerId == myId ? ACCENT : TEXT);
            if (mb.ready) TextRight("bereit", rx, y, 16 * s, GOOD);
            y += 22 * s;
        }
        y += 10 * s;
    }

    // --- Mitte: Ergebnis des letzten Einsatzes, Anwahl, Team unterwegs --------
    const float midW = std::min(500 * s, W - 2 * (24 * s + 460 * s) - 24 * s);
    if (ended_ != 0 && teams.hasResult && midW > 100 * s) {
        const Rectangle br{W * 0.5f - midW * 0.5f, top, midW, 70 * s};
        Panel(br);
        TextCentered(teams.success ? TextFormat("Einsatz %d: zurück mit %d Proben", teams.number, teams.samples)
                                   : TextFormat("Einsatz %d gescheitert", teams.number),
                     W * 0.5f, br.y + 12 * s, 22 * s, teams.success ? GOOD : BAD);
        TextCentered(TextFormat("%s Punkte  ·  %d Piraten  ·  %s", ui::Num(teams.points).c_str(), teams.pirates,
                                ui::Time(teams.seconds).c_str()),
                     W * 0.5f, br.y + 42 * s, 18 * s, TEXT);
    }
    if (midW > 100 * s && (dialT_ >= 0.0f || (mine && mine->state == 2))) {
        const Rectangle dr{W * 0.5f - midW * 0.5f, H - 110 * s, midW, 80 * s};
        Panel(dr);
        if (dialT_ >= 0.0f) {
            TextCentered(dialT_ < cfg::GATE_DIAL ? "Stargate wird angewählt ..." : TextFormat("Wurmloch offen. Team SG%d, los!", mine->number),
                         W * 0.5f, dr.y + 14 * s, 22 * s, dialT_ < cfg::GATE_DIAL ? TEXT : GOOD);
            Bar({dr.x + 20 * s, dr.y + 50 * s, dr.width - 40 * s, 12 * s}, dialT_ / cfg::GATE_DIAL, SHIELD);
        } else {
            TextCentered("Dein Team ist im Einsatz ...", W * 0.5f, dr.y + 14 * s, 22 * s, TEXT);
            TextCentered("Gleich geht es durch das Tor hinterher.", W * 0.5f, dr.y + 46 * s, 18 * s, MUTED);
        }
    }
    return cmd;
}

// ===========================================================================
// Rollenwahl
// ===========================================================================
int DrawRoleChoice(int& sel, bool live, bool online) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 150});
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
         {online ? "Start im Command Center: Team gründen oder beitreten." : "Start im Command Center mit Team SG1.",
          TextFormat("%d Proben an gelben Kristallen sammeln", cfg::SAMPLE_GOAL),
          "und durch ein Stargate zurückkehren."}},
    };
    const float cw = 460 * s, ch = 250 * s, gap = 40 * s;
    const float y = H * 0.5f - ch * 0.5f;
    int pick = -1;
    for (int i = 0; i < 2; ++i) {
        const Rectangle r{W * 0.5f - cw - gap * 0.5f + i * (cw + gap), y, cw, ch};
        if (Button(r, "", 20 * s, sel == i) && live) pick = i;
        const Card& c = cards[i];
        Text(c.title, r.x + 28 * s, r.y + 26 * s, 40 * s, sel == i ? ACCENT : TEXT);
        Text(c.sub, r.x + 28 * s, r.y + 76 * s, 22 * s, MUTED);
        for (int l = 0; l < 3; ++l) Text(c.lines[l], r.x + 28 * s, r.y + 124 * s + l * 30 * s, 20 * s, TEXT);
    }
    if (live) {
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT)) sel = 0;
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) sel = 1;
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) pick = sel;
    }
    TextCentered(online ? "Klick oder Pfeiltasten und Enter: wählen    ESC: abmelden" : "Klick oder Pfeiltasten und Enter: wählen    ESC: zurück",
                 W * 0.5f, y + ch + 30 * s, 20 * s, MUTED);
    if (pick >= 0) g_audio.Play(Sfx::UiConfirm);
    return pick;
}
