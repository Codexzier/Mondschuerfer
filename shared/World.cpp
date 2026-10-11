// Spiellogik: Bewegung, Scanner, Abbau, Kampf, Stationen.
//
// Ablauf eines Zeitschritts (Update):
//   Spieler (Eingaben, Scanner, Abbau) -> KI der anderen Schiffe -> abstürzende
//   Schiffe -> Schiffe auseinanderschieben -> Geschütztürme -> Geschosse ->
//   Partikel -> Wracks -> Aufräumen -> neue Lagerstätten/Gegner -> Meldungen.
//
// Wichtig für den Einzelspieler: Die Zufallszahlen (Rand/RandInt) werden in
// einer festen Reihenfolge gezogen. Gleicher Seed + gleiche Eingaben ergeben
// deshalb genau dasselbe Spiel. Beim Umbau für den Mehrspieler wurde darauf
// geachtet, diese Reihenfolge im Einzelspieler nicht zu verändern.
#include "World.hpp"
#include "Models.hpp"
#include <algorithm>
#include <cmath>

namespace {
int PirateTier(long long delivered) { return static_cast<int>(std::min<long long>(5, delivered / 18000)); }
} // namespace

// ===========================================================================
// Neues Spiel
// ===========================================================================
void World::NewGame(unsigned int newSeed, bool sim) {
    *this = World{};
    seed = newSeed;
    simulation = sim;
    rng.seed(seed);

    ResolveModels();
    BuildStations();
    GenerateTerrain();

    // Einzelspieler: genau ein Spieler mit ID 0, sein Schiff steht immer an ships[0]
    players.push_back(PlayerState{});
    PlayerState& ps = players[0];
    Ship p = MakeShip(ShipKind::Player, {0.0f, cfg::SHIP_ALTITUDE, 24.0f});
    p.yaw = PI; // Blick nach Süden, weg von der Station
    p.playerId = ps.id;
    ships.push_back(p);
    ps.shipId = p.id;
    ApplyUpgrades(ps, true);

    for (int i = 0; i < cfg::DEPOSIT_COUNT; ++i) SpawnDeposit(true);
    for (int i = 0; i < cfg::PIRATES_BASE; ++i) SpawnPirate();
    for (int i = 0; i < cfg::MINERS_COUNT; ++i) SpawnMiner(i == 0);

    lastStage = StationStage();
    if (!simulation) {
        Notify("Willkommen, Pilot! Finde Kristalle mit dem Scanner [E].", Color{255, 200, 120, 255});
        Notify("Ziel: 100.000 Ressourcenpunkte auf dem Konto.", Color{200, 220, 255, 255});
    }
}

void World::NewServerWorld(unsigned int newSeed, const GameSettings& s) {
    *this = World{};
    seed = newSeed;
    isServer = true;
    settings = s;
    Sanitize(settings);
    rng.seed(seed);

    ResolveModels();
    BuildStations();
    GenerateTerrain();   // nur für dieselbe Zufallsfolge wie beim Client; der Server zeichnet nichts

    for (int i = 0; i < DepositTarget(); ++i) SpawnDeposit(true);
    for (int i = 0; i < settings.enemyShips; ++i) SpawnPirate();
    for (int i = 0; i < cfg::MINERS_COUNT; ++i) SpawnMiner(i == 0);
    lastStage = StationStage();
}

void World::BuildMirror(unsigned int newSeed, const GameSettings& s) {
    *this = World{};
    seed = newSeed;
    isMirror = true;
    settings = s;
    Sanitize(settings);
    rng.seed(seed);

    // Gleiche Reihenfolge wie beim Server: dadurch entsteht dieselbe Mondoberfläche
    ResolveModels();
    BuildStations();
    GenerateTerrain();

    // Platzhalter für den eigenen Spieler und sein Schiff (Index 0). Der Client
    // überschreibt beides mit den Daten vom Server. Zuschauer haben kein Schiff:
    // das Platzhalterschiff bleibt dann unsichtbar.
    players.push_back(PlayerState{});
    Ship dummy;
    dummy.id = -1;
    dummy.model = mdl.player;
    dummy.hidden = true;
    dummy.dead = true;
    ships.push_back(dummy);
}

Ship World::MakeShip(ShipKind kind, Vector3 pos) {
    Ship s;
    s.id = nextId++;
    s.kind = kind;
    s.pos = pos;
    s.pos.y = cfg::SHIP_ALTITUDE;
    s.yaw = Rand(0, 2 * PI);
    const int tier = PirateTier(totalDelivered);
    switch (kind) {
        case ShipKind::Player:
            s.faction = Faction::Player;
            s.model = mdl.player;
            s.turnRate = 2.9f;
            break;
        case ShipKind::Pirate:
            s.faction = Faction::Pirate;
            s.model = Rand(0, 1) < 0.5f ? mdl.pirateA : mdl.pirateB;
            s.maxHp = s.hp = 40.0f + tier * 12.0f;
            s.maxSpeed = 14.5f + tier * 0.8f;
            s.turnRate = 2.3f;
            s.damage = cfg::PIRATE_DAMAGE;
            s.fireInterval = 0.95f - tier * 0.05f;
            s.cargo = RandInt(15, 40) * 10 + tier * 80;
            s.cargoMax = s.cargo;
            break;
        case ShipKind::Elite:
            s.faction = Faction::Pirate;
            s.model = mdl.elite;
            s.maxHp = s.hp = 100.0f + tier * 18.0f;
            s.maxSpeed = 18.5f + tier * 0.6f;
            s.turnRate = 2.6f;
            s.damage = cfg::ELITE_DAMAGE;
            s.fireInterval = 0.6f;
            s.cargo = RandInt(50, 95) * 10 + tier * 120;
            s.cargoMax = s.cargo;
            break;
        case ShipKind::Miner: {
            s.faction = Faction::Neutral;
            int v = RandInt(0, 2);
            s.model = v == 0 ? mdl.miner : (v == 1 ? mdl.cargoA : mdl.cargoB);
            s.maxHp = s.hp = 70.0f;
            s.maxSpeed = 11.5f;
            s.turnRate = 2.0f;
            s.damage = 10.0f;
            s.fireInterval = 1.0f;
            s.cargoMax = RandInt(40, 90) * 10;
            s.ai = AiState::GoMine;
            break;
        }
    }
    return s;
}

void World::SpawnPirate() {
    Vector3 p = RandomFreePos(cfg::STATION_SAFE_RADIUS + 25.0f, ships.empty() ? 0.0f : 70.0f);
    float eliteChance = std::clamp(static_cast<float>(totalDelivered) / 70000.0f, 0.0f, 0.45f);
    ShipKind k = Rand(0, 1) < eliteChance ? ShipKind::Elite : ShipKind::Pirate;
    Ship s = MakeShip(k, p);
    s.ai = AiState::Wander;
    s.waypoint = s.pos;
    ships.push_back(s);
}

