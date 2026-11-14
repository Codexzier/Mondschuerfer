// ---------------------------------------------------------------------------
// AlienStation.cpp – Alien-Station, die dritte Karte der Piloten (Teil der
// Klasse World, Deklarationen in World.hpp, Zahlenwerte in Config.hpp unter
// ALIEN_*).
//
// Wer auf der Weltraumkarte das Ziel erreicht (Orbit.cpp), fliegt hier weiter,
// bis die Station der Aliens besiegt ist:
//   - wie im All kein Boden, keine Kristalle, keine Händler und Missionen
//     (World::orbit gilt auch hier, dazu World::alien);
//   - im Norden die Alien-Station (cfg::ALIEN_STATION_MODEL) mit Schild
//     (stationHp) und Geschützen, die auf Piloten in ihrer Nähe schießen.
//     Geschosse der Spieler treffen sie (UpdateBullets). Sie muss in
//     cfg::ALIEN_TIME_LIMIT besiegt werden (Gesamtbedingung der Karte); sonst
//     repariert sie sich ganz, und die Zeit beginnt von vorn. Die Zeit läuft
//     nur, solange Piloten auf der Karte sind.
//   - besiegt: Belohnung nach Schadensanteil (wie beim Mutterschiff), die
//     Karte ist für alle Piloten auf ihr geschafft (PlayerState::alienGoal:
//     Kartenwechsel zwischen allen drei Karten frei). Nach
//     cfg::ALIEN_STATION_RESPAWN kommt eine neue Station;
//   - Alien-Schiffe starten an der Station (SpawnPirate): Typ 1 (Alien-Jäger,
//     ShipKind::Pirate, alien_spacecraft_01) und Typ 2 (Tarnschiff,
//     ShipKind::Elite, alien_spacecraft_02). Alle erholen sich nach
//     cfg::ALIEN_REGEN_DELAY ohne Treffer; Tarnschiffe sind getarnt
//     (Ship::cloak) und zeigen sich nur kurz nach einem Schuss oder Treffer.
//     Abschüsse bringen Punkte wie im All (OrbitBounty), ein Ziel im
//     Kontostand gibt es hier nicht;
//   - im Süden nur ein Trägerschiff (StationType::Dealer, "Trägerschiff"):
//     Reparatur und Kartenwechsel, kein Schiffswechsel und kein Kauf
//     (Hud.cpp, GameServer::HandleDockAction).
//
// Aufrufer: main.cpp (NewAlien beim Sprung von der Weltraumkarte), Save.cpp
// (NewAlien beim Laden), World::UpdateOrbit (UpdateAlien), World::UpdateBullets
// (DamageAlienStation), GameServer (eigene Welt für alle Piloten dort).
// Gezeichnet wird in src/Render.cpp (Tarnung: Effects.cpp und Assets.cpp),
// das HUD steht in src/Hud.cpp.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "Models.hpp"
#include <algorithm>
#include <cmath>

namespace {
// Oberkante der Station: knapp unter der Flughöhe wie in Orbit.cpp, die
// Schiffe fliegen über sie hinweg.
constexpr float DECK_Y = cfg::SHIP_ALTITUDE - 1.2f;

// Punkte mit Tausenderpunkten ("2.000") für die Meldungen
std::string Points(long long v) {
    std::string t = std::to_string(v);
    for (int i = static_cast<int>(t.size()) - 3; i > 0; i -= 3) t.insert(static_cast<size_t>(i), ".");
    return t;
}
} // namespace

void World::NewAlien(unsigned int newSeed, long long delivered) {
    *this = World{};
    seed = newSeed;
    rng.seed(seed);
    orbit = true;
    alien = true;
    // Insgesamt abgeliefert (vom Mond): bestimmt, wie stark die Aliens sind
    totalDelivered = delivered;
    ResolveModels();
    mdl.pirateA = mdl.pirateB = ModelId(cfg::ORBIT_PIRATE_MODEL);
    mdl.elite = ModelId(cfg::ALIEN_CLOAKER_MODEL);

    // Nur ein Trägerschiff, mitten im Süden (Reparatur und Kartenwechsel)
    AddCarrier(StationType::Dealer, "Trägerschiff", "carrier/carrier-a", 0.0f);
    lastStage = StationStage();
    PlaceAlienStation();

    // Ein Spieler mit neuem Schiff vor dem Trägerschiff (wie NewOrbit)
    players.push_back(PlayerState{});
    PlayerState& ps = players[0];
    Ship p = MakeShip(ShipKind::Player, StartPos());
    p.yaw = StartYaw();
    p.playerId = ps.id;
    ships.push_back(p);
    ps.shipId = p.id;
    ApplyUpgrades(ps, true);

    for (int i = 0; i < cfg::PIRATES_BASE; ++i) SpawnPirate();
    Notify(ps, "Die Aliens haben eine Raumstation im Norden! Besiege sie in 20 Minuten.", Color{215, 140, 255, 255});
    Notify(ps, "Alien-Schiffe erholen sich, Tarnschiffe sieht man nur als leichte Krümmung.", Color{200, 220, 255, 255});
}

