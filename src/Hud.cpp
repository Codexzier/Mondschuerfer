// HUD, Minikarte, Stationsfenster und Endbildschirme.
#include "Hud.hpp"
#include "Assets.hpp"
#include "Audio.hpp"
#include "Ui.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>

using namespace ui;

namespace {

void StatBar(float x, float y, float w, const char* label, const std::string& value, float frac, Color col) {
    const float s = Scale();
    Text(label, x, y, 20 * s, MUTED);
    TextRight(value, x + w, y, 20 * s, TEXT);
    Bar({x, y + 24 * s, w, 12 * s}, frac, col);
}

// Name über einer Station. Liegt sie außerhalb des Bildes, zeigt ein Pfeil am
// Bildrand in ihre Richtung (nur für die nächste Station jeder Art: edgeArrow).
void StationLabel(const World& w, const Camera3D& cam, int idx, bool edgeArrow) {
    const float s = Scale();
    const Station& st = w.stations[idx];
    const bool trade = st.type == StationType::Trade;
    Vector3 top = Vector3Add(st.pos, {0, 10.0f, 0});
    Vector2 p = GetWorldToScreen(top, cam);
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    Color col = trade ? ACCENT : Color{120, 210, 255, 255};
    std::string upper = st.name;
    for (char& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    std::string name = trade ? TextFormat("%s  Stufe %d", upper.c_str(), w.StationStage()) : upper;
    float margin = 40 * s;
    Vector2 base = GetWorldToScreen(st.pos, cam);
    bool baseOnScreen = base.x > 0 && base.x < W && base.y > 0 && base.y < H;
    bool onScreen = p.x > margin && p.x < W - margin && p.y > margin && p.y < H - margin;
    if (onScreen || baseOnScreen) {
        float ly = std::max(p.y, 50 * s);
        if (p.x < 400 * s && ly < 330 * s) ly = 330 * s;
        TextCentered(name, p.x, ly, 20 * s, col);
    } else if (edgeArrow) {
        // Randmarkierung in Richtung der Station
        Vector2 c{W * 0.5f, H * 0.5f};
        Vector2 d = Vector2Subtract(p, c);
        float kx = (W * 0.5f - margin) / std::max(1.0f, fabsf(d.x));
        float ky = (H * 0.5f - margin) / std::max(1.0f, fabsf(d.y));
        float k = std::min(kx, ky);
        Vector2 e = Vector2Add(c, Vector2Scale(d, k));
        if (e.x < 400 * s && e.y < 340 * s) e.y = 340 * s;              // nicht unter das Statusfenster
        if (e.x > W - 260 * s && e.y < 300 * s) e.y = 300 * s;          // nicht unter die Minikarte
        float ang = atan2f(d.y, d.x);
        Vector2 tip{e.x + cosf(ang) * 14 * s, e.y + sinf(ang) * 14 * s};
        Vector2 l{e.x + cosf(ang + 2.5f) * 12 * s, e.y + sinf(ang + 2.5f) * 12 * s};
        Vector2 r{e.x + cosf(ang - 2.5f) * 12 * s, e.y + sinf(ang - 2.5f) * 12 * s};
        DrawTriangle(tip, r, l, col);
        DrawTriangle(tip, l, r, col);
        int dist = static_cast<int>(Dist2D(w.Player().pos, st.pos));
        std::string t = TextFormat("%s %d m", trade ? "Handel" : "Werft", dist);
        Vector2 tp = Vector2Subtract(e, Vector2Scale(Vector2Normalize(d), 34 * s));
        TextCentered(t, tp.x, tp.y - 8 * s, 18 * s, col);
    }
}

void Minimap(const World& w) {
    const float s = Scale();
    const float size = 230 * s;
    Rectangle r{GetScreenWidth() - size - 16 * s, 16 * s, size, size};
    Panel(r, Color{18, 22, 30, 215});
    auto map = [&](Vector3 p) {
        return Vector2{r.x + (p.x + cfg::WORLD_HALF) / (2 * cfg::WORLD_HALF) * r.width,
                       r.y + (p.z + cfg::WORLD_HALF) / (2 * cfg::WORLD_HALF) * r.height};
    };
    for (int i = 1; i < 6; ++i) {
        float x = r.x + r.width * i / 6.0f, y = r.y + r.height * i / 6.0f;
        DrawLineEx({x, r.y}, {x, r.y + r.height}, 1, Color{255, 255, 255, 18});
        DrawLineEx({r.x, y}, {r.x + r.width, y}, 1, Color{255, 255, 255, 18});
    }
    const Ship& p = w.Player();
    // Radarreichweite
    Vector2 pp = map(p.pos);
    DrawCircleLinesV(pp, 90.0f / (2 * cfg::WORLD_HALF) * r.width, Color{255, 255, 255, 35});

    for (const auto& d : w.deposits) {
        if (!d.revealed) continue;
        Vector2 m = map(d.pos);
        DrawRectangleV({m.x - 2 * s, m.y - 2 * s}, {4 * s, 4 * s}, GOOD);
    }
    for (const auto& wr : w.wrecks) {
        Vector2 m = map(wr.pos);
        DrawRectangleV({m.x - 2 * s, m.y - 2 * s}, {4 * s, 4 * s}, Color{255, 170, 60, 255});
    }
    for (const Station& st : w.stations) {
        Vector2 m = map(st.pos);
        Color c = st.type == StationType::Trade ? ACCENT : Color{120, 210, 255, 255};
        DrawRectangleV({m.x - 5 * s, m.y - 5 * s}, {10 * s, 10 * s}, c);
        DrawRectangleLinesEx({m.x - 7 * s, m.y - 7 * s, 14 * s, 14 * s}, 1, c);
    }
    for (const auto& sh : w.ships) {
        if (sh.dead || sh.hidden || sh.kind == ShipKind::Player) continue;
        if (Dist2D(sh.pos, p.pos) > 90.0f) continue;
        Vector2 m = map(sh.pos);
        Color c = (sh.faction == Faction::Pirate || sh.hostileToPlayer) ? BAD
                  : sh.kind == ShipKind::Player ? GOOD                  // andere Spieler (Mehrspieler)
                                                : Color{140, 200, 255, 255};
        DrawCircleV(m, 2.6f * s, c);
    }
    if (!p.dead && !p.hidden) {
        Vector3 f = ForwardFromYaw(p.yaw);
        Vector2 fd{f.x, f.z};
        Vector2 sd{-fd.y, fd.x};
        Vector2 tip = Vector2Add(pp, Vector2Scale(fd, 8 * s));
        Vector2 a = Vector2Add(pp, Vector2Add(Vector2Scale(fd, -5 * s), Vector2Scale(sd, 5 * s)));
        Vector2 b = Vector2Add(pp, Vector2Add(Vector2Scale(fd, -5 * s), Vector2Scale(sd, -5 * s)));
        DrawTriangle(tip, a, b, WHITE);
        DrawTriangle(tip, b, a, WHITE);
    }
    Text("RADAR", r.x + 8 * s, r.y + 6 * s, 16 * s, MUTED);
    // Legende
    struct L { const char* t; Color c; } legend[4] = {{"Handel", ACCENT}, {"Werft", Color{120, 210, 255, 255}},
                                                   {"Kristall", GOOD}, {"Gegner", BAD}};
    for (int i = 0; i < 4; ++i) {
        float lx = r.x + (i % 2) * r.width * 0.5f + 4 * s;
        float ly = r.y + r.height + 8 * s + (i / 2) * 20 * s;
        DrawRectangleV({lx, ly + 3 * s}, {9 * s, 9 * s}, legend[i].c);
        Text(legend[i].t, lx + 14 * s, ly, 15 * s, MUTED);
    }
}

} // namespace

void DrawControls(float x, float y, float size) {
    const char* keys[] = {"W / S", "A / D", "Leertaste", "E", "F", "Mausrad", "ESC", "M"};
    const char* what[] = {"Schub / Bremsen", "Drehen", "Schießen", "Scanner an/aus", "Abbauen / Andocken", "Zoom", "Pause",
                          "Ton an/aus"};
    for (int i = 0; i < 8; ++i) {
        Text(keys[i], x, y + i * size * 1.35f, size, Color{255, 200, 140, 210});
        Text(what[i], x + size * 7.0f, y + i * size * 1.35f, size, Color{220, 225, 235, 200});
    }
}

void DrawHud(const World& w, const Camera3D& cam, bool simulation) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const Ship& p = w.Player();
    const PlayerState& me = w.Me();