void World::SpawnMiner(bool atStation) {
    Vector3 p{};
    if (atStation) {
        // Bei mehreren Handelsstationen eine zufällige wählen (bei einer: ohne Zufallszahl,
        // damit der Einzelspieler dieselbe Zufallsfolge behält)
        int trades = CountStations(StationType::Trade);
        int pick = trades > 1 ? RandInt(0, trades - 1) : 0;
        int idx = 0;
        for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
            if (stations[i].type != StationType::Trade) continue;
            if (pick-- == 0) { idx = i; break; }
        }
        const Vector3 sp = stations[idx].pos;
        p = Vector3{sp.x + Rand(-12, 12), 0, sp.z + 16.0f};
    } else {
        p = RandomFreePos(30.0f, 30.0f);
    }
    Ship s = MakeShip(ShipKind::Miner, p);
    ships.push_back(s);
}

int World::DepositTarget() const {
    if (!isServer) return cfg::DEPOSIT_COUNT;
    // Mehr Spieler brauchen mehr Lagerstätten
    int extra = std::max(0, static_cast<int>(players.size()) - 1) * cfg::MP_DEPOSITS_PER_PLAYER;
    return std::min(cfg::MP_DEPOSITS_MAX, cfg::DEPOSIT_COUNT + extra);
}

// ===========================================================================
// Spieler (Mehrspieler-Server)
// ===========================================================================
PlayerState* World::FindPlayer(int playerId) {
    for (auto& ps : players) if (ps.id == playerId) return &ps;
    return nullptr;
}

const PlayerState* World::FindPlayer(int playerId) const {
    for (const auto& ps : players) if (ps.id == playerId) return &ps;
    return nullptr;
}

Ship* World::PlayerShip(const PlayerState& ps) { return ps.shipId >= 0 ? FindShip(ps.shipId) : nullptr; }
const Ship* World::PlayerShip(const PlayerState& ps) const { return ps.shipId >= 0 ? FindShip(ps.shipId) : nullptr; }

PlayerState* World::OwnerOfShip(int shipId) {
    Ship* s = FindShip(shipId);
    if (!s || s->playerId < 0) return nullptr;
    return FindPlayer(s->playerId);
}

PlayerState& World::AddPlayer(int playerId, const std::string& name) {
    PlayerState ps;
    ps.id = playerId;
    ps.name = name;
    players.push_back(ps);
    PlayerState& added = players.back();
    SpawnPlayerShip(added);
    return added;
}

void World::RemovePlayer(int playerId) {
    PlayerState* ps = FindPlayer(playerId);
    if (!ps) return;
    const int shipId = ps->shipId;
    // Schiff sofort entfernen; Geschosse und KI-Ziele verweisen nur über die ID darauf
    ships.erase(std::remove_if(ships.begin(), ships.end(), [&](const Ship& s) { return s.id == shipId && shipId >= 0; }),
                ships.end());
    for (auto& d : deposits) {
        auto& v = d.revealedFor;
        v.erase(std::remove(v.begin(), v.end(), playerId), v.end());
    }
    players.erase(std::remove_if(players.begin(), players.end(), [&](const PlayerState& p) { return p.id == playerId; }),
                  players.end());
}

void World::SpawnPlayerShip(PlayerState& ps) {
    // An einer zufälligen Handelsstation, mit Blick von der Station weg
    std::vector<int> trades;
    for (int i = 0; i < static_cast<int>(stations.size()); ++i)
        if (stations[i].type == StationType::Trade) trades.push_back(i);
    const Vector3 c = trades.empty() ? Vector3{0, 0, 0} : stations[trades[RandInt(0, static_cast<int>(trades.size()) - 1)]].pos;
    const float a = Rand(0, 2 * PI);
    Vector3 pos{c.x + cosf(a) * cfg::MP_SPAWN_DISTANCE, cfg::SHIP_ALTITUDE, c.z + sinf(a) * cfg::MP_SPAWN_DISTANCE};
    Ship s = MakeShip(ShipKind::Player, pos);
    s.yaw = YawTowards(c, pos);
    s.playerId = ps.id;
    ships.push_back(s);
    ps.shipId = s.id;
    ps.respawnT = 0.0f;
    ApplyUpgrades(ps, true);
}

void World::ResetProgress(PlayerState& ps) {
    ps.balance = 0;
    for (int& u : ps.upgrades) u = 0;
    ps.scanEnergy = cfg::SCANNER_MAX_ENERGY;
    ps.scanOn = false;
    ps.scanPulse = 0.0f;
    ps.mining = false;
    ps.mineT = 0.0f;
    ps.dockedAt = -1;
    ps.lastSale = 0;
    ps.playTime = 0.0f;
    ps.roundTime = 0.0f;
    ps.round = 1;
}

bool World::PlayerAlive(const PlayerState& ps) const {
    const Ship* s = PlayerShip(ps);
    return s && !s->crashing && !s->dead;
}

bool World::RevealedFor(const Deposit& d, const PlayerState& ps) const {
    if (!isServer) return d.revealed;
    return std::find(d.revealedFor.begin(), d.revealedFor.end(), ps.id) != d.revealedFor.end();
}

bool World::Targetable(const Ship& s) const {
    return !s.dead && !s.crashing && !s.hidden && !s.docked;
}

