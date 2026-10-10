// Verhalten der anderen Raumschiffe und der Autopilot für den Simulationsmodus.
#include "World.hpp"
#include "Assets.hpp"
#include <algorithm>
#include <cmath>

namespace {

float Dist2D(Vector3 a, Vector3 b) { return Vector2Distance({a.x, a.z}, {b.x, b.z}); }

// Steuert ein Schiff auf einen Zielpunkt zu
ShipInput SteerTo(const Ship& s, Vector3 target, float arrive, float throttle = 1.0f) {
    ShipInput in;
    float desired = YawTowards(s.pos, target);
    float diff = AngleDiff(s.yaw, desired);
    in.turn = Clamp(diff * 2.8f, -1.0f, 1.0f);
    float d = Dist2D(s.pos, target);
    if (d > arrive && fabsf(diff) < 1.3f) {
        in.thrust = throttle;
        if (d < arrive * 2.5f) in.thrust *= 0.45f;
    }
    return in;
}

// Zielt mit Vorhalt auf ein Schiff
Vector3 LeadPoint(const Ship& shooter, const Ship& target) {
    float d = Dist2D(shooter.pos, target.pos);
    float t = d / cfg::BULLET_SPEED;
    return Vector3Add(target.pos, Vector3Scale(target.vel, t));
}

bool InsideSafeZone(const World& w, Vector3 p, float extra = 0.0f) {
    for (const auto& st : w.stations)
        if (Dist2D(p, st.pos) < cfg::STATION_SAFE_RADIUS + extra) return true;
    return false;
}

// Kampfmanöver: anfliegen, schießen, abdrehen
ShipInput Dogfight(World& w, Ship& s, const Ship& target, float dt, bool& wantFire) {
    ShipInput in;
    wantFire = false;
    float d = Dist2D(s.pos, target.pos);
    if (s.breakOff > 0.0f) {
        s.breakOff -= dt;
        return SteerTo(s, s.waypoint, 2.0f, 1.0f);
    }
    Vector3 lead = LeadPoint(s, target);
    float desired = YawTowards(s.pos, lead);
    float diff = AngleDiff(s.yaw, desired);
    in.turn = Clamp(diff * 3.2f, -1.0f, 1.0f);
    in.thrust = d > 14.0f ? 1.0f : (d > 8.0f ? 0.35f : -0.2f);
    if (d < 6.5f) {
        // zu nah: abdrehen und neu anfliegen
        s.breakOff = w.Rand(0.9f, 1.6f);
        Vector3 fwd = ForwardFromYaw(s.yaw);
        Vector3 side{fwd.z, 0, -fwd.x};
        float sgn = w.Rand(0, 1) < 0.5f ? -1.0f : 1.0f;
        s.waypoint = Vector3Add(s.pos, Vector3Add(Vector3Scale(fwd, 14.0f), Vector3Scale(side, 14.0f * sgn)));
    }
    wantFire = fabsf(diff) < 0.2f && d < 34.0f;
    return in;
}

} // namespace