    // Treffer-/Warnrand
    float danger = p.hitFlash * 0.6f;
    if (!p.crashing && !p.dead && p.hp / p.maxHp < 0.3f) danger = std::max(danger, 0.25f + 0.2f * sinf(w.time * 6.0f));
    if (danger > 0.01f) {
        unsigned char a = static_cast<unsigned char>(std::min(1.0f, danger) * 150);
        float t = 120 * s;
        DrawRectangleGradientV(0, 0, static_cast<int>(W), static_cast<int>(t), Color{255, 40, 30, a}, Color{255, 40, 30, 0});
        DrawRectangleGradientV(0, static_cast<int>(H - t), static_cast<int>(W), static_cast<int>(t), Color{255, 40, 30, 0}, Color{255, 40, 30, a});
        DrawRectangleGradientH(0, 0, static_cast<int>(t), static_cast<int>(H), Color{255, 40, 30, a}, Color{255, 40, 30, 0});
        DrawRectangleGradientH(static_cast<int>(W - t), 0, static_cast<int>(t), static_cast<int>(H), Color{255, 40, 30, 0}, Color{255, 40, 30, a});
    }

    const int nearTrade = w.NearestStation(p.pos, StationType::Trade);
    const int nearYard = w.NearestStation(p.pos, StationType::Shipyard);
    for (int i = 0; i < static_cast<int>(w.stations.size()); ++i)
        StationLabel(w, cam, i, i == nearTrade || i == nearYard);