// ===========================================================================
// Hauptupdate
// ===========================================================================
void World::Update(float dt) {
    time += dt;
    for (auto& ps : players) {
        if (!isServer) {
            if (outcome == Outcome::None) ps.playTime += dt;
        } else if (PlayerAlive(ps)) {
            ps.playTime += dt;
            ps.roundTime += dt;
        }
    }
    if (outcome != Outcome::None) outcomeT += dt;

    for (auto& ps : players) UpdatePlayer(ps, dt);

    for (size_t i = 0; i < ships.size(); ++i) {
        Ship& s = ships[i];
        if (s.kind == ShipKind::Player) continue;  // Spielerschiffe werden über ihre Eingaben gesteuert
        if (s.dead || s.crashing || (s.hidden && s.ai != AiState::Docked)) continue;
        UpdateAi(s, dt);
    }

    // Abstürzende Schiffe
    for (size_t i = 0; i < ships.size(); ++i) {
        Ship& s = ships[i];
        if (!s.crashing || s.dead) continue;
        s.crashT += dt;
        s.vel.y -= 11.0f * dt;
        s.vel.x *= (1.0f - 0.6f * dt);
        s.vel.z *= (1.0f - 0.6f * dt);
        s.pos = Vector3Add(s.pos, Vector3Scale(s.vel, dt));
        s.yaw += s.crashSpin * dt;
        s.bank += s.crashSpin * 0.6f * dt;
        if (!isServer && RandInt(0, 2) == 0) {
            Particle sm;
            sm.pos = s.pos;
            sm.vel = {Rand(-1, 1), Rand(1.0f, 2.5f), Rand(-1, 1)};
            sm.life = sm.maxLife = Rand(0.8f, 1.5f);
            sm.size = Rand(0.35f, 0.7f);
            sm.color = RandInt(0, 3) == 0 ? Color{255, 150, 40, 255} : Color{70, 72, 80, 255};
            sm.glow = sm.color.r > 200;
            particles.push_back(sm);
        }
        if (s.pos.y <= 0.45f) {
            Explosion({s.pos.x, 0.8f, s.pos.z}, s.kind == ShipKind::Player ? 2.0f : 1.4f);
            SoundAt(Sfx::CrashBoom, s.pos, s.kind == ShipKind::Player ? 1.0f : 0.8f);
            s.dead = true;
            if (s.kind == ShipKind::Player && !isServer) {
                s.hidden = true;
                outcome = Outcome::Dead;
                outcomeT = 0.0f;
            } else {
                if (s.kind == ShipKind::Player) PlayerShipLost(s);
                Wreck w;
                w.id = nextId++;
                w.pos = {s.pos.x, 0.15f, s.pos.z};
                w.yaw = s.yaw;
                w.tilt = Rand(-0.35f, 0.35f);
                w.model = s.model;
                w.amount = std::max(120, (s.cargo / 10) * 10);
                w.initial = w.amount;
                wrecks.push_back(w);
            }
        }
    }

    // Schiffe voneinander trennen
    for (size_t i = 0; i < ships.size(); ++i) {
        for (size_t j = i + 1; j < ships.size(); ++j) {
            Ship& a = ships[i];
            Ship& b = ships[j];
            if (a.dead || b.dead || a.hidden || b.hidden || a.crashing || b.crashing) continue;
            if (isServer && (a.docked || b.docked)) continue;
            Vector2 d{b.pos.x - a.pos.x, b.pos.z - a.pos.z};
            float len = Vector2Length(d);
            const float minD = 2.6f;
            if (len < minD && len > 0.001f) {
                Vector2 n = Vector2Scale(d, 1.0f / len);
                float push = (minD - len) * 0.5f;
                a.pos.x -= n.x * push; a.pos.z -= n.y * push;
                b.pos.x += n.x * push; b.pos.z += n.y * push;
            }
        }
    }

    UpdateTurrets(dt);
    UpdateBullets(dt);
    UpdateParticles(dt);

    // Wracks altern
    for (auto& w : wrecks) {
        w.age += dt;
        if (!isServer && RandInt(0, 30) == 0 && w.age < 60.0f) {
            Particle sm;
            sm.pos = Vector3Add(w.pos, {Rand(-0.6f, 0.6f), 0.6f, Rand(-0.6f, 0.6f)});
            sm.vel = {Rand(-0.3f, 0.3f), Rand(0.8f, 1.6f), Rand(-0.3f, 0.3f)};
            sm.life = sm.maxLife = Rand(1.5f, 2.5f);
            sm.size = Rand(0.3f, 0.6f);
            sm.color = {60, 62, 70, 255};
            particles.push_back(sm);
        }
    }

    // Aufräumen: erschöpfte Lagerstätten und Wracks
    auto depEnd = std::remove_if(deposits.begin(), deposits.end(), [](const Deposit& d) { return d.amount <= 0; });
    int removedDeposits = static_cast<int>(deposits.end() - depEnd);
    deposits.erase(depEnd, deposits.end());
    if (removedDeposits > 0) ++depositRevision;
    wrecks.erase(std::remove_if(wrecks.begin(), wrecks.end(),
                                [](const Wreck& w) { return w.amount <= 0 || w.age > 300.0f; }),
                 wrecks.end());

    // Neue Lagerstätten (im Mehrspieler schneller, je mehr Spieler da sind)
    if (static_cast<int>(deposits.size()) < DepositTarget()) {
        depositSpawnT -= dt;
        if (depositSpawnT <= 0.0f) {
            SpawnDeposit(false);
            float interval = cfg::DEPOSIT_RESPAWN;
            if (isServer) interval /= std::max(1.0f, static_cast<float>(players.size()) * 0.5f);
            depositSpawnT = interval * Rand(0.6f, 1.2f);
        }
    }

    // Piraten nachschieben. Einzelspieler: mehr, je weiter das Spiel fortgeschritten ist.
    // Mehrspieler: feste Anzahl aus den Server-Einstellungen (Schwierigkeitsgrad).
    int pirates = 0, miners = 0;
    for (const auto& s : ships) {
        if (s.dead) continue;
        if (s.faction == Faction::Pirate) ++pirates;
        if (s.kind == ShipKind::Miner) ++miners;
    }
    int wantPirates = isServer ? settings.enemyShips
                               : std::min<int>(cfg::PIRATES_MAX, cfg::PIRATES_BASE + static_cast<int>(totalDelivered / 12000));
    pirateSpawnT -= dt;
    if (pirates < wantPirates && pirateSpawnT <= 0.0f) {
        SpawnPirate();
        pirateSpawnT = Rand(9.0f, 18.0f);
        // Viele Piraten (Schwer): schneller auffüllen
        if (isServer && wantPirates > 8) pirateSpawnT *= 0.3f;
    }
    minerSpawnT -= dt;
    if (miners < cfg::MINERS_COUNT && minerSpawnT <= 0.0f) {
        SpawnMiner(true);
        minerSpawnT = Rand(25.0f, 40.0f);
    }

    // Tote Schiffe entfernen. Einzelspieler: das Spielerschiff bleibt immer an Index 0.
    const size_t keep = isServer ? 0 : 1;
    if (ships.size() > keep) {
        ships.erase(std::remove_if(ships.begin() + static_cast<long>(keep), ships.end(),
                                   [](const Ship& s) { return s.dead; }),
                    ships.end());
    }

    // Mehrspieler: neues Schiff nach einem Absturz
    if (isServer) {
        for (auto& ps : players) {
            if (ps.shipId >= 0 && FindShip(ps.shipId)) continue;
            ps.shipId = -1;
            ps.respawnT -= dt;
            if (ps.respawnT <= 0.0f) {
                SpawnPlayerShip(ps);
                Notify(ps, "Neues Raumschiff! Dein Fortschritt beginnt von vorn.", Color{200, 220, 255, 255});
            }
        }
    }

    for (auto& ps : players) {
        for (auto& n : ps.notices) n.time -= dt;
        ps.notices.erase(std::remove_if(ps.notices.begin(), ps.notices.end(), [](const Notice& n) { return n.time <= 0; }),
                         ps.notices.end());
    }
    if (stageAnim > 0.0f) stageAnim = std::max(0.0f, stageAnim - dt * 0.6f);
    for (auto& d : deposits) if (d.revealed && d.revealT < 1.0f) d.revealT = std::min(1.0f, d.revealT + dt * 2.0f);
}

void World::UpdateAmbient(float dt) {
    if (dt <= 0.0f) return;
    time += dt;
    UpdateParticles(dt);
    for (auto& ps : players) {
        for (auto& n : ps.notices) n.time -= dt;
        ps.notices.erase(std::remove_if(ps.notices.begin(), ps.notices.end(), [](const Notice& n) { return n.time <= 0; }),
                         ps.notices.end());
    }
    if (stageAnim > 0.0f) stageAnim = std::max(0.0f, stageAnim - dt * 0.6f);
}

