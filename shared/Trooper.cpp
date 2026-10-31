// ---------------------------------------------------------------------------
// Trooper.cpp: Trupp zu Fuß (FPS-Modus) – Aussteigen und Einsteigen, Bewegung
// mit Mondschwerkraft, Hindernisse aus Deko und Stationsteilen, Schießen und
// Nachladen. Datentypen in World.hpp (Trooper, TrooperInput, TrooperBlock).
//
// Wer benutzt die Datei?
//   - World::Update ruft UpdateTroopers auf (nach den Spielern).
//   - World::UpdateBullets fragt TrooperBlocked (Treffer der Bodengeschosse).
//   - src/main.cpp: StartTrooper/EndTrooper (Pausenmenü), TrooperEye/
//     TrooperAim für die Ego-Kamera. src/Hud.cpp: NearestGate.
//
// Bisher nur im Einzelspieler: Der Spieler steigt über die Pause aus dem
// Schiff aus und läuft auf der Mondkarte herum. Rollenwahl, Stargates und
// Mehrspieler kommen in späteren Schritten (siehe FPS-Plan).
//
// Wie bei den Schiffen rechnet UpdateTrooperPhysics ohne Zufall, damit der
// Mehrspieler-Client später die eigene Figur genauso vorhersagen kann wie der
// Server. Die Hindernisse kommen deshalb nur aus Deko, Stationen und
// ModelBounds.inc (das kennen Server und Client gleich).
//
// Abhängigkeiten: World.hpp, Models.hpp (ModelId, ModelBounds). Keine Grafik.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "Models.hpp"
#include <algorithm>
#include <cmath>

namespace {
// Nächster Punkt der Strecke a-b zum Punkt p (auf dem Boden)
Vector2 ClosestOnSeg(Vector2 p, Vector2 a, Vector2 b) {
    const Vector2 ab = Vector2Subtract(b, a);
    const float len2 = Vector2LengthSqr(ab);
    const float t = len2 > 0.0f ? std::clamp(Vector2DotProduct(Vector2Subtract(p, a), ab) / len2, 0.0f, 1.0f) : 0.0f;
    return Vector2Add(a, Vector2Scale(ab, t));
}

bool InField(Vector3 p) { return fabsf(p.x) <= cfg::WORLD_HALF && fabsf(p.z) <= cfg::WORLD_HALF; }
} // namespace

Vector3 TrooperEye(const Trooper& t) { return {t.pos.x, t.pos.y + cfg::TROOPER_EYE, t.pos.z}; }

Vector3 TrooperAim(const Trooper& t) {
    const Vector3 f = ForwardFromYaw(t.yaw);
    const float c = cosf(t.pitch);
    return {f.x * c, sinf(t.pitch), f.z * c};
}

Trooper* World::FindTrooper(int id) {
    for (auto& t : troopers)
        if (t.id == id) return &t;
    return nullptr;
}

const Trooper* World::FindTrooper(int id) const {
    for (const auto& t : troopers)
        if (t.id == id) return &t;
    return nullptr;
}

// ===========================================================================
// Aussteigen und Einsteigen
// ===========================================================================
// Abbau und Scanner enden beim Aussteigen; das Schiff bleibt versteckt
// stehen (hidden: nicht angreifbar, UpdatePlayer überspringt es).
bool World::StartTrooper(PlayerState& ps) {
    Ship* sp = PlayerShip(ps);
    if (ps.role == PlayerRole::Trooper || !sp || sp->dead || sp->crashing || sp->hidden || ps.dockedAt >= 0) return false;
    if (ps.mining) SoundAt(Sfx::MineAbort, sp->pos, 1.0f, 0.0f, ps.id);
    ps.mining = false;
    sp->mining = false;
    ps.scanOn = false;
    BuildTrooperBlocks();
    Trooper t;
    t.id = nextTrooperId++;
    t.playerId = ps.id;
    t.pos = {sp->pos.x, 0.0f, sp->pos.z};
    t.yaw = sp->yaw;
    troopers.push_back(t);
    ps.role = PlayerRole::Trooper;
    ps.trooperId = t.id;
    ps.trooperInput = TrooperInput{};
    ps.trooperInput.yaw = t.yaw;
    sp->vel = {0.0f, 0.0f, 0.0f};
    sp->hidden = true;
    return true;
}