// Neue Station der Aliens im Norden: Schild voll, die Zeit beginnt von vorn.
// Auch der Mehrspieler-Client ruft das (NetClient::ApplyOrbit), damit Lage
// und Größe zum Server passen.
void World::PlaceAlienStation() {
    SpaceStation st;
    st.model = ModelId(cfg::ALIEN_STATION_MODEL);
    st.scale = cfg::ALIEN_STATION_SCALE;
    const BoundingBox b = ModelBounds(st.model);
    st.pos = {0.0f, DECK_Y - b.max.y * st.scale, cfg::ALIEN_STATION_Z};
    // Trefferkreis: Mittel aus halber Breite und halber Länge (die Station
    // dreht sich langsam, Render.cpp)
    st.r = ((b.max.x - b.min.x) + (b.max.z - b.min.z)) * 0.25f * st.scale;
    spaceStations.assign(1, st);
    stationVariant = 2;
    stationHp = stationMaxHp = cfg::ALIEN_STATION_HP;
    stationDownT = 0.0f;
    alienT = cfg::ALIEN_TIME_LIMIT;
    alienDamage.clear();
}

// Ein Schritt der Alien-Station (aus UpdateOrbit): Erholung und Tarnung der
// Alien-Schiffe, nach dem Sieg die neue Station, sonst die Zeit und die
// Geschütze der Station.
void World::UpdateAlien(float dt) {
    for (Ship& s : ships) {
        if (s.faction != Faction::Pirate || s.dead) continue;
        if (s.crashing) {
            s.cloak = 0.0f;   // abgeschossen: taumelt sichtbar
            continue;
        }
        s.calmT += dt;
        if (s.calmT >= cfg::ALIEN_REGEN_DELAY && s.hp < s.maxHp) s.hp = std::min(s.maxHp, s.hp + s.maxHp / cfg::ALIEN_REGEN_TIME * dt);
        if (s.kind != ShipKind::Elite) continue;
        s.decloakT = std::max(0.0f, s.decloakT - dt);
        const float want = s.decloakT > 0.0f ? 0.0f : 1.0f;
        const float step = cfg::ALIEN_CLOAK_FADE * dt;
        s.cloak = want > s.cloak ? std::min(want, s.cloak + step) : std::max(want, s.cloak - step);
    }

    if (stationDownT > 0.0f) {
        stationDownT -= dt;
        if (stationDownT <= 0.0f) {
            PlaceAlienStation();
            for (PlayerState& ps : players)
                Notify(ps, "Eine neue Alien-Station ist aufgetaucht! Wieder 20 Minuten Zeit.", Color{215, 140, 255, 255});
        }
        return;
    }
    if (spaceStations.empty()) return;
    const SpaceStation st = spaceStations[0];

    // Zeit (nur mit Piloten auf der Karte), Warnung bei 5 und 1 Minute
    if (!players.empty()) {
        const float before = alienT;
        alienT -= dt;
        for (int minutes : {5, 1})
            if (before > minutes * 60.0f && alienT <= minutes * 60.0f)
                for (PlayerState& ps : players)
                    Notify(ps, Fmt("Noch %d %s, um die Alien-Station zu besiegen!", minutes, minutes == 1 ? "Minute" : "Minuten"),
                           Color{255, 170, 90, 255});
        if (alienT <= 0.0f) {
            stationHp = stationMaxHp;
            alienDamage.clear();
            alienT = cfg::ALIEN_TIME_LIMIT;
            for (PlayerState& ps : players) {
                Notify(ps, "Die Zeit ist abgelaufen! Die Alien-Station hat sich vollständig repariert.", Color{255, 90, 80, 255});
                Notify(ps, "Neuer Versuch: wieder 20 Minuten.", Color{200, 220, 255, 255});
            }
        }
    }

    // Geschütze: alle ALIEN_GUN_INTERVAL Sekunden je ein Schuss auf die
    // ALIEN_GUNS nächsten Piloten in Reichweite (Abstand zur Hülle)
    alienGunT -= dt;
    if (alienGunT > 0.0f) return;
    alienGunT = cfg::ALIEN_GUN_INTERVAL;
    std::vector<std::pair<float, int>> targets;   // Abstand, Index in ships
    for (int i = 0; i < static_cast<int>(ships.size()); ++i) {
        const Ship& s = ships[static_cast<size_t>(i)];
        if (s.kind != ShipKind::Player || !Targetable(s)) continue;
        const float d = Dist2D(s.pos, st.pos) - st.r;
        if (d <= cfg::ALIEN_GUN_RANGE) targets.push_back({d, i});
    }
    std::sort(targets.begin(), targets.end());
    for (size_t k = 0; k < targets.size() && k < static_cast<size_t>(cfg::ALIEN_GUNS); ++k)
        FireAlienGun(st, ships[static_cast<size_t>(targets[k].second)]);
}