void World::UpdateCosmetics(float dt) {
    // Der Mehrspieler-Client simuliert die Welt nicht selbst. Rauch und Funken,
    // die im Einzelspieler nebenbei in Update() entstehen, erzeugt er hier aus
    // dem Zustand, den der Server geschickt hat.
    for (const auto& s : ships) {
        if (!s.crashing || s.dead || s.hidden) continue;
        if (RandInt(0, 2) != 0) continue;
        Particle sm;
        sm.pos = s.pos;
        sm.vel = {Rand(-1, 1), Rand(1.0f, 2.5f), Rand(-1, 1)};
        sm.life = sm.maxLife = Rand(0.8f, 1.5f);
        sm.size = Rand(0.35f, 0.7f);
        sm.color = RandInt(0, 3) == 0 ? Color{255, 150, 40, 255} : Color{70, 72, 80, 255};
        sm.glow = sm.color.r > 200;
        particles.push_back(sm);
    }
    for (const auto& w : wrecks) {
        if (RandInt(0, 30) != 0 || w.age > 60.0f) continue;
        Particle sm;
        sm.pos = Vector3Add(w.pos, {Rand(-0.6f, 0.6f), 0.6f, Rand(-0.6f, 0.6f)});
        sm.vel = {Rand(-0.3f, 0.3f), Rand(0.8f, 1.6f), Rand(-0.3f, 0.3f)};
        sm.life = sm.maxLife = Rand(1.5f, 2.5f);
        sm.size = Rand(0.3f, 0.6f);
        sm.color = {60, 62, 70, 255};
        particles.push_back(sm);
    }
    // Eigener Abbaustrahl: grüne Teilchen fliegen von der Lagerstätte zum Schiff
    const PlayerState& me = Me();
    const Ship& p = Player();
    if (me.mining && !p.dead && !p.hidden && RandInt(0, 2) == 0) {
        Vector3 target{};
        bool ok = false;
        for (const auto& d : deposits) if (me.mineKind == 1 && d.id == me.mineId) { target = d.pos; ok = true; }
        for (const auto& w : wrecks) if (me.mineKind == 2 && w.id == me.mineId) { target = w.pos; ok = true; }
        if (ok) {
            Particle pt;
            Vector3 off{Rand(-0.8f, 0.8f), 0.5f, Rand(-0.8f, 0.8f)};
            pt.pos = Vector3Add(target, off);
            pt.vel = Vector3Scale(Vector3Subtract(p.pos, pt.pos), 1.6f);
            pt.life = pt.maxLife = 0.6f;
            pt.size = Rand(0.15f, 0.3f);
            pt.color = {90, 255, 170, 255};
            pt.glow = true;
            particles.push_back(pt);
        }
    }
    for (auto& w : wrecks) w.age += dt;
    UpdateParticles(dt);
    for (auto& ps : players) {
        for (auto& n : ps.notices) n.time -= dt;
        ps.notices.erase(std::remove_if(ps.notices.begin(), ps.notices.end(), [](const Notice& n) { return n.time <= 0; }),
                         ps.notices.end());
    }
    if (stageAnim > 0.0f) stageAnim = std::max(0.0f, stageAnim - dt * 0.6f);
    for (auto& d : deposits) if (d.revealed && d.revealT < 1.0f) d.revealT = std::min(1.0f, d.revealT + dt * 2.0f);
}

// ===========================================================================
// Spieler
// ===========================================================================
void World::UpdatePlayer(PlayerState& ps, float dt) {
    if (ps.dockedAt >= 0) return;   // angedockt: Schiff steht still (Einzelspieler ruft Update dann gar nicht auf)
    Ship* sp = PlayerShip(ps);
    if (!sp) return;
    Ship& p = *sp;
    if (p.dead || p.crashing || p.hidden) return;
    const ShipInput& in = ps.input;

    UpdateShipPhysics(p, dt, in);
    p.fireCd -= dt;
    p.hitFlash = std::max(0.0f, p.hitFlash - dt * 3.0f);

    // --- Scanner ---------------------------------------------------------
    if (in.scanToggle) {
        if (ps.scanOn) {
            ps.scanOn = false;
            SoundAt(Sfx::ScanOff, p.pos, 1.0f, 0.0f, ps.id);
        } else if (ps.scanEnergy >= 1.0f) {
            ps.scanOn = true;
            ps.scanPulse = 0.0f;
            SoundAt(Sfx::ScanOn, p.pos, 1.0f, 0.0f, ps.id);
            SoundAt(Sfx::ScanPing, p.pos, 1.0f, 0.15f, ps.id);
        } else if (!simulation) {
            Notify(ps, "Scanner-Energie zu niedrig", Color{255, 170, 120, 255});
            SoundAt(Sfx::Denied, p.pos, 1.0f, 0.0f, ps.id);
        }
    }
    if (ps.scanOn) {
        ps.scanEnergy -= cfg::SCANNER_DRAIN * dt;
        const float lastPulse = ps.scanPulse;
        ps.scanPulse += dt;
        // Jeder neue Scannerring gibt einen Ping
        if (floorf(ps.scanPulse / cfg::SCANNER_PULSE_TIME) > floorf(lastPulse / cfg::SCANNER_PULSE_TIME))
            SoundAt(Sfx::ScanPing, p.pos, 1.0f, 0.0f, ps.id);
        float ring = cfg::SCANNER_RADIUS * fmodf(ps.scanPulse, cfg::SCANNER_PULSE_TIME) / cfg::SCANNER_PULSE_TIME;
        int found = 0;
        for (auto& d : deposits) {
            if (!RevealedFor(d, ps) && Dist2D(d.pos, p.pos) <= ring) {
                if (isServer) {
                    d.revealedFor.push_back(ps.id);
                    ++ps.revealRevision;
                } else {
                    d.revealed = true;
                    d.revealT = 0.0f;
                }
                ++found;
            }
        }
        if (found > 0) SoundAt(Sfx::Discover, p.pos, 1.0f, 0.0f, ps.id);
        if (found > 0 && !simulation)
            Notify(ps, found == 1 ? std::string("Lagerstätte entdeckt!") : Fmt("%d Lagerstätten entdeckt!", found),
                   Color{120, 255, 190, 255});
        if (ps.scanEnergy <= 0.0f) {
            ps.scanEnergy = 0.0f;
            ps.scanOn = false;
            SoundAt(Sfx::ScanEmpty, p.pos, 1.0f, 0.0f, ps.id);
            if (!simulation) Notify(ps, "Scanner abgeschaltet: Energie leer", Color{255, 170, 120, 255});
        }
    } else {
        ps.scanEnergy = std::min(cfg::SCANNER_MAX_ENERGY, ps.scanEnergy + cfg::SCANNER_REGEN * dt);
    }

    // --- Waffe -------------------------------------------------------------
    if (in.fire && p.fireCd <= 0.0f) {
        FireBullet(p);
        p.fireCd = p.fireInterval;
    }

    // --- Interaktion (F) ----------------------------------------------------
    if (in.interact) {
        if (ps.mining) {
            ps.mining = false;
            SoundAt(Sfx::MineAbort, p.pos, 1.0f, 0.0f, ps.id);
            if (!simulation) Notify(ps, "Abbau abgebrochen", Color{255, 200, 150, 255});
        } else {
            int kind = 0, id = -1;
            InteractTarget(ps, &kind, &id);
            if (kind == 3 || kind == 4) {
                Dock(ps, id);
                return;   // angedockt: Rest dieses Schritts entfällt
            } else if (kind == 1 || kind == 2) {
                if (p.cargo >= p.cargoMax) {
                    SoundAt(Sfx::CargoFull, p.pos, 1.0f, 0.0f, ps.id);
                    if (!simulation) Notify(ps, "Lager voll! Ab zur Handelsstation.", Color{255, 200, 120, 255});
                } else {
                    StartMining(ps, kind, id);
                }
            }
        }
    }

    // --- Abbau-Fortschritt ----------------------------------------------------
    p.mining = false;
    if (ps.mining) {
        Vector3 target{};
        bool valid = false;
        if (ps.mineKind == 1) {
            if (Deposit* d = FindDeposit(ps.mineId)) { target = d->pos; valid = d->amount > 0; }
        } else if (Wreck* w = FindWreck(ps.mineId)) {
            target = w->pos;
            valid = w->amount > 0;
        }
        if (!valid) {
            ps.mining = false;
            SoundAt(Sfx::MineAbort, p.pos, 1.0f, 0.0f, ps.id);
            if (!simulation) Notify(ps, "Lagerstätte erschöpft", Color{255, 200, 150, 255});
        } else if (Dist2D(target, p.pos) > cfg::MINING_RANGE + 1.5f) {
            ps.mining = false;
            SoundAt(Sfx::MineAbort, p.pos, 1.0f, 0.0f, ps.id);
            if (!simulation) Notify(ps, "Abbau abgebrochen: zu weit entfernt", Color{255, 200, 150, 255});
        } else {
            ps.mineT += dt;
            if (isServer) {
                // Damit andere Spieler den Abbaustrahl sehen
                p.mining = true;
                p.mineTarget = target;
            } else if (RandInt(0, 2) == 0) {
                Particle pt;
                Vector3 off{Rand(-0.8f, 0.8f), 0.5f, Rand(-0.8f, 0.8f)};
                pt.pos = Vector3Add(target, off);
                Vector3 to = Vector3Subtract(p.pos, pt.pos);
                pt.vel = Vector3Scale(to, 1.6f);
                pt.life = pt.maxLife = 0.6f;
                pt.size = Rand(0.15f, 0.3f);
                pt.color = {90, 255, 170, 255};
                pt.glow = true;
                particles.push_back(pt);
            }
            if (ps.mineT >= cfg::MINING_TIME) FinishMining(ps);
        }
    }
}