void World::EndTrooper(PlayerState& ps) {
    if (ps.role != PlayerRole::Trooper) return;
    Ship* sp = PlayerShip(ps);
    if (const Trooper* t = FindTrooper(ps.trooperId); t && sp) {
        sp->pos.x = t->pos.x;
        sp->pos.z = t->pos.z;
        sp->yaw = t->yaw;
    }
    troopers.erase(std::remove_if(troopers.begin(), troopers.end(), [&](const Trooper& t) { return t.id == ps.trooperId; }),
                   troopers.end());
    if (sp) sp->hidden = false;
    ps.role = PlayerRole::Pilot;
    ps.trooperId = -1;
    ps.trooperInput = TrooperInput{};
}

// ===========================================================================
// Hindernisse
// ===========================================================================
// Jedes Deko- und Stationsteil wird zu einer Kapsel: die Grundfläche aus
// ModelBounds (gedreht und skaliert wie beim Zeichnen), entlang der längeren
// Seite gestreckt, Radius = halbe kürzere Seite, oben flach in Höhe der
// Oberkante. Ausgelassen werden flache Teile (unter 0,3 m), Teile über
// Kopfhöhe, Krater (man läuft über den Rand hinweg), Rover (fahren weg)
// und alles außerhalb des Spielfelds (Felswall, dort endet das Feld ohnehin).
// Stationsteile zählen nur, wenn sie bei der aktuellen Ausbaustufe stehen;
// zu Fuß kann man nichts abliefern, die Stufe ändert sich also nicht.
void World::BuildTrooperBlocks() {
    trooperBlocks.clear();
    auto add = [&](int model, Vector3 pos, float yaw, float scale, float yScale) {
        const BoundingBox b = ModelBounds(model);
        const float ys = yScale > 0.0f ? yScale : scale;
        const float bottom = pos.y + b.min.y * ys, top = pos.y + b.max.y * ys;
        if (top < 0.3f || bottom > cfg::TROOPER_HEIGHT) return;
        const float cx = (b.min.x + b.max.x) * 0.5f * scale, cz = (b.min.z + b.max.z) * 0.5f * scale;
        const float hx = (b.max.x - b.min.x) * 0.5f * scale, hz = (b.max.z - b.min.z) * 0.5f * scale;
        Vector3 la{cx, 0.0f, cz}, lb{cx, 0.0f, cz};
        float r;
        if (hx >= hz) {
            la.x -= hx - hz;
            lb.x += hx - hz;
            r = hz;
        } else {
            la.z -= hz - hx;
            lb.z += hz - hx;
            r = hx;
        }
        const Matrix rot = MatrixRotateY(yaw);
        const Vector3 wa = Vector3Add(pos, Vector3Transform(la, rot)), wb = Vector3Add(pos, Vector3Transform(lb, rot));
        trooperBlocks.push_back({{wa.x, wa.z}, {wb.x, wb.z}, r, top});
    };
    const int rover = ModelId("rover"), craterA = ModelId("crater"), craterB = ModelId("craterLarge");
    for (const Deco& d : decos) {
        if (!InField(d.pos) || d.model == rover || d.model == craterA || d.model == craterB) continue;
        add(d.model, d.pos, d.yaw, d.scale, d.yScale);
    }
    const int stage = StationStage();
    for (const Station& st : stations) {
        const int stStage = st.type == StationType::Trade ? stage : 0;
        for (const StationPart& p : st.parts)
            if (p.stage <= stStage) add(p.model, Vector3Add(st.pos, p.offset), p.yaw, p.scale, 0.0f);
    }
}

