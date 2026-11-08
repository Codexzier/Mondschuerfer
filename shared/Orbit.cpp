// ---------------------------------------------------------------------------
// Orbit.cpp – Weltraumkarte der Piloten (Teil der Klasse World, Deklarationen
// in World.hpp, Zahlenwerte in Config.hpp unter ORBIT_*).
//
// Wer auf dem Mond das Ziel erreicht (cfg::GOAL_POINTS), fliegt hier weiter:
//   - kein Boden, keine Kristalle, keine Händler und keine Missionen;
//   - in der Mitte die Raumstation (station-a bzw. station-b) mit
//     Strukturpunkten (World::stationHp);
//   - im Süden das Trägerschiff mit Werft und Raumschiffsverkäufer (zwei
//     Stationen, Andocken wie auf dem Mond, mit Geschütztürmen);
//   - Piraten: Nur ihre Abschüsse bringen Punkte (OrbitBounty);
//   - ab und an ein Asteroid, der langsam auf die Station zufliegt. Trifft er
//     sie, verliert sie Strukturpunkte. Bei 0 explodiert sie: Alle Piloten
//     auf der Karte haben Kontostand 0, nach cfg::ORBIT_STATION_RESPAWN kommt
//     eine neue Station (abwechselnd das andere Modell).
// Ziel: wieder cfg::GOAL_POINTS auf dem Konto. Die nächste Karte gibt es noch
// nicht; bis dahin beginnt danach eine neue Runde auf dieser Karte. Ab dann
// ist der Kartenwechsel frei (PlayerState::orbitGoal): angedockt an der
// Handelsstation bzw. am Trägerschiff (Reiter "Karte"), mit demselben
// Kontostand; das Ziel auf dem Mond ist dann ebenfalls eine neue Runde dort.
//
// Aufrufer: main.cpp (NewOrbit und CarryPilot beim Sprung ins All), Save.cpp
// (NewOrbit beim Laden), World::Update (UpdateOrbit), World::UpdateBullets
// (DamageAsteroid), World::DamageShip (OrbitBounty).
// Gezeichnet wird in src/Render.cpp, das HUD steht in src/Hud.cpp.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "Models.hpp"
#include <algorithm>
#include <cmath>

namespace {
// Oberkante von Raumstation und Trägerschiff: knapp unter der Flughöhe, die
// Schiffe fliegen über sie hinweg (wie auf dem Mond gibt es keine Kollision).
constexpr float DECK_Y = cfg::SHIP_ALTITUDE - 1.2f;
constexpr float CARRIER_SCALE = 50.0f;   // Trägerschiff: 50 m lang
constexpr float DOCK_SPACING = 16.0f;    // Andockplätze 16 m links und rechts der Schiffsmitte

// Punkte mit Tausenderpunkten ("2.000") für die Meldungen
std::string Points(long long v) {
    std::string t = std::to_string(v);
    for (int i = static_cast<int>(t.size()) - 3; i > 0; i -= 3) t.insert(static_cast<size_t>(i), ".");
    return t;
}
} // namespace

Vector3 World::StartPos() const {
    return orbit ? Vector3{0.0f, cfg::SHIP_ALTITUDE, cfg::ORBIT_CARRIER_Z - 26.0f} : Vector3{0.0f, cfg::SHIP_ALTITUDE, 24.0f};
}

// Mond: Blick nach Süden, weg von der Station; Weltraumkarte: nach Norden zur Station
float World::StartYaw() const { return orbit ? 0.0f : PI; }