    // --- Statuspanel oben links ------------------------------------------------
    Rectangle pr{16 * s, 16 * s, 360 * s, 300 * s};
    Panel(pr);
    float x = pr.x + 18 * s, y = pr.y + 14 * s, bw = pr.width - 36 * s;
    Text("KONTOSTAND", x, y, 20 * s, MUTED);
    y += 24 * s;
    Text(Num(me.balance), x, y, 40 * s, ACCENT);
    TextRight("/ " + Num(cfg::GOAL_POINTS), x + bw, y + 16 * s, 20 * s, MUTED);
    y += 46 * s;
    Bar({x, y, bw, 8 * s}, static_cast<float>(me.balance) / cfg::GOAL_POINTS, ACCENT);
    y += 22 * s;
    StatBar(x, y, bw, "SCHILD", TextFormat("%d / %d", static_cast<int>(ceilf(p.hp)), static_cast<int>(p.maxHp)),
            p.hp / p.maxHp, p.hp / p.maxHp < 0.3f ? BAD : SHIELD);
    y += 48 * s;
    std::string scanText = me.scanOn ? TextFormat("AKTIV  %.1f s", me.scanEnergy)
                                     : (me.scanEnergy >= cfg::SCANNER_MAX_ENERGY - 0.01f ? std::string("bereit [E]")
                                                                                         : TextFormat("lädt  %.1f s", me.scanEnergy));
    StatBar(x, y, bw, "SCANNER", scanText, me.scanEnergy / cfg::SCANNER_MAX_ENERGY,
            me.scanOn ? Color{120, 255, 210, 255} : SCAN);
    y += 48 * s;
    StatBar(x, y, bw, "LAGER", TextFormat("%s / %s", Num(p.cargo).c_str(), Num(p.cargoMax).c_str()),
            static_cast<float>(p.cargo) / std::max(1, p.cargoMax), p.cargo >= p.cargoMax ? GOOD : ACCENT);
    y += 50 * s;
    const char* names[4] = {"Antrieb", "Panzer", "Waffen", "Lager"};
    float cx = x;
    for (int i = 0; i < 4; ++i) {
        Text(names[i], cx, y, 16 * s, MUTED);
        for (int k = 0; k < cfg::UPGRADE_LEVELS; ++k) {
            Color c = k < me.upgrades[i] ? ACCENT : Color{70, 76, 90, 255};
            DrawRectangle(static_cast<int>(cx + k * 13 * s), static_cast<int>(y + 20 * s), static_cast<int>(10 * s),
                          static_cast<int>(6 * s), c);
        }
        cx += bw / 4.0f;
    }

    Minimap(w);

    // --- Meldungen ---------------------------------------------------------------------
    float ny = H * 0.15f;
    for (const auto& n : me.notices) {
        float a = std::clamp(n.time / 0.6f, 0.0f, 1.0f);
        Color c = n.color;
        c.a = static_cast<unsigned char>(255 * a);
        std::string t = n.count > 1 ? n.text + TextFormat("  (%dx)", n.count) : n.text;
        float tw = Width(t, 24 * s);
        DrawRectangle(static_cast<int>(W * 0.5f - tw * 0.5f - 12 * s), static_cast<int>(ny - 6 * s),
                      static_cast<int>(tw + 24 * s), static_cast<int>(36 * s), Color{15, 18, 26, static_cast<unsigned char>(170 * a)});
        TextCentered(t, W * 0.5f, ny, 24 * s, c);
        ny += 42 * s;
    }