// Ein Schuss der Station von ihrer Hülle aus auf target, mit Vorhalt wie bei
// den Geschütztürmen. ownerId -2 = die Station (kein Schiff, kein Turm): Das
// Geschoss trifft wie ein Piratengeschoss nur Nicht-Piraten.
void World::FireAlienGun(const SpaceStation& st, const Ship& target) {
    const float d = std::max(0.001f, Dist2D(target.pos, st.pos));
    const Vector3 out{(target.pos.x - st.pos.x) / d, 0.0f, (target.pos.z - st.pos.z) / d};
    const Vector3 from{st.pos.x + out.x * st.r * 0.85f, cfg::SHIP_ALTITUDE, st.pos.z + out.z * st.r * 0.85f};
    const Vector3 lead = Vector3Add(target.pos, Vector3Scale(target.vel, Dist2D(from, target.pos) / cfg::BULLET_SPEED));
    Bullet b;
    b.pos = from;
    b.vel = Vector3Scale(ForwardFromYaw(YawTowards(from, lead) + Rand(-0.03f, 0.03f)), cfg::BULLET_SPEED);
    b.vel.y = 0.0f;
    b.life = (cfg::ALIEN_GUN_RANGE + st.r * 0.15f) / cfg::BULLET_SPEED + 0.2f;
    b.damage = cfg::ALIEN_GUN_DAMAGE;
    b.faction = Faction::Pirate;
    b.ownerId = -2;
    bullets.push_back(b);
    SoundAt(Sfx::TurretShot, from);
}

// Treffer eines Spielergeschosses: Der Schaden je Spieler zählt für die
// Belohnung (höchstens der restliche Schild).
void World::DamageAlienStation(float dmg, int attackerPlayerId) {
    if (spaceStations.empty() || stationHp <= 0.0f) return;
    if (attackerPlayerId >= 0) {
        const float done = std::min(dmg, stationHp);
        auto it = std::find_if(alienDamage.begin(), alienDamage.end(), [&](const auto& e) { return e.first == attackerPlayerId; });
        if (it != alienDamage.end()) it->second += done;
        else alienDamage.push_back({attackerPlayerId, done});
    }
    stationHp -= dmg;
    if (stationHp > 0.0f) return;
    stationHp = 0.0f;
    AlienStationDown();
}

// Schild 0: große Explosion, die Station ist weg. Belohnung nach Anteil am
// Schaden (wie BossBounty, Orbit.cpp); für alle Piloten auf der Karte ist sie
// geschafft (alienGoal). Einzelspieler: gleich speichern (main.cpp).
void World::AlienStationDown() {
    const SpaceStation st = spaceStations[0];
    for (int k = 0; k < 10; ++k) {
        const float ang = k * (PI / 5.0f) + Rand(-0.3f, 0.3f), d = st.r * Rand(0.2f, 0.9f);
        Explosion({st.pos.x + sinf(ang) * d, DECK_Y, st.pos.z + cosf(ang) * d}, 3.0f);
    }
    Explosion({st.pos.x, DECK_Y, st.pos.z}, 4.0f);
    spaceStations.clear();
    stationDownT = cfg::ALIEN_STATION_RESPAWN;
    shake = 1.0f;
    float total = 0.0f;
    for (const auto& [id, dmg] : alienDamage)
        if (FindPlayer(id)) total += dmg;
    for (PlayerState& ps : players) {
        float mine = 0.0f;
        for (const auto& [id, dmg] : alienDamage)
            if (id == ps.id) mine = dmg;
        if (total > 0.0f && mine > 0.0f) {
            const int pct = static_cast<int>(lroundf(mine / total * 100.0f));
            const int points = static_cast<int>(lroundf(cfg::ALIEN_STATION_BOUNTY * mine / total));
            ps.balance += points;
            Notify(ps, Fmt("Alien-Station besiegt! Dein Anteil %d %%: +%s Punkte", pct, Points(points).c_str()), Color{255, 210, 130, 255});
        } else {
            Notify(ps, "Die Alien-Station ist besiegt!", Color{215, 140, 255, 255});
        }
        ++ps.goals;
        if (!ps.alienGoal) {
            ps.alienGoal = true;
            Notify(ps, "Karte geschafft! Am Trägerschiff kannst du jetzt zwischen allen drei Karten wechseln.", Color{110, 240, 170, 255});
        }
        Notify(ps, Fmt("Eine neue Alien-Station kommt in %.0f Sekunden.", cfg::ALIEN_STATION_RESPAWN), Color{200, 220, 255, 255});
        if (const Ship* sp = PlayerShip(ps)) {
            SoundAt(Sfx::CrashBoom, sp->pos, 1.0f, 0.0f, ps.id);
            SoundAt(Sfx::Victory, sp->pos, 1.0f, 0.6f, ps.id);
        }
    }
    alienDamage.clear();
    wantAutosave = !isServer && !simulation;
}
