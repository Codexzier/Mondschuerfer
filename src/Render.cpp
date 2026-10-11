// Darstellung der Spielwelt in 3D.
#include "World.hpp"
#include "Assets.hpp"
#include "rlgl.h"
#include <algorithm>
#include <cmath>

namespace {

// Flacher Ring auf dem Boden (beidseitig sichtbar)
void DrawGroundRing(Vector3 c, float rIn, float rOut, Color col, int segments = 64, float y = 0.12f) {
    rlBegin(RL_TRIANGLES);
    rlColor4ub(col.r, col.g, col.b, col.a);
    for (int i = 0; i < segments; ++i) {
        float a0 = 2 * PI * i / segments;
        float a1 = 2 * PI * (i + 1) / segments;
        Vector3 i0{c.x + cosf(a0) * rIn, y, c.z + sinf(a0) * rIn};
        Vector3 o0{c.x + cosf(a0) * rOut, y, c.z + sinf(a0) * rOut};
        Vector3 i1{c.x + cosf(a1) * rIn, y, c.z + sinf(a1) * rIn};
        Vector3 o1{c.x + cosf(a1) * rOut, y, c.z + sinf(a1) * rOut};
        rlVertex3f(i0.x, i0.y, i0.z); rlVertex3f(o0.x, o0.y, o0.z); rlVertex3f(o1.x, o1.y, o1.z);
        rlVertex3f(i0.x, i0.y, i0.z); rlVertex3f(o1.x, o1.y, o1.z); rlVertex3f(i1.x, i1.y, i1.z);
    }
    rlEnd();
}

// Gestrichelter Ring
void DrawDashedRing(Vector3 c, float r, float width, Color col, int dashes, float phase) {
    rlBegin(RL_TRIANGLES);
    rlColor4ub(col.r, col.g, col.b, col.a);
    const int sub = 4;
    for (int d = 0; d < dashes; ++d) {
        float start = (d + phase) * 2 * PI / dashes;
        float len = 2 * PI / dashes * 0.55f;
        for (int s = 0; s < sub; ++s) {
            float a0 = start + len * s / sub;
            float a1 = start + len * (s + 1) / sub;
            float rIn = r - width * 0.5f, rOut = r + width * 0.5f;
            rlVertex3f(c.x + cosf(a0) * rIn, 0.12f, c.z + sinf(a0) * rIn);
            rlVertex3f(c.x + cosf(a0) * rOut, 0.12f, c.z + sinf(a0) * rOut);
            rlVertex3f(c.x + cosf(a1) * rOut, 0.12f, c.z + sinf(a1) * rOut);
            rlVertex3f(c.x + cosf(a0) * rIn, 0.12f, c.z + sinf(a0) * rIn);
            rlVertex3f(c.x + cosf(a1) * rOut, 0.12f, c.z + sinf(a1) * rOut);
            rlVertex3f(c.x + cosf(a1) * rIn, 0.12f, c.z + sinf(a1) * rIn);
        }
    }
    rlEnd();
}

// Senkrechter Lichtstrahl als Markierung
void DrawBeam(Vector3 base, float height, float radius, Color col) {
    DrawCylinder({base.x, 0.2f, base.z}, radius * 0.6f, radius, height, 8, col);
}

void DrawShadow(Vector3 pos, float radius, float alpha) {
    DrawCylinder({pos.x, 0.06f, pos.z}, radius, radius, 0.02f, 18, Fade(BLACK, alpha));
}

float EaseOutBack(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

} // namespace

void World::Draw(const Camera3D& cam) const {
    g_assets.SetCamera(cam.position);
    const Vector3 focus = cam.target;
    const float cullR = drawRange;
    const Ship& player = Player();
    const PlayerState& me = Me();
    const bool orangeMoon = g_assets.GetMoonColor() == MoonColor::Orange;

    // --- Boden --------------------------------------------------------------
    // Beim orangefarbenen Mond (Originalfarbe) den Boden kaum abdunkeln
    g_assets.Draw(mdl.terrain, TRS({0, -0.05f, 0}, 0, 1400.0f), orangeMoon ? Color{150, 140, 136, 255} : Color{120, 120, 128, 255});
    g_assets.Draw(mdl.terrain, TRS({0, 0, 0}, 0, cfg::WORLD_HALF * 2.0f + 30.0f),
                  orangeMoon ? Color{225, 218, 214, 255} : Color{176, 176, 184, 255});

    // --- Dekoration -----------------------------------------------------------
    for (const auto& d : decos) {
        if (Dist2D(d.pos, focus) > cullR + d.scale * 0.6f) continue;
        if (d.yScale > 0.0f) {
            Matrix m = MatrixMultiply(MatrixScale(d.scale, d.yScale, d.scale), MatrixRotateY(d.yaw));
            m = MatrixMultiply(m, MatrixTranslate(d.pos.x, d.pos.y, d.pos.z));
            g_assets.Draw(d.model, m, Color{225, 225, 232, 255});
        } else {
            g_assets.Draw(d.model, d.pos, d.yaw, d.scale);
        }
    }

    // --- Stationen ------------------------------------------------------------
    for (const Station& st : stations) {
        if (Dist2D(st.pos, focus) > cullR + 30.0f) continue;
        const bool trade = st.type == StationType::Trade;
        int stage = trade ? StationStage() : 0;
        for (const auto& p : st.parts) {
            if (p.stage > stage) continue;
            float scale = p.scale;
            Vector3 off = p.offset;
            if (trade && p.stage == lastStage && stageAnim > 0.0f && p.stage > 0) {
                float t = std::clamp(1.0f - stageAnim * 1.4f, 0.0f, 1.0f);
                float k = EaseOutBack(t);
                scale *= std::max(0.001f, k);
                off.y *= std::max(0.0f, k);
            }
            Vector3 wp = Vector3Add(st.pos, off);
            g_assets.Draw(p.model, wp, p.yaw, scale);
        }
        static int turretDouble = -1, turretSingle = -1;
        if (turretDouble < 0) {
            turretDouble = g_assets.Id("turret_double");
            turretSingle = g_assets.Id("turret_single");
        }
        for (const auto& tu : st.turrets) {
            if (tu.stage > stage) continue;
            Vector3 wp = Vector3Add(st.pos, tu.offset);
            g_assets.Draw(tu.isDouble ? turretDouble : turretSingle, wp, tu.yaw, 2.2f);
        }
    }

    // --- Lagerstätten -----------------------------------------------------------
    for (const auto& d : deposits) {
        if (!d.revealed) continue;
        if (Dist2D(d.pos, focus) > cullR) continue;
        float pulse = 0.5f + 0.5f * sinf(time * 3.0f + d.id);
        float appear = EaseOutBack(std::clamp(d.revealT, 0.0f, 1.0f));
        float fill = 0.55f + 0.45f * static_cast<float>(d.amount) / std::max(1, d.initial);
        g_assets.Draw(d.model, d.pos, d.yaw, d.scale * appear * fill, WHITE,
                      Vector3{0.05f, 0.18f * pulse, 0.10f * pulse});
    }

    // --- Wracks ------------------------------------------------------------------
    for (const auto& w : wrecks) {
        if (Dist2D(w.pos, focus) > cullR) continue;
        g_assets.Draw(w.model, TRS(w.pos, w.yaw, w.tilt, w.tilt * 0.7f, 1.0f), Color{95, 92, 92, 255});
    }

    // --- Schiffe -------------------------------------------------------------------
    for (const auto& s : ships) {
        if (s.dead || s.hidden) continue;
        if (Dist2D(s.pos, focus) > cullR) continue;
        Color tint = WHITE;
        if (s.kind == ShipKind::Pirate) tint = Color{255, 150, 145, 255};
        else if (s.kind == ShipKind::Elite) tint = Color{190, 150, 255, 255};
        Vector3 em{s.hitFlash * 0.7f, s.hitFlash * 0.5f, s.hitFlash * 0.4f};
        float pitch = s.crashing ? -0.5f : -0.06f * s.thrustVisual;
        g_assets.Draw(s.model, TRS(s.pos, s.yaw, pitch, s.bank, 1.0f), tint, em);
    }

    // ===== Ab hier transparente / leuchtende Effekte ====================================
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthMask();

    // Schatten
    for (const auto& s : ships) {
        if (s.dead || s.hidden || Dist2D(s.pos, focus) > cullR) continue;
        float h = std::max(0.0f, s.pos.y);
        DrawShadow(s.pos, 1.1f, 0.16f * std::clamp(1.2f - h * 0.08f, 0.2f, 1.0f));
    }

    // Markierungen unter Schiffen
    if (!player.dead && !player.hidden && !player.crashing)
        DrawGroundRing(player.pos, 1.7f, 1.95f, Color{255, 170, 60, 130}, 40);
    for (const auto& s : ships) {
        if (s.dead || s.hidden || s.crashing || &s == &player) continue;
        if (Dist2D(s.pos, focus) > cullR) continue;
        if (s.kind == ShipKind::Player)   // andere Spieler (Mehrspieler)
            DrawGroundRing(s.pos, 1.7f, 1.95f, Color{110, 240, 170, 130}, 32);
        else if (s.faction == Faction::Pirate || s.hostileToPlayer)
            DrawGroundRing(s.pos, 1.7f, 1.95f, Color{255, 70, 60, 150}, 32);
        else
            DrawGroundRing(s.pos, 1.7f, 1.9f, Color{120, 200, 255, 100}, 32);
    }

    // Andockbereich der Stationen
    for (const Station& st : stations) {
        if (player.hidden) break;
        float d = Dist2D(player.pos, st.pos);
        if (d < 55.0f) {
            float a = std::clamp((55.0f - d) / 25.0f, 0.0f, 1.0f);
            Color c = st.type == StationType::Trade ? Color{255, 190, 80, static_cast<unsigned char>(120 * a)}
                                                    : Color{120, 210, 255, static_cast<unsigned char>(120 * a)};
            DrawDashedRing(st.pos, cfg::DOCK_RANGE, 0.35f, c, 28, time * 0.15f);
        }
    }

    BeginBlendMode(BLEND_ADDITIVE);

    // Scanner
    if (me.scanOn && !player.crashing && !player.hidden) {
        float f = fmodf(me.scanPulse, cfg::SCANNER_PULSE_TIME) / cfg::SCANNER_PULSE_TIME;
        float r = cfg::SCANNER_RADIUS * f;
        unsigned char a = static_cast<unsigned char>(200 * (1.0f - f * 0.7f));
        DrawGroundRing(player.pos, std::max(0.0f, r - 0.9f), r, Color{60, 255, 180, a}, 96);
        DrawGroundRing(player.pos, cfg::SCANNER_RADIUS - 0.25f, cfg::SCANNER_RADIUS, Color{60, 220, 160, 90}, 96);
    }

    // Leuchtmarkierungen für entdeckte Lagerstätten und Wracks
    for (const auto& d : deposits) {
        if (!d.revealed || Dist2D(d.pos, focus) > cullR) continue;
        float pulse = 0.6f + 0.4f * sinf(time * 2.5f + d.id);
        DrawBeam(d.pos, 9.0f, 0.35f, Color{40, 200, 120, static_cast<unsigned char>(70 * pulse)});
        DrawGroundRing(d.pos, 3.2f, 3.6f, Color{60, 255, 170, static_cast<unsigned char>(120 * pulse)}, 40);
    }
    for (const auto& w : wrecks) {
        if (Dist2D(w.pos, focus) > cullR) continue;
        float pulse = 0.6f + 0.4f * sinf(time * 3.0f + w.id);
        DrawBeam(w.pos, 8.0f, 0.3f, Color{220, 140, 40, static_cast<unsigned char>(70 * pulse)});
        DrawGroundRing(w.pos, 3.0f, 3.4f, Color{255, 170, 60, static_cast<unsigned char>(120 * pulse)}, 40);
    }

    // Abbaustrahlen
    auto beam = [&](Vector3 from, Vector3 to) {
        float w = 0.12f + 0.05f * sinf(time * 20.0f);
        DrawCylinderEx({from.x, from.y - 0.3f, from.z}, {to.x, 0.8f, to.z}, w, w * 1.8f, 8, Color{60, 255, 170, 150});
        DrawCylinderEx({from.x, from.y - 0.3f, from.z}, {to.x, 0.8f, to.z}, w * 2.5f, w * 4.0f, 8, Color{30, 140, 90, 60});
    };
    if (me.mining && !player.crashing && !player.hidden) {
        Vector3 t{};
        bool ok = false;
        for (const auto& d : deposits) if (me.mineKind == 1 && d.id == me.mineId) { t = d.pos; ok = true; }
        for (const auto& w : wrecks) if (me.mineKind == 2 && w.id == me.mineId) { t = w.pos; ok = true; }
        if (ok) beam(player.pos, t);
    }
    // Abbaustrahlen der anderen Schiffe (Händler und, im Mehrspieler, andere Spieler)
    for (const auto& s : ships) {
        if (&s != &player && s.mining && !s.hidden && Dist2D(s.pos, focus) < cullR) beam(s.pos, s.mineTarget);
    }

    // Triebwerke
    for (const auto& s : ships) {
        if (s.dead || s.hidden || Dist2D(s.pos, focus) > cullR) continue;
        Vector3 fwd = ForwardFromYaw(s.yaw);
        Vector3 side{fwd.z, 0, -fwd.x};
        float len = 0.25f + s.thrustVisual * 0.9f + 0.08f * sinf(time * 40.0f + s.id);
        float back = s.kind == ShipKind::Miner ? 1.4f : 1.15f;
        for (int k = -1; k <= 1; k += 2) {
            Vector3 base = Vector3Add(s.pos, Vector3Add(Vector3Scale(fwd, -back), Vector3Scale(side, 0.45f * k)));
            base.y += 0.35f;
            Vector3 tip = Vector3Add(base, Vector3Scale(fwd, -len));
            Color c = s.faction == Faction::Pirate ? Color{255, 90, 70, 200} : Color{255, 170, 60, 200};
            DrawCylinderEx(base, tip, 0.18f, 0.02f, 6, c);
        }
    }

    // Geschosse
    for (const auto& b : bullets) {
        if (Dist2D(b.pos, focus) > cullR) continue;
        Color c;
        if (b.ownerId == -1) c = Color{110, 220, 255, 255};
        else if (b.faction == Faction::Player) c = Color{255, 210, 90, 255};
        else if (b.faction == Faction::Pirate) c = Color{255, 70, 60, 255};
        else c = Color{255, 240, 150, 255};
        Vector3 dir = Vector3Normalize(b.vel);
        Vector3 tail = Vector3Subtract(b.pos, Vector3Scale(dir, 1.6f));
        DrawCylinderEx(tail, b.pos, 0.06f, 0.14f, 6, c);
        DrawCylinderEx(tail, b.pos, 0.18f, 0.3f, 6, Fade(c, 0.35f));
    }

    // Partikel
    for (const auto& p : particles) {
        if (!p.glow) continue;
        float f = p.life / p.maxLife;
        float s = p.size * (0.4f + 0.6f * f);
        DrawCube(p.pos, s, s, s, Fade(p.color, f));
    }
    EndBlendMode();
    for (const auto& p : particles) {
        if (p.glow) continue;
        float f = p.life / p.maxLife;
        float s = p.size * (p.gravity > 0 ? 1.0f : (1.6f - f * 0.6f));
        DrawCube(p.pos, s, s, s, Fade(p.color, std::min(1.0f, f * 1.5f)));
    }

    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}