bool World::TrooperBlocked(Vector3 p) const {
    const Vector2 q{p.x, p.z};
    for (const TrooperBlock& o : trooperBlocks)
        if (p.y < o.h && Vector2Distance(q, ClosestOnSeg(q, o.a, o.b)) < o.r) return true;
    return false;
}

bool World::NearestGate(Vector3 p, Vector3* out) const {
    static const int gateA = ModelId("gate_simple"), gateB = ModelId("gate_complex");
    float best = 1e9f;
    for (const Deco& d : decos) {
        if (d.model != gateA && d.model != gateB) continue;
        const float dist = Dist2D(p, d.pos);
        if (dist < best) {
            best = dist;
            *out = d.pos;
        }
    }
    return best < 1e9f;
}

// ===========================================================================
// Bewegung
// ===========================================================================
// Am Boden folgt das Tempo der Eingabe mit TROOPER_ACCEL, in der Luft nur mit
// einem Viertel davon. Hindernisse schieben die Figur seitlich hinaus und
// nehmen das Tempo in ihre Richtung weg. Niedrige Hindernisse (bis
// TROOPER_STEP über den Füßen) werden bestiegen: Steht die Mitte der Figur
// darüber, ist ihre Oberkante der Boden. Läuft die Figur von so einer Kante
// herunter, setzt sie gleich unten auf (statt langsam zu fallen).
void World::UpdateTrooperPhysics(Trooper& t, float dt, const TrooperInput& in) {
    t.yaw = in.yaw;
    t.pitch = std::clamp(in.pitch, -cfg::TROOPER_PITCH_MAX, cfg::TROOPER_PITCH_MAX);
    const Vector3 fwd = ForwardFromYaw(t.yaw);
    const Vector3 right{-fwd.z, 0.0f, fwd.x};
    Vector3 wish = Vector3Add(Vector3Scale(fwd, std::clamp(in.forward, -1.0f, 1.0f)),
                              Vector3Scale(right, std::clamp(in.strafe, -1.0f, 1.0f)));
    const float wl = Vector3Length(wish);
    if (wl > 1.0f) wish = Vector3Scale(wish, 1.0f / wl);
    const float speed = in.sprint && in.forward > 0.0f ? cfg::TROOPER_SPRINT : cfg::TROOPER_WALK;
    const float maxDv = cfg::TROOPER_ACCEL * (t.onGround ? 1.0f : cfg::TROOPER_AIR_CONTROL) * dt;
    Vector2 dv{wish.x * speed - t.vel.x, wish.z * speed - t.vel.z};
    const float dl = Vector2Length(dv);
    if (dl > maxDv) dv = Vector2Scale(dv, maxDv / dl);
    t.vel.x += dv.x;
    t.vel.z += dv.y;
    const bool wasOnGround = t.onGround;
    if (in.jump && t.onGround) {
        t.vel.y = cfg::TROOPER_JUMP;
        t.onGround = false;
    }
    t.vel.y -= cfg::MOON_GRAVITY * dt;
    t.pos = Vector3Add(t.pos, Vector3Scale(t.vel, dt));
    const float lim = cfg::WORLD_HALF - 2.0f;
    t.pos.x = std::clamp(t.pos.x, -lim, lim);
    t.pos.z = std::clamp(t.pos.z, -lim, lim);

    float ground = 0.0f;
    for (const TrooperBlock& o : trooperBlocks) {
        const Vector2 p{t.pos.x, t.pos.z};
        const Vector2 c = ClosestOnSeg(p, o.a, o.b);
        Vector2 d = Vector2Subtract(p, c);
        float dist = Vector2Length(d);
        const float reach = o.r + cfg::TROOPER_RADIUS;
        if (dist >= reach) continue;
        if (o.h <= t.pos.y + cfg::TROOPER_STEP) {
            if (dist < o.r) ground = std::max(ground, o.h);
            continue;
        }
        // Genau auf der Mittellinie: entgegen der Blickrichtung hinaus
        if (dist < 1e-4f) {
            d = {-fwd.x, -fwd.z};
            dist = 1.0f;
        }
        const Vector2 n = Vector2Scale(d, 1.0f / dist);
        t.pos.x = c.x + n.x * reach;
        t.pos.z = c.y + n.y * reach;
        const float vn = t.vel.x * n.x + t.vel.z * n.y;
        if (vn < 0.0f) {
            t.vel.x -= vn * n.x;
            t.vel.z -= vn * n.y;
        }
    }
    if (t.pos.y <= ground || (wasOnGround && t.vel.y <= 0.0f && t.pos.y - ground <= cfg::TROOPER_STEP)) {
        t.pos.y = ground;
        t.vel.y = 0.0f;
        t.onGround = true;
    } else {
        t.onGround = false;
    }
}