void World::NewOrbit(unsigned int newSeed, long long delivered) {
    *this = World{};
    seed = newSeed;
    rng.seed(seed);
    orbit = true;
    // Insgesamt abgeliefert (vom Mond): bestimmt, wie stark die Piraten sind
    totalDelivered = delivered;
    ResolveModels();

    // Trägerschiff längs der x-Achse; sein Rumpf ist ein Bauteil der Werft.
    // Auf dem Deck zwei Andockplätze: Werft im Westen, Raumschiffsverkäufer
    // im Osten, je mit zwei Geschütztürmen (Schutz am Rückzugspunkt).
    // Die Stationen liegen in Deckhöhe (Render.cpp: Andockring auf dem Deck).
    const int carrier = ModelId("carrier/carrier-a");
    const float carrierY = -ModelBounds(carrier).max.y * CARRIER_SCALE;
    Station yard;
    yard.type = StationType::Shipyard;
    yard.name = "Werft";
    yard.pos = {-DOCK_SPACING, DECK_Y, cfg::ORBIT_CARRIER_Z};
    yard.parts.push_back({carrier, {DOCK_SPACING, carrierY, 0.0f}, PI * 0.5f, CARRIER_SCALE, 0});
    Station dealer;
    dealer.type = StationType::Dealer;
    dealer.name = "Raumschiffsverkäufer";
    dealer.pos = {DOCK_SPACING, DECK_Y, cfg::ORBIT_CARRIER_Z};
    for (Station* st : {&yard, &dealer}) {
        st->turrets.push_back({{-6.0f, 0.0f, -4.0f}, 0.0f, 0.0f, 0, true});
        st->turrets.push_back({{6.0f, 0.0f, 4.0f}, PI, 0.0f, 0, false});
    }
    stations.push_back(yard);
    stations.push_back(dealer);
    lastStage = StationStage();
    PlaceOrbitStation();

    // Ein Spieler mit neuem Schiff vor dem Trägerschiff (wie NewGame)
    players.push_back(PlayerState{});
    PlayerState& ps = players[0];
    Ship p = MakeShip(ShipKind::Player, StartPos());
    p.yaw = StartYaw();
    p.playerId = ps.id;
    ships.push_back(p);
    ps.shipId = p.id;
    ApplyUpgrades(ps, true);

    for (int i = 0; i < cfg::PIRATES_BASE; ++i) SpawnPirate();
    asteroidT = cfg::ORBIT_ASTEROID_FIRST;
    Notify(ps, "Willkommen im All! Beschütze die Raumstation vor Asteroiden.", Color{150, 230, 255, 255});
    Notify(ps, "Punkte gibt es nur für abgeschossene Piraten. Ziel: 100.000.", Color{200, 220, 255, 255});
}

void World::CarryPilot(PlayerState& ps, const PlayerState& from, bool keepBalance) {
    ps.name = from.name;
    // Ziel auf dieser Karte: wieder 100.000, der Kontostand beginnt bei 0
    // (Kartenwechsel: ein Konto für beide Karten)
    ps.balance = keepBalance ? from.balance : 0;
    ps.orbitGoal = from.orbitGoal;
    ps.delivered = from.delivered;
    ps.lives = from.lives;
    for (int u = 0; u < cfg::UP_COUNT; ++u) ps.upgrades[u] = from.upgrades[u];
    ps.shipType = from.shipType;
    ps.hangar = from.hangar;
    ps.playTime = from.playTime;
    ps.kills = from.kills;
    ps.deaths = from.deaths;
    ps.goals = from.goals;
    ps.lastRole = from.lastRole;
    ps.sgMates = from.sgMates;
    ps.sgLog = from.sgLog;
    ps.sgTotal = from.sgTotal;
    ApplyUpgrades(ps, true);
}

void World::PlaceOrbitStation() {
    SpaceStation st;
    st.model = ModelId(stationVariant == 0 ? "station/station-a" : "station/station-b");
    st.scale = cfg::ORBIT_STATION_SCALE;
    const BoundingBox b = ModelBounds(st.model);
    st.pos = {0.0f, DECK_Y - b.max.y * st.scale, 0.0f};
    st.r = std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f * st.scale;
    spaceStations.assign(1, st);
    stationHp = stationMaxHp = cfg::ORBIT_STATION_HP;
    stationDownT = 0.0f;
}