    // --- Kontext-Hinweis unten ---------------------------------------------------------
    if (p.crashing) {
        TextCentered("SCHIFF STÜRZT AB!", W * 0.5f, H * 0.42f, 50 * s, BAD);
    } else if (!p.dead && !p.hidden && me.dockedAt < 0) {
        std::string hint;
        Color hc = TEXT;
        int kind = 0, id = -1;
        w.InteractTarget(&kind, &id);
        Rectangle hr{W * 0.5f - 300 * s, H - 110 * s, 600 * s, 80 * s};
        if (me.mining) {
            Panel(hr);
            float f = me.mineT / cfg::MINING_TIME;
            TextCentered(TextFormat("ABBAU LÄUFT ...  %.1f / %.0f s", me.mineT, cfg::MINING_TIME), W * 0.5f, hr.y + 12 * s, 22 * s, GOOD);
            Bar({hr.x + 20 * s, hr.y + 44 * s, hr.width - 40 * s, 16 * s}, f, GOOD);
            TextRight("[F] abbrechen", hr.x + hr.width - 14 * s, hr.y + hr.height + 4 * s, 16 * s, MUTED);
        } else {
            if (kind == 3) hint = p.cargo > 0 ? TextFormat("[F] Andocken: Handelsstation  (%s verkaufen)", Num(p.cargo).c_str())
                                              : "[F] Andocken: Handelsstation";
            else if (kind == 4) hint = "[F] Andocken: Werft (Upgrades)";
            else if (kind == 1 || kind == 2) {
                int amount = 0;
                if (kind == 1) { for (const auto& d : w.deposits) if (d.id == id) amount = d.amount; }
                else { for (const auto& wr : w.wrecks) if (wr.id == id) amount = wr.amount; }
                if (p.cargo >= p.cargoMax) { hint = "Lager voll! Zur Handelsstation fliegen."; hc = Color{255, 200, 120, 255}; }
                else hint = TextFormat("[F] %s abbauen  (%s vorhanden)", kind == 1 ? "Kristalle" : "Wrack", Num(amount).c_str());
            } else if (p.cargo >= p.cargoMax) {
                hint = "Lager voll! Zur Handelsstation fliegen.";
                hc = Color{255, 200, 120, 255};
            }
            if (!hint.empty()) {
                float tw = Width(hint, 26 * s) + 40 * s;
                Rectangle r{W * 0.5f - tw * 0.5f, H - 90 * s, tw, 50 * s};
                Panel(r);
                TextCentered(hint, W * 0.5f, r.y + 13 * s, 26 * s, hc);
            }
        }
    }

    if (!simulation) {
        if (me.playTime < 90.0f || IsKeyDown(KEY_H)) DrawControls(18 * s, H - 8 * 16 * s * 1.35f - 16 * s, 16 * s);
        else Text("[H] Steuerung anzeigen", 18 * s, H - 30 * s, 16 * s, Color{200, 205, 215, 170});
    }
}