// ===========================================================================
// Schritt aller Figuren: Bewegung, Nachladen, Schießen
// ===========================================================================
// Nachladen mit R oder automatisch beim Abdrücken mit leerem Magazin. Ohne
// Ersatzmunition gibt es nur den Ton "Abgelehnt".
void World::UpdateTroopers(float dt) {
    for (auto& t : troopers) {
        const PlayerState* ps = FindPlayer(t.playerId);
        const TrooperInput in = ps ? ps->trooperInput : TrooperInput{};
        UpdateTrooperPhysics(t, dt, in);
        t.age += dt;
        t.fireCd -= dt;
        t.hitFlash = std::max(0.0f, t.hitFlash - dt * 3.0f);
        if (t.reloadT > 0.0f) {
            t.reloadT -= dt;
            if (t.reloadT <= 0.0f) {
                const int n = std::min(cfg::TROOPER_MAG - t.mag, t.ammo);
                t.mag += n;
                t.ammo -= n;
                t.reloadT = 0.0f;
                SoundAt(Sfx::UiConfirm, t.pos, 0.6f, 0.0f, t.playerId);
            }
        } else if ((in.reload || (in.fire && t.mag == 0)) && t.mag < cfg::TROOPER_MAG && t.ammo > 0) {
            t.reloadT = cfg::TROOPER_RELOAD;
            SoundAt(Sfx::UiMove, t.pos, 0.8f, 0.0f, t.playerId);
        }
        if (!in.fire || t.reloadT > 0.0f || t.fireCd > 0.0f) continue;
        if (t.mag > 0) {
            FireTrooperBullet(t);
            --t.mag;
            t.fireCd = cfg::TROOPER_FIRE_INTERVAL;
        } else if (t.ammo == 0) {
            SoundAt(Sfx::Denied, t.pos, 0.6f, 0.0f, t.playerId);
            t.fireCd = 0.5f;
        }
    }
}

// Das Geschoss startet etwas rechts unterhalb der Augen (an der Waffe) und
// fliegt parallel zur Blickrichtung: Es trifft knapp neben dem Fadenkreuz.
void World::FireTrooperBullet(Trooper& t) {
    const Vector3 dir = TrooperAim(t);
    const Vector3 fwd = ForwardFromYaw(t.yaw);
    const Vector3 right{-fwd.z, 0.0f, fwd.x};
    Bullet b;
    b.pos = Vector3Add(Vector3Add(TrooperEye(t), Vector3Scale(dir, 0.6f)), Vector3Add(Vector3Scale(right, 0.12f), {0.0f, -0.1f, 0.0f}));
    b.vel = Vector3Scale(dir, cfg::TROOPER_BULLET_SPEED);
    b.life = cfg::TROOPER_BULLET_LIFE;
    b.damage = cfg::TROOPER_DAMAGE;
    b.faction = t.faction;
    b.ownerPlayer = t.playerId;
    b.ground = true;
    bullets.push_back(b);
    SoundAt(Sfx::PlayerShot, t.pos, 0.7f);
}
