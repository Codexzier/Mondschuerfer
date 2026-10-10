// Spiellogik: Bewegung, Scanner, Abbau, Kampf, Stationen.
#include "World.hpp"
#include "Assets.hpp"
#include <algorithm>
#include <cmath>

namespace {
float Dist2D(Vector3 a, Vector3 b) { return Vector2Distance({a.x, a.z}, {b.x, b.z}); }

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

    Ship p = MakeShip(ShipKind::Player, {0.0f, cfg::SHIP_ALTITUDE, 24.0f});
    p.yaw = PI; // Blick nach Süden, weg von der Station
    ships.push_back(p);
    ApplyUpgrades(true);

    for (int i = 0; i < cfg::DEPOSIT_COUNT; ++i) SpawnDeposit(true);
    for (int i = 0; i < cfg::PIRATES_BASE; ++i) SpawnPirate();
    for (int i = 0; i < cfg::MINERS_COUNT; ++i) SpawnMiner(i == 0);

    lastStage = StationStage();
    if (!simulation) {
        Notify("Willkommen, Pilot! Finde Kristalle mit dem Scanner [E].", Color{255, 200, 120, 255});
        Notify("Ziel: 100.000 Ressourcenpunkte auf dem Konto.", Color{200, 220, 255, 255});
    }
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
    Vector3 p = atStation ? Vector3{stations[0].pos.x + Rand(-12, 12), 0, stations[0].pos.z + 16.0f}
                          : RandomFreePos(30.0f, 30.0f);
    Ship s = MakeShip(ShipKind::Miner, p);
    ships.push_back(s);
}