// ===========================================================================
// Stationsfenster
// ===========================================================================
DockRequest DrawDockPanel(const World& w, bool interactive) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 120});
    const Ship& p = w.Player();
    const PlayerState& me = w.Me();
    DockRequest req;
    if (me.dockedAt < 0 || me.dockedAt >= static_cast<int>(w.stations.size())) return req;
    const Station& station = w.stations[me.dockedAt];

    if (station.type == StationType::Trade) {
        Rectangle r{W * 0.5f - 330 * s, H * 0.5f - 265 * s, 660 * s, 530 * s};
        Panel(r);
        float x = r.x + 30 * s, y = r.y + 24 * s, bw = r.width - 60 * s;
        std::string title = station.name;
        for (char& c : title) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        Text(title, x, y, 40 * s, ACCENT);
        y += 46 * s;
        Text(TextFormat("Ausbaustufe %d von %d", w.StationStage(), cfg::STATION_STAGES - 1), x, y, 20 * s, MUTED);
        y += 34 * s;
        Text("Ladung verkauft", x, y, 24 * s, MUTED);
        TextRight("+" + Num(me.lastSale), x + bw, y, 24 * s, me.lastSale > 0 ? GOOD : TEXT);
        y += 38 * s;
        Text("Kontostand", x, y, 24 * s, MUTED);
        TextRight(Num(me.balance) + " / " + Num(cfg::GOAL_POINTS), x + bw, y, 24 * s, TEXT);
        y += 32 * s;
        Bar({x, y, bw, 12 * s}, static_cast<float>(me.balance) / cfg::GOAL_POINTS, ACCENT);
        y += 34 * s;

        int stage = w.StationStage();
        int next = w.NextStageThreshold();
        Text("Stationsausbau (abgelieferte Ressourcen)", x, y, 20 * s, MUTED);
        y += 26 * s;
        if (next > 0) {
            int cur = cfg::STATION_THRESHOLDS[stage];
            float f = static_cast<float>(w.totalDelivered - cur) / static_cast<float>(next - cur);
            Bar({x, y, bw, 12 * s}, f, Color{255, 210, 120, 255});
            y += 18 * s;
            Text(TextFormat("%s / %s bis Stufe %d", Num(w.totalDelivered).c_str(), Num(next).c_str(), stage + 1), x, y, 18 * s, TEXT);
        } else {
            Bar({x, y, bw, 12 * s}, 1.0f, Color{255, 210, 120, 255});
            y += 18 * s;
            Text("Die Station ist vollständig ausgebaut!", x, y, 18 * s, GOOD);
        }
        y += 40 * s;

        Text("Schild", x, y, 24 * s, MUTED);
        TextRight(TextFormat("%d / %d", static_cast<int>(ceilf(p.hp)), static_cast<int>(p.maxHp)), x + bw, y, 24 * s, TEXT);
        y += 30 * s;
        Bar({x, y, bw, 12 * s}, p.hp / p.maxHp, SHIELD);
        y += 32 * s;

        int cost = w.RepairCost(me);
        std::string rl = cost > 0 ? TextFormat("Schild reparieren  (%s Pkt.)  [R]", Num(cost).c_str()) : "Schild ist voll";
        bool canRepair = cost > 0 && me.balance >= cfg::REPAIR_COST_PER_POINT;
        Rectangle rb{x, y, bw, 50 * s};
        bool repairHover = false;
        if (Button(rb, rl, 22 * s, false, canRepair && interactive, &repairHover) || (interactive && canRepair && IsKeyPressed(KEY_R)))
            req.repair = true;
        else if (interactive && !canRepair && (IsKeyPressed(KEY_R) || (repairHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))))
            g_audio.Play(Sfx::Denied);
        y += 64 * s;
        Rectangle ub{x, y, bw, 50 * s};
        if (Button(ub, "Abdocken  [F]", 22 * s, true, interactive)) req.undock = true;
    } else {
        Rectangle r{W * 0.5f - 400 * s, H * 0.5f - 285 * s, 800 * s, 570 * s};
        Panel(r);
        float x = r.x + 30 * s, y = r.y + 24 * s, bw = r.width - 60 * s;
        std::string title = station.name;
        for (char& c : title) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        Text(title, x, y, 40 * s, Color{120, 210, 255, 255});
        TextRight("Kontostand  " + Num(me.balance), x + bw, y + 10 * s, 24 * s, ACCENT);
        y += 46 * s;
        Text("Rüste dein Schiff auf. Upgrades werden mit Ressourcenpunkten bezahlt.", x, y, 18 * s, MUTED);
        y += 34 * s;

        const char* names[4] = {"Antrieb", "Panzerung", "Bewaffnung", "Lager"};
        for (int t = 0; t < 4; ++t) {
            int lvl = me.upgrades[t];
            bool maxed = lvl >= cfg::UPGRADE_LEVELS;
            std::string desc;
            switch (t) {
                case cfg::UP_SPEED:
                    desc = maxed ? TextFormat("Tempo %.0f", cfg::SPEED_VALUES[lvl])
                                 : TextFormat("Tempo %.0f  ->  %.0f", cfg::SPEED_VALUES[lvl], cfg::SPEED_VALUES[lvl + 1]);
                    break;
                case cfg::UP_ARMOR:
                    desc = maxed ? TextFormat("Schild %.0f", cfg::ARMOR_VALUES[lvl])
                                 : TextFormat("Schild %.0f  ->  %.0f", cfg::ARMOR_VALUES[lvl], cfg::ARMOR_VALUES[lvl + 1]);
                    break;
                case cfg::UP_WEAPON:
                    desc = maxed ? TextFormat("Schaden %.0f, %.1f Schuss/s", cfg::WEAPON_DAMAGE[lvl], 1.0f / cfg::WEAPON_RATE[lvl])
                                 : TextFormat("Schaden %.0f -> %.0f,  %.1f -> %.1f Schuss/s", cfg::WEAPON_DAMAGE[lvl],
                                              cfg::WEAPON_DAMAGE[lvl + 1], 1.0f / cfg::WEAPON_RATE[lvl], 1.0f / cfg::WEAPON_RATE[lvl + 1]);
                    break;
                default:
                    desc = maxed ? TextFormat("Kapazität %d", cfg::CARGO_VALUES[lvl])
                                 : TextFormat("Kapazität %d  ->  %d", cfg::CARGO_VALUES[lvl], cfg::CARGO_VALUES[lvl + 1]);
                    break;
            }
            Rectangle row{x, y, bw, 78 * s};
            DrawRectangleRec(row, Color{36, 42, 54, 220});
            Text(names[t], row.x + 16 * s, row.y + 10 * s, 26 * s, TEXT);
            for (int k = 0; k < cfg::UPGRADE_LEVELS; ++k) {
                Color c = k < lvl ? ACCENT : Color{70, 76, 90, 255};
                DrawRectangle(static_cast<int>(row.x + 16 * s + k * 22 * s), static_cast<int>(row.y + 46 * s),
                              static_cast<int>(18 * s), static_cast<int>(10 * s), c);
            }
            Text(desc, row.x + 150 * s, row.y + 44 * s, 18 * s, MUTED);
            int cost = w.UpgradeCost(me, t);
            bool afford = cost >= 0 && me.balance >= cost;
            std::string bl = maxed ? std::string("MAXIMUM") : TextFormat("[%d]  %s Pkt.", t + 1, Num(cost).c_str());
            Rectangle b{row.x + row.width - 230 * s, row.y + 14 * s, 214 * s, 50 * s};
            bool buyHover = false;
            bool click = Button(b, bl, 22 * s, false, afford && interactive, &buyHover);
            bool keyBuy = IsKeyPressed(static_cast<KeyboardKey>(KEY_ONE + t));
            if (interactive && afford && (click || keyBuy)) {
                req.buy = t;
            } else if (interactive && !afford && (keyBuy || (buyHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)))) {
                g_audio.Play(Sfx::Denied);
            }
            y += 90 * s;
        }
        y += 6 * s;
        Rectangle ub{x, y, bw, 50 * s};
        if (Button(ub, "Abdocken  [F]", 22 * s, true, interactive)) req.undock = true;
    }

    if (interactive && (IsKeyPressed(KEY_F) || IsKeyPressed(KEY_ESCAPE))) req.undock = true;
    return req;
}