// ===========================================================================
// KI der Piraten und Händler
// ===========================================================================
void World::UpdateAi(Ship& s, float dt) {
    ShipInput in;
    bool fire = false;
    s.fireCd -= dt;
    s.hitFlash = std::max(0.0f, s.hitFlash - dt * 3.0f);
    s.aggroT = std::max(0.0f, s.aggroT - dt);
    s.thinkT -= dt;
    s.aiTimer -= dt;
    Ship& player = Player();
    const bool playerAvailable = !player.crashing && !player.dead && !player.hidden && dockedAt < 0;

    if (s.faction == Faction::Pirate) {
        // ---------------- Pirat -------------------------------------------
        if (s.thinkT <= 0.0f) {
            s.thinkT = 0.35f;
            Ship* best = nullptr;
            float bestScore = 1e9f;
            for (auto& o : ships) {
                if (&o == &s || o.dead || o.crashing || o.hidden || o.faction == Faction::Pirate) continue;
                if (o.kind == ShipKind::Player && !playerAvailable) continue;
                float d = Dist2D(o.pos, s.pos);
                float range = cfg::PIRATE_AGGRO;
                if (o.kind == ShipKind::Miner) range = 24.0f;
                if (s.aggroT > 0.0f && o.id == s.targetShip) range = 70.0f;
                if (d > range) continue;
                if (InsideSafeZone(*this, o.pos)) continue;
                float score = o.kind == ShipKind::Player ? d * 0.8f : d;
                if (score < bestScore) { bestScore = score; best = &o; }
            }
            if (best) {
                s.ai = AiState::Attack;
                s.targetShip = best->id;
            } else if (s.ai == AiState::Attack) {
                s.ai = AiState::Wander;
                s.aiTimer = 0.0f;
            }
        }

        if (s.ai == AiState::Attack) {
            Ship* t = FindShip(s.targetShip);
            if (!t || t->crashing || t->hidden || (t->kind == ShipKind::Player && !playerAvailable)) {
                s.ai = AiState::Wander;
                s.aiTimer = 0.0f;
            } else {
                in = Dogfight(*this, s, *t, dt, fire);
            }
        }
        if (s.ai != AiState::Attack) {
            if (s.aiTimer <= 0.0f || Dist2D(s.pos, s.waypoint) < 5.0f) {
                Vector3 p = s.pos;
                for (int i = 0; i < 20; ++i) {
                    p = {s.pos.x + Rand(-60, 60), 0, s.pos.z + Rand(-60, 60)};
                    p.x = Clamp(p.x, -cfg::WORLD_HALF + 10, cfg::WORLD_HALF - 10);
                    p.z = Clamp(p.z, -cfg::WORLD_HALF + 10, cfg::WORLD_HALF - 10);
                    if (!InsideSafeZone(*this, p, 12.0f)) break;
                }
                s.waypoint = p;
                s.aiTimer = Rand(8.0f, 16.0f);
            }
            in = SteerTo(s, s.waypoint, 3.0f, 0.6f);
        }
        // Stationen meiden (Geschütztürme)
        for (const auto& st : stations) {
            float d = Dist2D(s.pos, st.pos);
            if (d < cfg::STATION_SAFE_RADIUS + 4.0f) {
                Vector3 away = Vector3Add(s.pos, Vector3Scale(Vector3Normalize(Vector3Subtract(s.pos, st.pos)), 20.0f));
                in = SteerTo(s, away, 1.0f, 1.0f);
                fire = false;
                s.ai = AiState::Wander;
                s.waypoint = away;
                s.aiTimer = 4.0f;
            }
        }
    } else {
        // ---------------- Händler / Miner ------------------------------------
        if (s.ai != AiState::Mining) s.mining = false;
        switch (s.ai) {
            case AiState::Attack: {
                Ship* t = FindShip(s.targetShip);
                bool lost = !t || t->crashing || t->hidden || (t->kind == ShipKind::Player && !playerAvailable) ||
                            Dist2D(s.pos, t->pos) > 60.0f || s.aggroT <= 0.0f;
                if (lost) {
                    s.ai = s.cargo >= s.cargoMax * 0.8f ? AiState::GoSell : AiState::GoMine;
                    s.targetDeposit = -1;
                } else {
                    in = Dogfight(*this, s, *t, dt, fire);
                }
                break;
            }
            case AiState::Flee: {
                in = SteerTo(s, stations[0].pos, 8.0f, 1.0f);
                if (s.aiTimer <= 0.0f || Dist2D(s.pos, stations[0].pos) < cfg::STATION_SAFE_RADIUS * 0.6f)
                    s.ai = AiState::GoSell;
                break;
            }
            case AiState::GoMine: {
                Deposit* dep = FindDeposit(s.targetDeposit);
                if (!dep || dep->amount <= 0) {
                    // Eine der nächsten Lagerstätten wählen, die kein anderer Händler anfliegt
                    std::vector<std::pair<float, int>> cand;
                    for (auto& d : deposits) {
                        bool taken = false;
                        for (auto& o : ships)
                            if (&o != &s && o.kind == ShipKind::Miner && o.targetDeposit == d.id) taken = true;
                        if (!taken && d.amount > 0) cand.push_back({Dist2D(d.pos, s.pos), d.id});
                    }
                    std::sort(cand.begin(), cand.end());
                    if (!cand.empty()) {
                        int pick = RandInt(0, std::min<int>(2, static_cast<int>(cand.size()) - 1));
                        s.targetDeposit = cand[pick].second;
                    }
                    dep = FindDeposit(s.targetDeposit);
                }
                if (dep) {
                    in = SteerTo(s, dep->pos, 3.5f, 0.85f);
                    if (Dist2D(s.pos, dep->pos) < 5.0f) {
                        s.ai = AiState::Mining;
                        s.aiTimer = cfg::MINING_TIME;
                        s.mining = true;
                        s.mineTarget = dep->pos;
                    }
                }
                break;
            }
            case AiState::Mining: {
                Deposit* dep = FindDeposit(s.targetDeposit);
                if (!dep) { s.ai = AiState::GoMine; s.mining = false; break; }
                s.mining = true;
                s.mineTarget = dep->pos;
                if (s.aiTimer <= 0.0f) {
                    int take = std::min(s.cargoMax - s.cargo, dep->amount);
                    dep->amount -= take;
                    s.cargo += take;
                    s.mining = false;
                    s.targetDeposit = -1;
                    s.ai = s.cargo >= s.cargoMax * 0.8f ? AiState::GoSell : AiState::GoMine;
                }
                break;
            }
            case AiState::GoSell: {
                in = SteerTo(s, stations[0].pos, 6.0f, 0.85f);
                if (Dist2D(s.pos, stations[0].pos) < 10.0f) {
                    s.ai = AiState::Docked;
                    s.hidden = true;
                    s.aiTimer = Rand(4.0f, 9.0f);
                    s.cargo = 0;
                    s.vel = {0, 0, 0};
                }
                break;
            }
            case AiState::Docked: {
                if (s.aiTimer <= 0.0f) {
                    s.hidden = false;
                    float a = Rand(0, 2 * PI);
                    s.pos = {stations[0].pos.x + cosf(a) * 13.0f, cfg::SHIP_ALTITUDE, stations[0].pos.z + sinf(a) * 13.0f};
                    s.yaw = YawTowards(stations[0].pos, s.pos);
                    s.ai = AiState::GoMine;
                    s.targetDeposit = -1;
                }
                return;
            }
            default:
                s.ai = AiState::GoMine;
                break;
        }
    }

    if (fire && s.fireCd <= 0.0f) {
        FireBullet(s);
        s.fireCd = s.fireInterval;
    }
    UpdateShipPhysics(s, dt, in);
}