// Ein Schritt der Weltraumkarte (aus World::Update): Asteroiden kommen und
// fliegen, Einschläge an der Station; nach einer Explosion die neue Station.
void World::UpdateOrbit(float dt) {
    if (stationDownT > 0.0f) {
        stationDownT -= dt;
        if (stationDownT <= 0.0f) {
            PlaceOrbitStation();
            asteroidT = cfg::ORBIT_ASTEROID_FIRST;
            for (PlayerState& ps : players) Notify(ps, "Eine neue Raumstation ist eingetroffen!", Color{150, 230, 255, 255});
        }
        return;   // ohne Station keine Asteroiden
    }
    if (spaceStations.empty()) return;
    const SpaceStation st = spaceStations[0];
    asteroidT -= dt;
    if (asteroidT <= 0.0f) {
        SpawnAsteroid();
        asteroidT = cfg::ORBIT_ASTEROID_INTERVAL * Rand(0.8f, 1.2f);
    }
    bool destroyed = false;
    for (Asteroid& a : asteroids) {
        if (a.dead) continue;
        a.pos = Vector3Add(a.pos, Vector3Scale(a.vel, dt));
        a.yaw += a.spin * dt;
        a.hitFlash = std::max(0.0f, a.hitFlash - dt * 3.0f);
        // Einschlag: Der Asteroid erreicht die Hülle (zur Hälfte überlappend)
        if (Dist2D(a.pos, st.pos) > st.r + a.r * 0.5f) continue;
        a.dead = true;
        Explosion({a.pos.x, cfg::SHIP_ALTITUDE, a.pos.z}, 2.5f);
        SoundAt(Sfx::CrashBoom, a.pos);
        stationHp = std::max(0.0f, stationHp - cfg::ORBIT_ASTEROID_DAMAGE);
        if (stationHp <= 0.0f) {
            destroyed = true;
            break;
        }
        const int pct = static_cast<int>(ceilf(stationHp / stationMaxHp * 100.0f));
        for (PlayerState& ps : players)
            Notify(ps, Fmt("Asteroid eingeschlagen! Struktur der Station: %d %%", pct), Color{255, 110, 90, 255});
    }
    if (destroyed) StationExplodes();
    asteroids.erase(std::remove_if(asteroids.begin(), asteroids.end(), [](const Asteroid& a) { return a.dead; }), asteroids.end());
}

// Neuer Asteroid in ORBIT_ASTEROID_DIST um die Station, aus einer zufälligen
// Richtung; er fliegt geradewegs auf ihre Mitte zu. Alle Piloten bekommen
// eine Warnung mit der Himmelsrichtung.
void World::SpawnAsteroid() {
    if (spaceStations.empty()) return;
    const SpaceStation& st = spaceStations[0];
    Asteroid a;
    a.id = nextId++;
    a.model = ModelId(RandInt(0, 1) == 0 ? "meteor" : "meteor_detailed");
    a.scale = cfg::ORBIT_ASTEROID_SIZE * Rand(0.85f, 1.15f);
    const BoundingBox b = ModelBounds(a.model);
    a.r = std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f * a.scale;
    // Winkel 0 = Süden (+z), PI / 2 = Osten (+x); Mitte des Modells in Flughöhe
    const float ang = Rand(-PI, PI);
    a.pos = {st.pos.x + sinf(ang) * cfg::ORBIT_ASTEROID_DIST, cfg::SHIP_ALTITUDE - 0.5f * b.max.y * a.scale,
             st.pos.z + cosf(ang) * cfg::ORBIT_ASTEROID_DIST};
    a.vel = {-sinf(ang) * cfg::ORBIT_ASTEROID_SPEED, 0.0f, -cosf(ang) * cfg::ORBIT_ASTEROID_SPEED};
    a.yaw = Rand(0.0f, 2.0f * PI);
    a.spin = Rand(-0.6f, 0.6f);
    a.hp = a.maxHp = cfg::ORBIT_ASTEROID_HP;
    asteroids.push_back(a);
    static const char* const kDirs[8] = {"Süden", "Südosten", "Osten", "Nordosten", "Norden", "Nordwesten", "Westen", "Südwesten"};
    const int dir = (static_cast<int>(lroundf(ang / (PI / 4.0f))) + 8) % 8;
    for (PlayerState& ps : players) {
        Notify(ps, Fmt("Asteroid im Anflug aus %s! Zerstöre ihn vor dem Einschlag.", kDirs[dir]), Color{255, 170, 90, 255});
        if (const Ship* sp = PlayerShip(ps)) SoundAt(Sfx::LowShield, sp->pos, 1.0f, 0.0f, ps.id);
    }
}