void World::UpdateShipPhysics(Ship& s, float dt, const ShipInput& in) {
    s.yaw += in.turn * s.turnRate * dt;
    if (s.yaw > PI) s.yaw -= 2 * PI;
    if (s.yaw < -PI) s.yaw += 2 * PI;
    s.bank = Lerp(s.bank, -in.turn * 0.5f, std::min(1.0f, 6.0f * dt));

    Vector3 fwd = ForwardFromYaw(s.yaw);
    const float accel = s.maxSpeed * 1.9f;
    if (in.thrust > 0.0f) s.vel = Vector3Add(s.vel, Vector3Scale(fwd, accel * in.thrust * dt));
    else if (in.thrust < 0.0f) s.vel = Vector3Add(s.vel, Vector3Scale(fwd, accel * 0.7f * in.thrust * dt));

    float drag = (in.thrust == 0.0f) ? 1.4f : 0.35f;
    s.vel = Vector3Scale(s.vel, 1.0f - std::min(1.0f, drag * dt));
    // Seitliches Driften dämpfen
    Vector3 side{fwd.z, 0.0f, -fwd.x};
    float lat = Vector3DotProduct(s.vel, side);
    s.vel = Vector3Subtract(s.vel, Vector3Scale(side, lat * std::min(1.0f, 2.6f * dt)));
    s.vel.y = 0.0f;

    float fwdSpeed = Vector3DotProduct(s.vel, fwd);
    float speed = Vector3Length(s.vel);
    float limit = fwdSpeed >= 0 ? s.maxSpeed : s.maxSpeed * 0.5f;
    if (speed > limit) s.vel = Vector3Scale(s.vel, limit / speed);

    s.pos = Vector3Add(s.pos, Vector3Scale(s.vel, dt));
    s.pos.y = cfg::SHIP_ALTITUDE + sinf(time * 2.0f + static_cast<float>(s.id)) * 0.12f;
    s.thrustVisual = Lerp(s.thrustVisual, std::max(0.0f, in.thrust), std::min(1.0f, 8.0f * dt));

    // Hohe Stationsteile umfliegen
    for (const auto& st : stations) {
        for (const auto& ob : st.obstacles) {
            Vector2 c{st.pos.x + ob.x, st.pos.z + ob.z};
            Vector2 d{s.pos.x - c.x, s.pos.z - c.y};
            float len = Vector2Length(d);
            if (len < ob.y && len > 0.001f) {
                Vector2 n = Vector2Scale(d, 1.0f / len);
                s.pos.x = c.x + n.x * ob.y;
                s.pos.z = c.y + n.y * ob.y;
                float vn = s.vel.x * n.x + s.vel.z * n.y;
                if (vn < 0) { s.vel.x -= n.x * vn; s.vel.z -= n.y * vn; }
            }
        }
    }
    // Grenze des Einsatzgebiets
    const float lim = cfg::WORLD_HALF;
    if (s.pos.x < -lim) { s.pos.x = -lim; s.vel.x = std::max(0.0f, s.vel.x); }
    if (s.pos.x > lim) { s.pos.x = lim; s.vel.x = std::min(0.0f, s.vel.x); }
    if (s.pos.z < -lim) { s.pos.z = -lim; s.vel.z = std::max(0.0f, s.vel.z); }
    if (s.pos.z > lim) { s.pos.z = lim; s.vel.z = std::min(0.0f, s.vel.z); }
}

// ===========================================================================
// Interaktion, Abbau, Stationen
// ===========================================================================
bool World::NearStation(const Ship& s, int station, float range) const {
    if (station < 0 || station >= static_cast<int>(stations.size())) return false;
    return Dist2D(s.pos, stations[station].pos) <= range;
}

int World::NearestStation(Vector3 p, StationType type) const {
    int best = -1;
    float bestD = 1e9f;
    for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
        if (stations[i].type != type) continue;
        float d = Dist2D(p, stations[i].pos);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

int World::CountStations(StationType type) const {
    int n = 0;
    for (const auto& st : stations) if (st.type == type) ++n;
    return n;
}

int World::InteractTarget(const PlayerState& ps, int* kind, int* id) const {
    *kind = 0;
    *id = -1;
    const Ship* sp = PlayerShip(ps);
    if (!sp) return 0;
    const Ship& p = *sp;
    for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
        if (NearStation(p, i, cfg::DOCK_RANGE)) {
            *kind = stations[i].type == StationType::Trade ? 3 : 4;
            *id = i;
            return *kind;
        }
    }
    float best = cfg::MINING_RANGE;
    for (const auto& w : wrecks) {
        float d = Dist2D(w.pos, p.pos);
        if (d <= best && w.amount > 0) { best = d; *kind = 2; *id = w.id; }
    }
    if (*kind == 2) return 2;
    best = cfg::MINING_RANGE;
    for (const auto& d : deposits) {
        float dist = Dist2D(d.pos, p.pos);
        if (RevealedFor(d, ps) && dist <= best && d.amount > 0) { best = dist; *kind = 1; *id = d.id; }
    }
    return *kind;
}