// ===========================================================================
// Endbildschirme
// ===========================================================================
static bool EndScreen(const World& w, const char* title, const char* sub, Color tc) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 160});
    Rectangle r{W * 0.5f - 340 * s, H * 0.5f - 230 * s, 680 * s, 460 * s};
    Panel(r);
    TextCentered(title, W * 0.5f, r.y + 30 * s, 50 * s, tc);
    TextCentered(sub, W * 0.5f, r.y + 96 * s, 22 * s, MUTED);
    float x = r.x + 60 * s, y = r.y + 150 * s, bw = r.width - 120 * s;
    auto row = [&](const char* k, const std::string& v) {
        Text(k, x, y, 24 * s, MUTED);
        TextRight(v, x + bw, y, 24 * s, TEXT);
        y += 38 * s;
    };
    row("Kontostand", Num(w.Me().balance));
    row("Insgesamt abgeliefert", Num(w.totalDelivered));
    row("Stationsstufe", TextFormat("%d / %d", w.StationStage(), cfg::STATION_STAGES - 1));
    row("Abschüsse", std::to_string(w.Me().kills));
    row("Spielzeit", Time(w.Me().playTime));
    Rectangle b{W * 0.5f - 170 * s, r.y + r.height - 76 * s, 340 * s, 52 * s};
    bool go = Button(b, "Zum Hauptmenü", 24 * s, true);
    if (w.outcomeT > 1.0f && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE))) go = true;
    return go;
}

bool DrawGameOver(const World& w) {
    return EndScreen(w, "SCHIFF ZERSTÖRT", "Dein Schiff wurde abgeschossen. Das Spiel ist zu Ende.", BAD);
}

bool DrawVictory(const World& w) {
    return EndScreen(w, "ZIEL ERREICHT!", "Du hast 100.000 Ressourcenpunkte erwirtschaftet.", GOOD);
}