// ===========================================================================
// Autopilot (Simulationsmodus)
// ===========================================================================
namespace {
struct AutopilotState {
    Vector3 explore{};
    float exploreT = 0.0f;
    float interactCd = 0.0f;
    bool hasExplore = false;
} g_ap;

int CheapestUpgrade(const World& w, long long budget) {
    // Reihenfolge bei gleichem Preis: Lager, Waffen, Panzerung, Antrieb
    const int order[4] = {cfg::UP_CARGO, cfg::UP_WEAPON, cfg::UP_ARMOR, cfg::UP_SPEED};
    int best = -1;
    int bestCost = 1 << 30;
    for (int t : order) {
        int c = w.UpgradeCost(t);
        if (c >= 0 && c <= budget && c < bestCost) { best = t; bestCost = c; }
    }
    return best;
}
} // namespace

void ResetAutopilot() { g_ap = AutopilotState{}; }

void AutopilotDockActions(World& w) {
    if (w.dockedAt == 0) {
        w.Repair();
    } else if (w.dockedAt == 1) {
        for (int i = 0; i < 8; ++i) {
            int t = CheapestUpgrade(w, w.balance - 300);
            if (t < 0 || !w.BuyUpgrade(t)) break;
            static const char* names[4] = {"Antrieb", "Panzerung", "Bewaffnung", "Lager"};
            w.Notify(TextFormat("Autopilot kauft: %s Stufe %d", names[t], w.upgrades[t]), Color{255, 210, 130, 255});
        }
    }
}