void World::StartMining(PlayerState& ps, int kind, int id) {
    ps.mining = true;
    ps.mineKind = kind;
    ps.mineId = id;
    ps.mineT = 0.0f;
    if (const Ship* p = PlayerShip(ps)) SoundAt(Sfx::MineStart, p->pos, 1.0f, 0.0f, ps.id);
    if (!simulation) Notify(ps, kind == 2 ? "Wrack wird ausgeschlachtet ..." : "Abbau gestartet ...",
                            Color{150, 255, 200, 255});
}

void World::FinishMining(PlayerState& ps) {
    Ship* sp = PlayerShip(ps);
    ps.mining = false;
    if (!sp) return;
    Ship& p = *sp;
    int free = p.cargoMax - p.cargo;
    int* amount = nullptr;
    if (ps.mineKind == 1) { if (Deposit* d = FindDeposit(ps.mineId)) amount = &d->amount; }
    else if (Wreck* w = FindWreck(ps.mineId)) amount = &w->amount;
    if (!amount) return;
    int take = std::min(free, *amount);
    *amount -= take;
    p.cargo += take;
    if (ps.mineKind == 1) ++depositRevision;
    SoundAt(Sfx::MineDone, p.pos, 1.0f, 0.0f, ps.id);
    if (p.cargo >= p.cargoMax) SoundAt(Sfx::CargoFull, p.pos, 1.0f, 0.45f, ps.id);
    if (!simulation) {
        Notify(ps, Fmt("+%d Ressourcen geladen", take), Color{120, 255, 190, 255});
        if (p.cargo >= p.cargoMax) Notify(ps, "Lager voll! Ab zur Handelsstation.", Color{255, 200, 120, 255});
    }
    SpawnSparks(p.pos, Color{120, 255, 190, 255}, 14);
}

int World::StationStage() const {
    int stage = 0;
    for (int i = 0; i < cfg::STATION_STAGES; ++i)
        if (totalDelivered >= cfg::STATION_THRESHOLDS[i]) stage = i;
    return stage;
}

int World::NextStageThreshold() const {
    int st = StationStage();
    if (st + 1 >= cfg::STATION_STAGES) return -1;
    return cfg::STATION_THRESHOLDS[st + 1];
}

void World::Dock(PlayerState& ps, int station) {
    Ship* sp = PlayerShip(ps);
    if (!sp || station < 0 || station >= static_cast<int>(stations.size())) return;
    Ship& p = *sp;
    ps.dockedAt = station;
    ps.mining = false;
    ps.scanOn = false;
    p.vel = {0, 0, 0};
    p.docked = true;
    p.mining = false;
    ps.lastSale = 0;
    SoundAt(Sfx::Dock, p.pos, 1.0f, 0.0f, ps.id);
    if (stations[station].type == StationType::Trade) {
        int sale = p.cargo;
        p.cargo = 0;
        ps.balance += sale;
        ps.delivered += sale;
        totalDelivered += sale;
        ps.lastSale = sale;
        if (sale > 0) SoundAt(Sfx::Sale, p.pos, 1.0f, 0.35f, ps.id);
        int stage = StationStage();
        if (stage > lastStage) {
            lastStage = stage;
            stageAnim = 1.0f;
            SoundAt(Sfx::StationGrow, p.pos, 1.0f, 0.8f, ps.id);
            if (!simulation) Notify(ps, Fmt("Die Station wächst! Ausbaustufe %d", stage), Color{255, 210, 120, 255});
            for (const auto& st : stations) {
                if (st.type != StationType::Trade) continue;
                for (const auto& part : st.parts)
                    if (part.stage == stage) SpawnSparks(Vector3Add(st.pos, Vector3Add(part.offset, {0, 1.5f, 0})), Color{255, 220, 120, 255}, 12);
            }
        }
        if (isServer) {
            if (ps.balance >= cfg::GOAL_POINTS) GoalReached(ps);
        } else if (ps.balance >= cfg::GOAL_POINTS && outcome == Outcome::None) {
            outcome = Outcome::Won;
            outcomeT = 0.0f;
            SoundAt(Sfx::Victory, p.pos, 1.0f, 1.3f, ps.id);
        }
    }
    wantAutosave = !simulation;
}

void World::Undock(PlayerState& ps) {
    if (ps.dockedAt < 0) return;
    Ship* sp = PlayerShip(ps);
    const int station = ps.dockedAt;
    ps.dockedAt = -1;
    if (!sp) return;
    Ship& p = *sp;
    // Schiff zeigt beim Abdocken von der Station weg
    p.yaw = YawTowards(stations[station].pos, p.pos);
    p.vel = Vector3Scale(ForwardFromYaw(p.yaw), 6.0f);
    p.docked = false;
    SoundAt(Sfx::Undock, p.pos, 1.0f, 0.0f, ps.id);
}

int World::RepairCost(const PlayerState& ps) const {
    const Ship* p = PlayerShip(ps);
    if (!p) return 0;
    int missing = static_cast<int>(ceilf(p->maxHp - p->hp));
    return std::max(0, missing) * cfg::REPAIR_COST_PER_POINT;
}

bool World::Repair(PlayerState& ps) {
    Ship* sp = PlayerShip(ps);
    if (!sp) return false;
    Ship& p = *sp;
    int cost = RepairCost(ps);
    if (cost <= 0) return false;
    if (ps.balance >= cost) {
        ps.balance -= cost;
        p.hp = p.maxHp;
        SoundAt(Sfx::Repair, p.pos, 1.0f, 0.0f, ps.id);
        return true;
    }
    // Teilreparatur mit dem vorhandenen Geld
    int points = static_cast<int>(ps.balance / cfg::REPAIR_COST_PER_POINT);
    if (points <= 0) return false;
    ps.balance -= points * cfg::REPAIR_COST_PER_POINT;
    p.hp = std::min(p.maxHp, p.hp + points);
    SoundAt(Sfx::Repair, p.pos, 1.0f, 0.0f, ps.id);
    return true;
}

int World::UpgradeCost(const PlayerState& ps, int type) const {
    if (type < 0 || type >= cfg::UP_COUNT) return -1;
    if (ps.upgrades[type] >= cfg::UPGRADE_LEVELS) return -1;
    return cfg::UPGRADE_COST[type][ps.upgrades[type]];
}

bool World::BuyUpgrade(PlayerState& ps, int type) {
    int cost = UpgradeCost(ps, type);
    if (cost < 0 || ps.balance < cost) return false;
    ps.balance -= cost;
    ps.upgrades[type]++;
    ApplyUpgrades(ps, false);
    if (const Ship* p = PlayerShip(ps)) SoundAt(Sfx::Upgrade, p->pos, 1.0f, 0.0f, ps.id);
    if (!simulation) {
        static const char* names[cfg::UP_COUNT] = {"Antrieb", "Panzerung", "Bewaffnung", "Lager"};
        Notify(ps, Fmt("%s auf Stufe %d verbessert!", names[type], ps.upgrades[type]), Color{110, 240, 170, 255});
    }
    return true;
}