// ===========================================================================
// Hauptupdate
// ===========================================================================
void World::Update(float dt, const ShipInput& input) {
    time += dt;
    if (outcome == Outcome::None) playTime += dt;
    else outcomeT += dt;

    UpdatePlayer(dt, input);

    for (size_t i = 1; i < ships.size(); ++i) {
        Ship& s = ships[i];
        if (s.dead || s.crashing || (s.hidden && s.ai != AiState::Docked)) continue;
        UpdateAi(s, dt);
    }

    // Abstürzende Schiffe
    for (auto& s : ships) {
        if (!s.crashing || s.dead) continue;
        s.crashT += dt;
        s.vel.y -= 11.0f * dt;
        s.vel.x *= (1.0f - 0.6f * dt);
        s.vel.z *= (1.0f - 0.6f * dt);
        s.pos = Vector3Add(s.pos, Vector3Scale(s.vel, dt));
        s.yaw += s.crashSpin * dt;
        s.bank += s.crashSpin * 0.6f * dt;
        if (RandInt(0, 2) == 0) {
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
            s.dead = true;
            if (s.kind == ShipKind::Player) {
                s.hidden = true;
                outcome = Outcome::Dead;
                outcomeT = 0.0f;
            } else {
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
        if (RandInt(0, 30) == 0 && w.age < 60.0f) {
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
    (void)removedDeposits;
    wrecks.erase(std::remove_if(wrecks.begin(), wrecks.end(),
                                [](const Wreck& w) { return w.amount <= 0 || w.age > 300.0f; }),
                 wrecks.end());

    // Neue Lagerstätten
    if (static_cast<int>(deposits.size()) < cfg::DEPOSIT_COUNT) {
        depositSpawnT -= dt;
        if (depositSpawnT <= 0.0f) {
            SpawnDeposit(false);
            depositSpawnT = cfg::DEPOSIT_RESPAWN * Rand(0.6f, 1.2f);
        }
    }

    // Piraten nachschieben (mehr, je weiter das Spiel fortgeschritten ist)
    int pirates = 0, miners = 0;
    for (const auto& s : ships) {
        if (s.dead) continue;
        if (s.faction == Faction::Pirate) ++pirates;
        if (s.kind == ShipKind::Miner) ++miners;
    }
    int wantPirates = std::min<int>(cfg::PIRATES_MAX, cfg::PIRATES_BASE + static_cast<int>(totalDelivered / 12000));
    pirateSpawnT -= dt;
    if (pirates < wantPirates && pirateSpawnT <= 0.0f) {
        SpawnPirate();
        pirateSpawnT = Rand(9.0f, 18.0f);
    }
    minerSpawnT -= dt;
    if (miners < cfg::MINERS_COUNT && minerSpawnT <= 0.0f) {
        SpawnMiner(true);
        minerSpawnT = Rand(25.0f, 40.0f);
    }

    // Tote Schiffe entfernen (Spieler bleibt immer an Index 0)
    if (ships.size() > 1) {
        ships.erase(std::remove_if(ships.begin() + 1, ships.end(), [](const Ship& s) { return s.dead; }),
                    ships.end());
    }

    for (auto& n : notices) n.time -= dt;
    notices.erase(std::remove_if(notices.begin(), notices.end(), [](const Notice& n) { return n.time <= 0; }),
                  notices.end());
    if (stageAnim > 0.0f) stageAnim = std::max(0.0f, stageAnim - dt * 0.6f);
    for (auto& d : deposits) if (d.revealed && d.revealT < 1.0f) d.revealT = std::min(1.0f, d.revealT + dt * 2.0f);
}

void World::UpdateAmbient(float dt) {
    if (dt <= 0.0f) return;
    time += dt;
    UpdateParticles(dt);
    for (auto& n : notices) n.time -= dt;
    notices.erase(std::remove_if(notices.begin(), notices.end(), [](const Notice& n) { return n.time <= 0; }),
                  notices.end());
    if (stageAnim > 0.0f) stageAnim = std::max(0.0f, stageAnim - dt * 0.6f);
}

// ===========================================================================
// Spieler
// ===========================================================================
void World::UpdatePlayer(float dt, const ShipInput& in) {
    Ship& p = Player();
    if (p.dead || p.crashing || p.hidden) return;

    UpdateShipPhysics(p, dt, in);
    p.fireCd -= dt;
    p.hitFlash = std::max(0.0f, p.hitFlash - dt * 3.0f);

    // --- Scanner ---------------------------------------------------------
    if (in.scanToggle) {
        if (scanOn) {
            scanOn = false;
        } else if (scanEnergy >= 1.0f) {
            scanOn = true;
            scanPulse = 0.0f;
        } else if (!simulation) {
            Notify("Scanner-Energie zu niedrig", Color{255, 170, 120, 255});
        }
    }
    if (scanOn) {
        scanEnergy -= cfg::SCANNER_DRAIN * dt;
        scanPulse += dt;
        float ring = cfg::SCANNER_RADIUS * fmodf(scanPulse, cfg::SCANNER_PULSE_TIME) / cfg::SCANNER_PULSE_TIME;
        int found = 0;
        for (auto& d : deposits) {
            if (!d.revealed && Dist2D(d.pos, p.pos) <= ring) {
                d.revealed = true;
                d.revealT = 0.0f;
                ++found;
            }
        }
        if (found > 0 && !simulation)
            Notify(found == 1 ? "Lagerstätte entdeckt!" : TextFormat("%d Lagerstätten entdeckt!", found),
                   Color{120, 255, 190, 255});
        if (scanEnergy <= 0.0f) {
            scanEnergy = 0.0f;
            scanOn = false;
            if (!simulation) Notify("Scanner abgeschaltet: Energie leer", Color{255, 170, 120, 255});
        }
    } else {
        scanEnergy = std::min(cfg::SCANNER_MAX_ENERGY, scanEnergy + cfg::SCANNER_REGEN * dt);
    }

    // --- Waffe -------------------------------------------------------------
    if (in.fire && p.fireCd <= 0.0f) {
        FireBullet(p);
        p.fireCd = p.fireInterval;
    }

    // --- Interaktion (F) ----------------------------------------------------
    if (in.interact) {
        if (mining) {
            mining = false;
            if (!simulation) Notify("Abbau abgebrochen", Color{255, 200, 150, 255});
        } else {
            int kind = 0, id = -1;
            InteractTarget(&kind, &id);
            if (kind == 3) Dock(0);
            else if (kind == 4) Dock(1);
            else if (kind == 1 || kind == 2) {
                if (p.cargo >= p.cargoMax) {
                    if (!simulation) Notify("Lager voll! Ab zur Handelsstation.", Color{255, 200, 120, 255});
                } else {
                    StartMining(kind, id);
                }
            }
        }
    }

    // --- Abbau-Fortschritt ----------------------------------------------------
    if (mining) {
        Vector3 target{};
        bool valid = false;
        if (mineKind == 1) {
            if (Deposit* d = FindDeposit(mineId)) { target = d->pos; valid = d->amount > 0; }
        } else if (Wreck* w = FindWreck(mineId)) {
            target = w->pos;
            valid = w->amount > 0;
        }
        if (!valid) {
            mining = false;
            if (!simulation) Notify("Lagerstätte erschöpft", Color{255, 200, 150, 255});
        } else if (Dist2D(target, p.pos) > cfg::MINING_RANGE + 1.5f) {
            mining = false;
            if (!simulation) Notify("Abbau abgebrochen: zu weit entfernt", Color{255, 200, 150, 255});
        } else {
            mineT += dt;
            if (RandInt(0, 2) == 0) {
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
            if (mineT >= cfg::MINING_TIME) FinishMining();
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
bool World::NearStation(int station, float range) const {
    const Ship& p = Player();
    return Dist2D(p.pos, stations[station].pos) <= range;
}

int World::InteractTarget(int* kind, int* id) const {
    *kind = 0;
    *id = -1;
    const Ship& p = Player();
    if (NearStation(0, cfg::DOCK_RANGE)) { *kind = 3; return 3; }
    if (NearStation(1, cfg::DOCK_RANGE)) { *kind = 4; return 4; }
    float best = cfg::MINING_RANGE;
    for (const auto& w : wrecks) {
        float d = Dist2D(w.pos, p.pos);
        if (d <= best && w.amount > 0) { best = d; *kind = 2; *id = w.id; }
    }
    if (*kind == 2) return 2;
    best = cfg::MINING_RANGE;
    for (const auto& d : deposits) {
        float dist = Dist2D(d.pos, p.pos);
        if (d.revealed && dist <= best && d.amount > 0) { best = dist; *kind = 1; *id = d.id; }
    }
    return *kind;
}

void World::StartMining(int kind, int id) {
    mining = true;
    mineKind = kind;
    mineId = id;
    mineT = 0.0f;
    if (!simulation) Notify(kind == 2 ? "Wrack wird ausgeschlachtet ..." : "Abbau gestartet ...",
                            Color{150, 255, 200, 255});
}

void World::FinishMining() {
    Ship& p = Player();
    mining = false;
    int free = p.cargoMax - p.cargo;
    int* amount = nullptr;
    if (mineKind == 1) { if (Deposit* d = FindDeposit(mineId)) amount = &d->amount; }
    else if (Wreck* w = FindWreck(mineId)) amount = &w->amount;
    if (!amount) return;
    int take = std::min(free, *amount);
    *amount -= take;
    p.cargo += take;
    if (!simulation) {
        Notify(TextFormat("+%d Ressourcen geladen", take), Color{120, 255, 190, 255});
        if (p.cargo >= p.cargoMax) Notify("Lager voll! Ab zur Handelsstation.", Color{255, 200, 120, 255});
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

void World::Dock(int station) {
    Ship& p = Player();
    dockedAt = station;
    mining = false;
    scanOn = false;
    p.vel = {0, 0, 0};
    lastSale = 0;
    if (station == 0) {
        int sale = p.cargo;
        p.cargo = 0;
        balance += sale;
        totalDelivered += sale;
        lastSale = sale;
        int stage = StationStage();
        if (stage > lastStage) {
            lastStage = stage;
            stageAnim = 1.0f;
            if (!simulation) Notify(TextFormat("Die Station wächst! Ausbaustufe %d", stage), Color{255, 210, 120, 255});
            for (const auto& part : stations[0].parts)
                if (part.stage == stage) SpawnSparks(Vector3Add(stations[0].pos, Vector3Add(part.offset, {0, 1.5f, 0})), Color{255, 220, 120, 255}, 12);
        }
        if (balance >= cfg::GOAL_POINTS && outcome == Outcome::None) {
            outcome = Outcome::Won;
            outcomeT = 0.0f;
        }
    }
    wantAutosave = !simulation;
}

void World::Undock() {
    if (dockedAt < 0) return;
    Ship& p = Player();
    // Schiff zeigt beim Abdocken von der Station weg
    p.yaw = YawTowards(stations[dockedAt].pos, p.pos);
    p.vel = Vector3Scale(ForwardFromYaw(p.yaw), 6.0f);
    dockedAt = -1;
}

int World::RepairCost() const {
    const Ship& p = Player();
    int missing = static_cast<int>(ceilf(p.maxHp - p.hp));
    return std::max(0, missing) * cfg::REPAIR_COST_PER_POINT;
}

bool World::Repair() {
    Ship& p = Player();
    int cost = RepairCost();
    if (cost <= 0) return false;
    if (balance >= cost) {
        balance -= cost;
        p.hp = p.maxHp;
        return true;
    }
    // Teilreparatur mit dem vorhandenen Geld
    int points = static_cast<int>(balance / cfg::REPAIR_COST_PER_POINT);
    if (points <= 0) return false;
    balance -= points * cfg::REPAIR_COST_PER_POINT;
    p.hp = std::min(p.maxHp, p.hp + points);
    return true;
}

int World::UpgradeCost(int type) const {
    if (upgrades[type] >= cfg::UPGRADE_LEVELS) return -1;
    return cfg::UPGRADE_COST[type][upgrades[type]];
}

bool World::BuyUpgrade(int type) {
    int cost = UpgradeCost(type);
    if (cost < 0 || balance < cost) return false;
    balance -= cost;
    upgrades[type]++;
    ApplyUpgrades(false);
    return true;
}

void World::ApplyUpgrades(bool refill) {
    Ship& p = Player();
    float oldMax = p.maxHp;
    p.maxSpeed = cfg::SPEED_VALUES[upgrades[cfg::UP_SPEED]];
    p.maxHp = cfg::ARMOR_VALUES[upgrades[cfg::UP_ARMOR]];
    p.damage = cfg::WEAPON_DAMAGE[upgrades[cfg::UP_WEAPON]];
    p.fireInterval = cfg::WEAPON_RATE[upgrades[cfg::UP_WEAPON]];
    p.cargoMax = cfg::CARGO_VALUES[upgrades[cfg::UP_CARGO]];
    if (refill) p.hp = p.maxHp;
    else if (p.maxHp > oldMax) p.hp += p.maxHp - oldMax;
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
    bullets.push_back(b);
}

void World::UpdateTurrets(float dt) {
    for (int si = 0; si < 2; ++si) {
        Station& st = stations[si];
        int stage = si == 0 ? StationStage() : 0;
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
            bool canHit;
            if (b.ownerId == -1) canHit = s.faction == Faction::Pirate;              // Geschützturm
            else if (b.faction == Faction::Player) canHit = s.faction != Faction::Player;
            else if (b.faction == Faction::Pirate) canHit = s.faction != Faction::Pirate;
            else canHit = s.faction != Faction::Neutral;                              // wütender Händler
            if (!canHit) continue;
            if (Vector3Distance(b.pos, s.pos) < cfg::SHIP_HIT_RADIUS) {
                b.life = 0.0f;
                SpawnSparks(b.pos, b.faction == Faction::Player ? Color{255, 200, 90, 255} : Color{255, 90, 80, 255}, 7);
                DamageShip(s, b.damage, b.ownerId, b.faction);
                break;
            }
        }
    }
    bullets.erase(std::remove_if(bullets.begin(), bullets.end(), [](const Bullet& b) { return b.life <= 0; }),
                  bullets.end());
}

void World::DamageShip(Ship& s, float dmg, int attackerId, Faction attacker) {
    if (s.crashing || s.dead) return;
    s.hp -= dmg;
    s.hitFlash = 1.0f;

    if (s.kind == ShipKind::Player) {
        shake = std::min(1.0f, shake + 0.45f);
    } else if (s.faction == Faction::Neutral) {
        if (attacker == Faction::Player && attackerId >= 0) {
            if (!s.hostileToPlayer && !simulation) Notify("Ein Händler wehrt sich gegen deinen Angriff!", Color{255, 220, 120, 255});
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
        if (s.kind == ShipKind::Player) {
            mining = false;
            scanOn = false;
            Notify("Schild zerstört! Dein Schiff stürzt ab!", Color{255, 110, 90, 255});
        } else if (attacker == Faction::Player && attackerId >= 0) {
            ++kills;
            if (!simulation) Notify(s.faction == Faction::Pirate ? "Pirat abgeschossen! Wrack kann abgebaut werden."
                                                                 : "Händler abgeschossen. Wrack kann abgebaut werden.",
                                    Color{255, 210, 130, 255});
        }
    }
}

// ===========================================================================
// Effekte
// ===========================================================================
void World::Explosion(Vector3 pos, float size) {
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

void World::Notify(const std::string& text, Color c) {
    for (auto& n : notices) {
        if (n.text == text) { n.time = 4.0f; n.count++; return; }
    }
    notices.push_back({text, 4.0f, c, 1});
    if (notices.size() > 5) notices.erase(notices.begin());
}

// ===========================================================================
// Suchen
// ===========================================================================
Ship* World::FindShip(int id) {
    for (auto& s : ships) if (s.id == id && !s.dead) return &s;
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