void World::DamageAsteroid(Asteroid& a, float dmg, int attackerPlayerId) {
    if (a.dead) return;
    a.hp -= dmg;
    a.hitFlash = 1.0f;
    if (a.hp > 0.0f) return;
    a.dead = true;
    Explosion({a.pos.x, cfg::SHIP_ALTITUDE, a.pos.z}, 2.0f);
    SoundAt(Sfx::ShipExplode, a.pos);
    for (PlayerState& ps : players)
        Notify(ps, ps.id == attackerPlayerId ? "Asteroid zerstört! Die Station ist in Sicherheit." : "Ein Asteroid wurde zerstört.",
               Color{150, 255, 170, 255});
}

// Strukturpunkte 0: große Explosion, die Station ist weg (keine Asteroiden
// mehr, bis die neue da ist), und alle Piloten auf der Karte verlieren ihren
// Kontostand.
void World::StationExplodes() {
    if (spaceStations.empty()) return;
    const SpaceStation st = spaceStations[0];
    for (int k = 0; k < 8; ++k) {
        const float ang = k * (PI / 4.0f) + Rand(-0.3f, 0.3f), d = st.r * Rand(0.2f, 0.9f);
        Explosion({st.pos.x + sinf(ang) * d, DECK_Y, st.pos.z + cosf(ang) * d}, 3.0f);
    }
    Explosion({st.pos.x, DECK_Y, st.pos.z}, 4.0f);
    spaceStations.clear();
    asteroids.clear();
    stationHp = 0.0f;
    stationDownT = cfg::ORBIT_STATION_RESPAWN;
    stationVariant ^= 1;
    shake = 1.0f;
    for (PlayerState& ps : players) {
        ps.balance = 0;
        if (const Ship* sp = PlayerShip(ps)) {
            SoundAt(Sfx::CrashBoom, sp->pos, 1.0f, 0.0f, ps.id);
            SoundAt(Sfx::ShipExplode, sp->pos, 1.0f, 0.5f, ps.id);
        }
        Notify(ps, "Die Raumstation ist explodiert! Alle Kontostände stehen auf 0.", Color{255, 90, 80, 255});
        Notify(ps, Fmt("Eine neue Raumstation kommt in %.0f Sekunden.", cfg::ORBIT_STATION_RESPAWN), Color{200, 220, 255, 255});
    }
}

bool World::CanSwitchMap(const PlayerState& ps) const {
    if (!ps.orbitGoal || ps.dockedAt < 0 || ps.dockedAt >= static_cast<int>(stations.size())) return false;
    const StationType t = stations[static_cast<size_t>(ps.dockedAt)].type;
    return orbit ? t == StationType::Shipyard || t == StationType::Dealer : t == StationType::Trade;
}

// Abschuss eines Piraten durch einen Spieler: Punkte aufs Konto. Ist das
// Ziel erreicht, beginnt eine neue Runde auf dieser Karte (im Mehrspieler
// mit Eintrag in die Bestenliste, World::GoalReached). Beim ersten Mal wird
// der Kartenwechsel frei.
void World::OrbitBounty(PlayerState& ps, const Ship& s) {
    const bool elite = s.kind == ShipKind::Elite;
    const int bounty = elite ? cfg::ORBIT_BOUNTY_ELITE : cfg::ORBIT_BOUNTY_PIRATE;
    ps.balance += bounty;
    Notify(ps, Fmt("%s abgeschossen! +%s Punkte", elite ? "Elite-Pirat" : "Pirat", Points(bounty).c_str()), Color{255, 210, 130, 255});
    if (ps.balance < cfg::GOAL_POINTS) return;
    if (!ps.orbitGoal) {
        ps.orbitGoal = true;
        Notify(ps, "Kartenwechsel frei: Mond und All im Reiter Karte (Handelsstation, Trägerschiff).", Color{150, 230, 255, 255});
    }
    if (isServer) GoalReached(ps);
    else NextRound(ps);
}

// Einzelspieler: Ziel erreicht auf der Weltraumkarte oder, bei freiem
// Kartenwechsel, auf dem Mond. Statt Siegesbildschirm eine neue Runde auf
// derselben Karte.
void World::NextRound(PlayerState& ps) {
    ++ps.goals;
    Notify(ps, "ZIEL ERREICHT! Neue Runde: Dein Kontostand beginnt wieder bei 0.", Color{110, 240, 170, 255});
    if (const Ship* sp = PlayerShip(ps)) SoundAt(Sfx::Victory, sp->pos, 1.0f, 0.4f, ps.id);
    ps.balance = 0;
}