void World::ApplyUpgrades(PlayerState& ps, bool refill) {
    Ship* sp = PlayerShip(ps);
    if (!sp) return;
    Ship& p = *sp;
    float oldMax = p.maxHp;
    p.maxSpeed = cfg::SPEED_VALUES[ps.upgrades[cfg::UP_SPEED]];
    p.maxHp = cfg::ARMOR_VALUES[ps.upgrades[cfg::UP_ARMOR]];
    p.damage = cfg::WEAPON_DAMAGE[ps.upgrades[cfg::UP_WEAPON]];
    p.fireInterval = cfg::WEAPON_RATE[ps.upgrades[cfg::UP_WEAPON]];
    p.cargoMax = cfg::CARGO_VALUES[ps.upgrades[cfg::UP_CARGO]];
    if (refill) p.hp = p.maxHp;
    else if (p.maxHp > oldMax) p.hp += p.maxHp - oldMax;
}

void World::GoalReached(PlayerState& ps) {
    // Mehrspieler: Ziel erreicht -> Eintrag in die Bestenliste, dann beginnt das
    // Ziel von vorn. Das Schiff behält seine Aufrüstungen (nächste Runde).
    goals.push_back({ps.id, ps.roundTime, ps.round});
    ++ps.goals;
    Notify(ps, Fmt("ZIEL ERREICHT! Runde %d geschafft.", ps.round), Color{110, 240, 170, 255});
    Notify(ps, "Neue Runde: Dein Schiff behält seine Aufrüstungen.", Color{200, 220, 255, 255});
    if (const Ship* p = PlayerShip(ps)) SoundAt(Sfx::Victory, p->pos, 1.0f, 1.3f, ps.id);
    ps.balance = 0;
    ps.round++;
    ps.roundTime = 0.0f;
}

void World::PlayerShipLost(Ship& s) {
    // Mehrspieler: Das Schiff ist aufgeschlagen. Der Spieler bekommt nach kurzer
    // Zeit ein neues Schiff und fängt mit seinem Fortschritt von vorne an.
    PlayerState* ps = FindPlayer(s.playerId);
    if (!ps) return;
    ResetProgress(*ps);
    ++ps->deaths;
    ps->respawnT = cfg::MP_RESPAWN_TIME;
    ps->shipId = -1;
}

// ===========================================================================
// Kampf
// ===========================================================================
void World::FireBullet(Ship& s) {
    Vector3 fwd = ForwardFromYaw(s.yaw);
    float spread = s.kind == ShipKind::Player ? 0.0f : Rand(-0.045f, 0.045f);
    Vector3 dir = ForwardFromYaw(s.yaw + spread);
    Vector3 side{fwd.z, 0.0f, -fwd.x};
    static int toggle = 0;
    toggle ^= 1;
    Bullet b;
    b.pos = Vector3Add(s.pos, Vector3Add(Vector3Scale(fwd, 1.5f), Vector3Scale(side, toggle ? 0.55f : -0.55f)));
    b.pos.y = s.pos.y + 0.25f;
    b.vel = Vector3Add(Vector3Scale(dir, cfg::BULLET_SPEED), Vector3Scale(s.vel, 0.4f));
    b.vel.y = 0.0f;
    b.life = cfg::BULLET_LIFE;
    b.damage = s.damage;
    b.faction = s.faction;
    b.ownerId = s.id;
    b.ownerPlayer = s.playerId;
    bullets.push_back(b);
    SoundAt(s.kind == ShipKind::Player ? Sfx::PlayerShot : Sfx::EnemyShot, s.pos, 1.0f, 0.0f, -1, s.id);
}

void World::UpdateTurrets(float dt) {
    for (auto& st : stations) {
        int stage = st.type == StationType::Trade ? StationStage() : 0;
        for (auto& tu : st.turrets) {
            if (tu.stage > stage) continue;
            tu.cd -= dt;
            Vector3 wp = Vector3Add(st.pos, tu.offset);
            Ship* best = nullptr;
            float bestD = cfg::TURRET_RANGE;
            for (auto& s : ships) {
                if (s.dead || s.crashing || s.hidden || s.faction != Faction::Pirate) continue;
                float d = Dist2D(s.pos, wp);
                if (d < bestD) { bestD = d; best = &s; }
            }
            if (!best) continue;
            Vector3 lead = Vector3Add(best->pos, Vector3Scale(best->vel, bestD / cfg::BULLET_SPEED));
            float want = YawTowards(wp, lead);
            float diff = AngleDiff(tu.yaw, want);
            tu.yaw += Clamp(diff, -4.0f * dt, 4.0f * dt);
            if (tu.cd <= 0.0f && fabsf(diff) < 0.15f) {
                tu.cd = 0.55f;
                Bullet b;
                b.pos = {wp.x, 1.4f, wp.z};
                Vector3 dir = Vector3Normalize(Vector3Subtract({lead.x, best->pos.y, lead.z}, b.pos));
                b.pos = Vector3Add(b.pos, Vector3Scale(dir, 1.2f));
                b.vel = Vector3Scale(dir, cfg::BULLET_SPEED);
                b.life = cfg::TURRET_RANGE / cfg::BULLET_SPEED + 0.2f;
                b.damage = 14.0f;
                b.faction = Faction::Neutral;
                b.ownerId = -1;
                bullets.push_back(b);
                SoundAt(Sfx::TurretShot, wp);
            }
        }
    }
}

void World::UpdateBullets(float dt) {
    for (auto& b : bullets) {
        b.pos = Vector3Add(b.pos, Vector3Scale(b.vel, dt));
        b.life -= dt;
        if (b.life <= 0) continue;
        for (auto& s : ships) {
            if (s.dead || s.crashing || s.hidden || s.id == b.ownerId) continue;
            if (isServer && s.docked) continue;   // angedockte Spieler sind geschützt
            bool canHit;
            if (b.ownerId == -1) canHit = s.faction == Faction::Pirate;              // Geschützturm
            else if (b.faction == Faction::Player) canHit = s.faction != Faction::Player;
            else if (b.faction == Faction::Pirate) canHit = s.faction != Faction::Pirate;
            else canHit = s.faction != Faction::Neutral;                              // wütender Händler
            if (!canHit) continue;
            if (Vector3Distance(b.pos, s.pos) < cfg::SHIP_HIT_RADIUS) {
                b.life = 0.0f;
                SpawnSparks(b.pos, b.faction == Faction::Player ? Color{255, 200, 90, 255} : Color{255, 90, 80, 255}, 7);
                SoundAt(Sfx::HitMetal, b.pos);
                if (s.kind == ShipKind::Player) SoundAt(Sfx::ShieldHit, s.pos);
                DamageShip(s, b.damage, b.ownerId, b.faction, b.ownerPlayer);
                break;
            }
        }
    }
    bullets.erase(std::remove_if(bullets.begin(), bullets.end(), [](const Bullet& b) { return b.life <= 0; }),
                  bullets.end());
}