ShipInput AutopilotInput(World& w, float dt) {
    ShipInput in;
    Ship& p = w.Player();
    if (p.crashing || p.dead || p.hidden || w.dockedAt >= 0) return in;
    g_ap.interactCd -= dt;
    g_ap.exploreT -= dt;

    auto go = [&](Vector3 target, float arrive, float throttle = 1.0f) {
        ShipInput s = SteerTo(p, target, arrive, throttle);
        in.thrust = s.thrust;
        in.turn = s.turn;
    };
    auto dock = [&](int station) {
        go(w.stations[station].pos, 4.0f);
        if (w.NearStation(station, cfg::DOCK_RANGE - 3.0f) && g_ap.interactCd <= 0.0f) {
            in.interact = true;
            g_ap.interactCd = 1.0f;
        }
    };

    // --- Bedrohung? ----------------------------------------------------------
    Ship* threat = nullptr;
    float threatD = 27.0f;
    for (auto& s : w.ships) {
        if (s.dead || s.crashing || s.hidden || s.kind == ShipKind::Player) continue;
        bool hostile = s.faction == Faction::Pirate || s.hostileToPlayer;
        if (!hostile) continue;
        float d = Dist2D(s.pos, p.pos);
        if (d < threatD) { threatD = d; threat = &s; }
    }
    const float shield = p.hp / p.maxHp;
    if (threat) {
        if (shield < 0.35f) {
            if (w.mining) { in.interact = true; return in; } // Abbau abbrechen
            dock(0);
            return in;
        }
        if (w.mining && threatD > 16.0f) {
            // weiter abbauen, aber Gegner im Blick behalten
        } else {
            if (w.mining) in.interact = true;
            Vector3 lead = LeadPoint(p, *threat);
            float desired = YawTowards(p.pos, lead);
            float diff = AngleDiff(p.yaw, desired);
            in.turn = Clamp(diff * 3.2f, -1.0f, 1.0f);
            in.thrust = threatD > 15.0f ? 0.9f : (threatD > 8.0f ? 0.2f : -0.5f);
            in.fire = fabsf(diff) < 0.22f && threatD < 30.0f;
            return in;
        }
    }

    if (w.mining) return in; // warten, bis das Lager gefüllt ist

    // --- Reparieren / Verkaufen ------------------------------------------------
    if (shield < 0.6f && w.balance >= 50) { dock(0); return in; }
    if (p.cargo >= p.cargoMax * 0.95f) { dock(0); return in; }

    // --- Aufrüsten ---------------------------------------------------------------
    if (w.balance < cfg::GOAL_POINTS - 2000 && CheapestUpgrade(w, w.balance - 300) >= 0) {
        dock(1);
        return in;
    }

    // --- Nächstes Abbauziel ------------------------------------------------------
    Vector3 target{};
    float best = 1e9f;
    bool found = false;
    for (auto& wr : w.wrecks) {
        float d = Dist2D(wr.pos, p.pos);
        if (wr.amount > 0 && d < 90.0f && d < best) { best = d; target = wr.pos; found = true; }
    }
    for (auto& d : w.deposits) {
        if (!d.revealed || d.amount <= 0) continue;
        float dist = Dist2D(d.pos, p.pos) + 10.0f; // Wracks leicht bevorzugen
        if (dist < best) { best = dist; target = d.pos; found = true; }
    }
    if (found) {
        go(target, cfg::MINING_RANGE * 0.5f, 1.0f);
        if (Dist2D(target, p.pos) < cfg::MINING_RANGE * 0.8f && g_ap.interactCd <= 0.0f) {
            in.interact = true;
            g_ap.interactCd = 0.8f;
        }
        return in;
    }

    // --- Erkunden und scannen ------------------------------------------------------
    if (!g_ap.hasExplore || g_ap.exploreT <= 0.0f || Dist2D(p.pos, g_ap.explore) < 8.0f) {
        const float lim = cfg::WORLD_HALF - 15.0f;
        g_ap.explore = {w.Rand(-lim, lim), 0, w.Rand(-lim, lim)};
        g_ap.exploreT = 25.0f;
        g_ap.hasExplore = true;
    }
    go(g_ap.explore, 4.0f, 1.0f);
    if (!w.scanOn && w.scanEnergy >= cfg::SCANNER_MAX_ENERGY - 0.01f) in.scanToggle = true;
    return in;
}