void World::DamageShip(Ship& s, float dmg, int attackerId, Faction attacker, int attackerPlayerId) {
    if (s.crashing || s.dead) return;
    s.hp -= dmg;
    s.hitFlash = 1.0f;

    PlayerState* attackerPlayer = attackerPlayerId >= 0 ? FindPlayer(attackerPlayerId) : nullptr;

    if (s.kind == ShipKind::Player) {
        if (!isServer) shake = std::min(1.0f, shake + 0.45f);
    } else if (s.faction == Faction::Neutral) {
        if (attacker == Faction::Player && attackerId >= 0) {
            if (!s.hostileToPlayer && !simulation && attackerPlayer)
                Notify(*attackerPlayer, "Ein Händler wehrt sich gegen deinen Angriff!", Color{255, 220, 120, 255});
            s.hostileToPlayer = true;
            s.ai = AiState::Attack;
            s.targetShip = attackerId;
            s.aggroT = 25.0f;
            s.mining = false;
        } else if (attacker == Faction::Pirate) {
            s.ai = AiState::Flee;
            s.aiTimer = 8.0f;
            s.mining = false;
        }
    } else if (s.faction == Faction::Pirate && attackerId >= 0) {
        s.ai = AiState::Attack;
        s.targetShip = attackerId;
        s.aggroT = 10.0f;
    }

    if (s.hp <= 0.0f) {
        s.hp = 0.0f;
        s.crashing = true;
        s.crashT = 0.0f;
        s.crashSpin = Rand(0, 1) < 0.5f ? Rand(-5.0f, -3.0f) : Rand(3.0f, 5.0f);
        s.vel.y = 1.5f;
        s.mining = false;
        Explosion(s.pos, 0.7f);
        SoundAt(Sfx::ShipExplode, s.pos);
        if (s.kind == ShipKind::Player) {
            if (PlayerState* ps = FindPlayer(s.playerId)) {
                ps->mining = false;
                ps->scanOn = false;
                Notify(*ps, "Schild zerstört! Dein Schiff stürzt ab!", Color{255, 110, 90, 255});
            }
        } else if (attacker == Faction::Player && attackerId >= 0 && attackerPlayer) {
            ++attackerPlayer->kills;
            if (!simulation) Notify(*attackerPlayer, s.faction == Faction::Pirate ? "Pirat abgeschossen! Wrack kann abgebaut werden."
                                                                                : "Händler abgeschossen. Wrack kann abgebaut werden.",
                                    Color{255, 210, 130, 255});
        }
    }
}

// ===========================================================================
// Effekte
// ===========================================================================
void World::Explosion(Vector3 pos, float size) {
    if (isServer) {
        // Der Server erzeugt keine Partikel, er meldet die Explosion nur an die Clients
        FxEvent e;
        e.type = FxEvent::Type::Explosion;
        e.pos = pos;
        e.size = size;
        fx.push_back(e);
        return;
    }
    for (int i = 0; i < static_cast<int>(26 * size); ++i) {
        Particle p;
        p.pos = pos;
        Vector3 dir = Vector3Normalize({Rand(-1, 1), Rand(-0.3f, 1.0f), Rand(-1, 1)});
        p.vel = Vector3Scale(dir, Rand(3.0f, 10.0f) * size);
        p.life = p.maxLife = Rand(0.4f, 0.9f);
        p.size = Rand(0.35f, 0.8f) * size;
        int c = RandInt(0, 2);
        p.color = c == 0 ? Color{255, 230, 140, 255} : (c == 1 ? Color{255, 150, 50, 255} : Color{255, 90, 40, 255});
        p.glow = true;
        particles.push_back(p);
    }
    for (int i = 0; i < static_cast<int>(12 * size); ++i) {
        Particle p;
        p.pos = pos;
        p.vel = {Rand(-6, 6), Rand(2, 9), Rand(-6, 6)};
        p.life = p.maxLife = Rand(0.8f, 1.6f);
        p.size = Rand(0.15f, 0.35f);
        p.color = RandInt(0, 1) ? Color{200, 205, 215, 255} : Color{70, 76, 86, 255};
        p.gravity = 14.0f;
        particles.push_back(p);
    }
    Particle flash;
    flash.pos = pos;
    flash.life = flash.maxLife = 0.25f;
    flash.size = 3.0f * size;
    flash.color = {255, 240, 200, 255};
    flash.glow = true;
    particles.push_back(flash);
}

void World::SpawnSparks(Vector3 pos, Color c, int n) {
    if (isServer) {
        FxEvent e;
        e.type = FxEvent::Type::Sparks;
        e.pos = pos;
        e.color = c;
        e.count = n;
        fx.push_back(e);
        return;
    }
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = pos;
        p.vel = {Rand(-5, 5), Rand(-1, 5), Rand(-5, 5)};
        p.life = p.maxLife = Rand(0.2f, 0.45f);
        p.size = Rand(0.1f, 0.22f);
        p.color = c;
        p.glow = true;
        particles.push_back(p);
    }
}

void World::UpdateParticles(float dt) {
    for (auto& p : particles) {
        p.vel.y -= p.gravity * dt;
        p.pos = Vector3Add(p.pos, Vector3Scale(p.vel, dt));
        if (p.gravity > 0 && p.pos.y < 0.1f) { p.pos.y = 0.1f; p.vel = Vector3Scale(p.vel, 0.3f); }
        p.life -= dt;
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life <= 0; }),
                    particles.end());
    if (particles.size() > 4000) particles.erase(particles.begin(), particles.begin() + 1000);
    shake = std::max(0.0f, shake - dt * 2.5f);
}

void World::Notify(PlayerState& ps, const std::string& text, Color c) {
    if (isServer) ps.outbox.push_back({text, 4.0f, c, 1});   // der Server verschickt die Meldung
    for (auto& n : ps.notices) {
        if (n.text == text) { n.time = 4.0f; n.count++; return; }
    }
    ps.notices.push_back({text, 4.0f, c, 1});
    if (ps.notices.size() > 5) ps.notices.erase(ps.notices.begin());
}

void World::SoundAt(Sfx id, Vector3 pos, float volume, float delay, int forPlayer, int sourceShip) {
    SoundEvent e;
    e.id = id;
    e.pos = pos;
    e.volume = volume;
    e.delay = delay;
    e.forPlayer = forPlayer;
    e.sourceShip = sourceShip;
    sounds.push_back(e);
}

// ===========================================================================
// Suchen
// ===========================================================================
Ship* World::FindShip(int id) {
    for (auto& s : ships) if (s.id == id && !s.dead) return &s;
    return nullptr;
}
const Ship* World::FindShip(int id) const {
    for (const auto& s : ships) if (s.id == id && !s.dead) return &s;
    return nullptr;
}
Deposit* World::FindDeposit(int id) {
    for (auto& d : deposits) if (d.id == id) return &d;
    return nullptr;
}
Wreck* World::FindWreck(int id) {
    for (auto& w : wrecks) if (w.id == id) return &w;
    return nullptr;
}
