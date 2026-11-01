// ---------------------------------------------------------------------------
// Trooper.cpp: Trupp zu Fuß (FPS-Modus) – Rolle Pilot oder SG-Team, Einsatz
// durch ein Stargate (Ankunft, Rückkehr, Punkte), Bewegung mit
// Mondschwerkraft, Hindernisse aus Deko, Stationsteilen und den Treppen vor
// den Toren, Schießen, Nachladen, Proben an gelben Kristallen, Treffer und
// Fallen. Dazu werden hier die KI-Figuren erzeugt: NPC-Kameraden bei der
// Ankunft und Gruppen von Bodenpiraten bei gelben Kristallen (ihr Verhalten
// steht in Ai.cpp).
// Datentypen in World.hpp (Trooper, TrooperInput, TrooperBlock, Stargate,
// SgRecord, Sample).
//
// Wer benutzt die Datei?
//   - World::Update ruft UpdateTroopers auf (nach den Spielern).
//   - World::UpdateBullets fragt TrooperBlocked und SegmentHitsTrooper
//     (Treffer der Bodengeschosse) und ruft DamageTrooper auf.
//   - src/main.cpp: SetRole (Rollenwahl, Stationsfenster), EndSgMission
//     (Pause), TrooperEye/TrooperAim für die Ego-Kamera.
//     src/CommandCenter.cpp: BuildStargates, StartSgMission.
//     src/Hud.cpp: NearestGate, SampleTarget. src/Render.cpp: Tore und
//     Treppen (GateLookAt, GateStairTop ...).
//
// Einzelspieler: Der Spieler wählt nach "Neues Spiel" bzw. "Fortsetzen" die
// Rolle, als SG-Team geht es vom Command Center durch ein Stargate auf die
// Mondkarte oder in eine Einsatzkarte. Mehrspieler: Der Server (GameServer)
// bildet Teams aus bis zu vier Spielern (PlayerState::sgTeam); der Anführer
// kommt zuerst durch das Tor, die anderen folgen (sgFollowers), Proben zählen
// für das ganze Team (TeamSamples).
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

// Kürzester Abstand der Strecken p1-q1 und p2-q2 auf dem Boden. s und u =
// Lage der beiden nächsten Punkte (0..1) auf der ersten bzw. zweiten Strecke.
// Übliches Verfahren (z. B. Ericson, "Real-Time Collision Detection", 5.1.9).
float SegSegDist(Vector2 p1, Vector2 q1, Vector2 p2, Vector2 q2, float& s, float& u) {
    const Vector2 d1 = Vector2Subtract(q1, p1), d2 = Vector2Subtract(q2, p2), r = Vector2Subtract(p1, p2);
    const float a = Vector2DotProduct(d1, d1), e = Vector2DotProduct(d2, d2), f = Vector2DotProduct(d2, r);
    constexpr float eps = 1e-6f;
    if (a <= eps && e <= eps) {
        s = u = 0.0f;
    } else if (a <= eps) {
        s = 0.0f;
        u = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = Vector2DotProduct(d1, r);
        if (e <= eps) {
            u = 0.0f;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = Vector2DotProduct(d1, d2), denom = a * e - b * b;
            s = denom > eps ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            u = (b * s + f) / e;
            if (u < 0.0f) {
                u = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (u > 1.0f) {
                u = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return Vector2Distance(Vector2Add(p1, Vector2Scale(d1, s)), Vector2Add(p2, Vector2Scale(d2, u)));
}

// Formation der Kameraden hinter ihrem Spieler: seitlich (rechts +) und nach
// hinten, in Metern, bezogen auf seine Laufrichtung (Trooper::formYaw).
constexpr Vector2 kMateSlots[3] = {{-0.9f, 1.0f}, {0.9f, 1.0f}, {0.0f, 1.9f}};

// Maße der Kit-Tore gate_simple/gate_complex (wie in Render.cpp), mal
// Deko-Größe: Mitte des Rings 0,585 über dem Boden, innen etwa 0,3 Radius,
// Sockel 0,25 vor und hinter der Fläche (ModelBounds, daraus wird auch das
// Hindernis des Tors).
constexpr float kGateCenter = 0.585f, kGateInner = 0.3f, kGateDepth = 0.25f;
// Die Treppe ist das Modell "stairs" (Space Kit): 7 Stufen, die Oberkanten
// bei 0,1, 0,25, 0,4 ... 1,0 der ganzen Höhe.
constexpr int kStairSteps = 7;
float StairStepTop(float top, int i) { return top * (0.1f + 0.15f * static_cast<float>(i)); }

// Steht noch jemand aus dem Team dieser Spielerfigur: ein Kamerad (KI) oder
// im Mehrspieler die Figur eines Mitspielers?
bool TeamStanding(const World& w, const Trooper& leader) {
    const PlayerState* lp = w.FindPlayer(leader.playerId);
    for (const Trooper& m : w.troopers) {
        if (m.down || m.id == leader.id) continue;
        if (m.playerId < 0 && m.kind == TrooperKind::Mate && m.leaderId == leader.id) return true;
        const PlayerState* mp = m.playerId >= 0 ? w.FindPlayer(m.playerId) : nullptr;
        if (lp && mp && World::SameTeam(*lp, *mp)) return true;
    }
    return false;
}
} // namespace

// Zeit seit Beginn der Anwahl d: Der Ring leuchtet langsam auf, 0,8 s vor
// dem Ende der Anwahl wächst die Fläche, 0,6 s davor schießt der helle Kegel
// heraus (bis kurz nach dem Öffnen). Beim Schließen fällt alles in GATE_CLOSE
// zusammen.
GateLook GateLookAt(GateState state, float t) {
    GateLook l;
    if (state == GateState::Idle) return l;
    if (state == GateState::Closing) {
        const float k = std::clamp(1.0f - t / cfg::GATE_CLOSE, 0.0f, 1.0f);
        l.glow = l.open = k;
        return l;
    }
    const float d = state == GateState::Dialing ? t : cfg::GATE_DIAL + t;
    l.glow = state == GateState::Dialing ? 0.25f + 0.75f * std::clamp(t / cfg::GATE_DIAL, 0.0f, 1.0f) : 1.0f;
    l.open = std::clamp((d - (cfg::GATE_DIAL - 0.8f)) / 0.35f, 0.0f, 1.0f);
    const float k = d - (cfg::GATE_DIAL - 0.6f);
    if (k > 0.0f && k < 1.05f) l.kawoosh = sinf(PI * k / 1.05f);
    return l;
}

Vector3 GateNormal(const Stargate& g) { return {sinf(g.yaw), 0.0f, cosf(g.yaw)}; }
Vector3 GateRight(const Stargate& g) { return {cosf(g.yaw), 0.0f, -sinf(g.yaw)}; }

// Die oberste Stufe liegt 0,4 m unter dem Rand des Wurmlochs (die Figur
// reicht dann mit dem Oberkörper hinein), höchstens aber so hoch, dass jede
// Stufe ohne Sprung zu schaffen ist (15 % der Höhe je Stufe).
float GateStairTop(const Stargate& g) {
    const float top = std::min((kGateCenter - kGateInner) * g.scale - 0.4f, cfg::TROOPER_STEP * 0.95f / 0.15f);
    return top > 0.06f ? top : 0.0f;
}

Vector3 GateExit(const Stargate& g) {
    const Vector3 n = GateNormal(g);
    const float d = kGateDepth * g.scale + cfg::TROOPER_RADIUS + 0.03f;
    return {g.pos.x + n.x * d, GateStairTop(g), g.pos.z + n.z * d};
}

// Das Hindernis des Tors hält die Figur TROOPER_RADIUS vor dem Sockel; wer
// dort auf der obersten Stufe steht, ist im Wurmloch. Ohne Treppe (kleines
// Tor) zählt der Boden vor dem Ring.
bool InGatePassage(const Stargate& g, Vector3 p) {
    const Vector3 d{p.x - g.pos.x, 0.0f, p.z - g.pos.z};
    const float along = Vector3DotProduct(d, GateNormal(g)), side = fabsf(Vector3DotProduct(d, GateRight(g)));
    const float top = GateStairTop(g);
    const float halfW = top > 0.0f ? cfg::GATE_STAIR_WIDTH * 0.5f : kGateInner * g.scale * 0.8f;
    return along > 0.0f && along < kGateDepth * g.scale + cfg::TROOPER_RADIUS + 0.25f && side < halfW && p.y >= top - 0.05f;
}

// Gefallene liegen am Boden: Die Ego-Kamera sinkt mit.
Vector3 TrooperEye(const Trooper& t) { return {t.pos.x, t.pos.y + (t.down ? 0.12f : cfg::TROOPER_EYE), t.pos.z}; }

Vector3 MateSlot(const Trooper& leader, int slot) {
    const Vector3 fwd = ForwardFromYaw(leader.formYaw);
    const Vector2 off = kMateSlots[slot % 3];
    return {leader.pos.x - fwd.z * off.x - fwd.x * off.y, leader.pos.y, leader.pos.z + fwd.x * off.x - fwd.z * off.y};
}

// Strecke a-b (ein Schritt eines Geschosses) gegen die senkrechte Kapsel der
// Figur: waagerecht näher als TROOPER_RADIUS (plus 4 cm) und dort zwischen
// Füßen und Kopf.
bool SegmentHitsTrooper(Vector3 a, Vector3 b, const Trooper& t) {
    const Vector2 p{t.pos.x, t.pos.z}, a2{a.x, a.z}, b2{b.x, b.z};
    const Vector2 c = ClosestOnSeg(p, a2, b2);
    if (Vector2Distance(p, c) > cfg::TROOPER_RADIUS + 0.04f) return false;
    const float len = Vector2Distance(a2, b2);
    const float k = len > 1e-5f ? Vector2Distance(a2, c) / len : 0.0f;
    const float y = a.y + (b.y - a.y) * k;
    return y >= t.pos.y && y <= t.pos.y + cfg::TROOPER_HEIGHT;
}

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
// Rolle und Einsatz
// ===========================================================================
// Als SG-Team bleibt das Schiff versteckt stehen (hidden: nicht angreifbar,
// UpdatePlayer überspringt es). Abbau und Scanner enden dabei.
void World::SetRole(PlayerState& ps, PlayerRole role) {
    if (ps.role == role) return;
    Ship* sp = PlayerShip(ps);
    if (role == PlayerRole::Trooper) {
        Undock(ps);
        if (sp) {
            if (ps.mining) SoundAt(Sfx::MineAbort, sp->pos, 1.0f, 0.0f, ps.id);
            sp->mining = false;
            sp->vel = {0.0f, 0.0f, 0.0f};
            sp->hidden = true;
        }
        ps.mining = false;
        ps.scanOn = false;
    } else {
        EndSgMission(ps, false, false);
        if (sp) sp->hidden = false;
    }
    ps.role = role;
    ps.trooperId = -1;
    ps.trooperInput = TrooperInput{};
}

// Jede Tor-Deko im Spielfeld wird ein Stargate (Zufallskarte: cfg::MAP_GATES,
// eigene Karte: alle gesetzten Tore).
void World::BuildStargates() {
    if (!stargates.empty()) return;
    static const int gateA = ModelId("gate_simple"), gateB = ModelId("gate_complex");
    for (const Deco& d : decos) {
        if ((d.model != gateA && d.model != gateB) || !InField(d.pos)) continue;
        Stargate g;
        g.pos = d.pos;
        g.yaw = d.yaw;
        g.scale = d.scale;
        stargates.push_back(g);
    }
}

// Das Tor ist bei der Ankunft schon offen (angewählt wurde im Command
// Center). Der Spieler steht auf der obersten Stufe und schaut vom Tor weg;
// die Kameraden und Mitspieler folgen im Abstand GATE_MATE_INTERVAL
// (UpdateStargates).
bool World::StartSgMission(PlayerState& ps, int mates, int gate) {
    if (ps.role != PlayerRole::Trooper || ps.trooperId >= 0) return false;
    BuildStargates();
    const bool follower = gate >= 0;
    if (follower) {
        if (gate >= static_cast<int>(stargates.size())) return false;
    } else {
        // frei: ruhend oder gerade beim Schließen (das Command Center hält die
        // Welt an, ein eben benutztes Tor schließt dort nicht zu Ende)
        std::vector<int> free;
        for (size_t i = 0; i < stargates.size(); ++i)
            if (stargates[i].state == GateState::Idle || stargates[i].state == GateState::Closing)
                free.push_back(static_cast<int>(i));
        if (free.empty()) return false;
        gate = free[static_cast<size_t>(RandInt(0, static_cast<int>(free.size()) - 1))];
    }
    Stargate& g = stargates[static_cast<size_t>(gate)];
    if (!follower) {
        g.state = GateState::Open;
        g.t = 0.0f;
        g.toCommand = false;
        g.playerId = ps.id;
        g.team = ps.sgTeam;
        g.awayT = 0.0f;
        BuildTrooperBlocks();
    }
    const Vector3 exit = GateExit(g);
    const float yaw = YawTowards({0.0f, 0.0f, 0.0f}, GateNormal(g));
    Trooper t;
    t.id = nextTrooperId++;
    t.playerId = ps.id;
    t.team = ps.sgTeam;
    // Haut: der Erste des Teams 0, Mitspieler die übrigen Militär-Häute
    for (const Trooper& o : troopers)
        if (o.playerId >= 0 && o.team != 0 && o.team == t.team) t.skin = (t.skin + 1) % 4;
    t.pos = exit;
    t.home = exit;
    t.yaw = yaw;
    t.formYaw = yaw;
    troopers.push_back(t);
    ps.trooperId = t.id;
    ps.trooperInput = TrooperInput{};
    ps.trooperInput.yaw = yaw;
    ps.sgArrive = std::clamp(mates, 0, cfg::SG_MATES_MAX);
    ps.sgArriveT = cfg::GATE_MATE_INTERVAL;
    ps.sgArrivePos = exit;
    ps.sgArriveYaw = yaw;
    ps.sgPirates = 0;
    ps.sgTime = 0.0f;
    ps.sgEnded = 0;
    ps.sgGate = gate;
    if (follower) {
        SoundAt(Sfx::Discover, exit, 0.8f, 0.0f, ps.id);
        return true;
    }
    // Bodenpiraten (KI, Ai.cpp), weit genug vom Tor
    for (int i = 0; i < cfg::GROUND_PIRATE_GROUPS; ++i) SpawnPirateGroup(t.pos);
    pirateGroupT = cfg::GROUND_PIRATE_RESPAWN;
    SoundAt(Sfx::Discover, exit, 0.8f, 0.0f, ps.id);
    return true;
}

// Punkte: Proben nur bei der Rückkehr, Bodenpiraten immer. Das Scoreboard
// behält höchstens SG_LOG_MAX Einsätze; ist es voll, fällt der schwächste
// heraus (nie der gerade beendete).
void World::EndSgMission(PlayerState& ps, bool success, bool record) {
    const Trooper* t = FindTrooper(ps.trooperId);
    if (!t) return;
    if (record) {
        SgRecord r;
        r.number = ps.sgLog.empty() ? 1 : ps.sgLog.back().number + 1;
        r.success = success;
        r.samples = success ? t->samples : 0;
        r.pirates = ps.sgPirates;
        r.points = r.samples * cfg::SG_POINTS_SAMPLE + r.pirates * cfg::SG_POINTS_PIRATE + (success ? cfg::SG_POINTS_RETURN : 0);
        r.seconds = ps.sgTime;
        ps.sgLog.push_back(r);
        if (ps.sgLog.size() > static_cast<size_t>(cfg::SG_LOG_MAX)) {
            auto worst = std::min_element(ps.sgLog.begin(), ps.sgLog.end() - 1,
                                          [](const SgRecord& a, const SgRecord& b) { return a.points < b.points; });
            ps.sgLog.erase(worst);
        }
        ps.sgEnded = success ? 1 : 2;
    }
    // Mit der Figur gehen ihre Kameraden; ist danach kein Spieler mehr zu
    // Fuß, auch die Bodenpiraten.
    const int id = ps.trooperId;
    troopers.erase(std::remove_if(troopers.begin(), troopers.end(),
                                  [&](const Trooper& o) {
                                      return o.id == id || (o.playerId < 0 && o.kind == TrooperKind::Mate && o.leaderId == id);
                                  }),
                   troopers.end());
    if (std::none_of(troopers.begin(), troopers.end(), [](const Trooper& o) { return o.playerId >= 0; })) troopers.clear();
    // Tore dieses Spielers schließen. Mehrspieler: Ist noch ein Mitspieler
    // draußen, gehört das Tor jetzt ihm (das Team geht nacheinander durch).
    // Noch nicht nachgekommene Mitspieler folgen dann ihm.
    for (Stargate& g : stargates) {
        if (g.playerId != ps.id || g.state == GateState::Idle || g.state == GateState::Closing) continue;
        PlayerState* next = nullptr;
        for (PlayerState& o : players)
            if (o.id != ps.id && SameTeam(o, ps) && FindTrooper(o.trooperId)) next = &o;
        if (next) {
            g.playerId = next->id;
            if (!g.toCommand) {
                next->sgFollowers.insert(next->sgFollowers.end(), ps.sgFollowers.begin(), ps.sgFollowers.end());
                next->sgGate = ps.sgGate;
                ps.sgFollowers.clear();
            }
            continue;
        }
        g.state = GateState::Closing;
        g.t = 0.0f;
    }
    ps.trooperId = -1;
    ps.trooperInput = TrooperInput{};
    ps.sgArrive = 0;
    ps.sgFollowers.clear();
    ps.sgGate = -1;
}

int World::TeamSamples(const PlayerState& ps) const {
    int n = 0;
    for (const PlayerState& o : players)
        if (SameTeam(o, ps))
            if (const Trooper* t = FindTrooper(o.trooperId)) n += t->samples;
    return n;
}

// Je Spieler im Einsatz: Dauer, nachkommende Kameraden und Mitspieler und
// die Rückkehr. Hat das Team genug Proben (TeamSamples) und ist jemand von
// ihm nah an einem ruhenden Tor, wird es angewählt; ist es offen, führt die
// oberste Stufe hinein. Danach die Zustände aller Tore.
// Team: im Einzelspieler der Spieler mit seinen Kameraden, im Mehrspieler
// alle Spieler mit demselben sgTeam (und deren Kameraden).
void World::UpdateStargates(float dt) {
    // Spieler, zu dem eine Figur gehört (Kamerad: der Spieler seines Anführers)
    auto ownerOf = [&](const Trooper& m) -> const PlayerState* {
        if (m.playerId >= 0) return FindPlayer(m.playerId);
        if (m.kind != TrooperKind::Mate) return nullptr;
        const Trooper* l = FindTrooper(m.leaderId);
        return l ? FindPlayer(l->playerId) : nullptr;
    };
    // Steht eine Figur, für deren Spieler member() gilt, in der Nähe des Tors?
    auto nearGate = [&](const Stargate& g, auto member) {
        for (const Trooper& m : troopers) {
            if (m.down || Dist2D(g.pos, m.pos) >= cfg::GATE_RETURN_RANGE) continue;
            const PlayerState* o = ownerOf(m);
            if (o && member(*o)) return true;
        }
        return false;
    };
    auto memberOf = [](const Stargate& g, const PlayerState& o) { return o.id == g.playerId || (g.team != 0 && o.sgTeam == g.team); };

    for (PlayerState& ps : players) {
        const Trooper* t = ps.role == PlayerRole::Trooper ? FindTrooper(ps.trooperId) : nullptr;
        if (!t) continue;
        ps.sgTime += dt;
        ps.sgArriveT -= dt;
        if (ps.sgArrive > 0 && ps.sgArriveT <= 0.0f) {
            const Trooper leader = *t;   // Kopie: SpawnMate vergrößert troopers
            int slot = 0;
            for (const Trooper& m : troopers)
                if (m.playerId < 0 && m.kind == TrooperKind::Mate && m.leaderId == leader.id) ++slot;
            SpawnMate(leader, slot, ps.sgArrivePos, ps.sgArriveYaw);
            --ps.sgArrive;
            ps.sgArriveT = cfg::GATE_MATE_INTERVAL;
            t = FindTrooper(ps.trooperId);
        } else if (!ps.sgFollowers.empty() && ps.sgArriveT <= 0.0f) {
            // Mehrspieler: der nächste Mitspieler kommt durch dasselbe Tor
            const int next = ps.sgFollowers.front();
            ps.sgFollowers.erase(ps.sgFollowers.begin());
            if (PlayerState* f = FindPlayer(next)) StartSgMission(*f, 0, ps.sgGate);
            ps.sgArriveT = cfg::GATE_MATE_INTERVAL;
            t = FindTrooper(ps.trooperId);
        }
        if (t->down || TeamSamples(ps) < cfg::SAMPLE_GOAL) continue;
        bool busy = false;
        for (const Stargate& g : stargates)
            if (g.toCommand && g.state != GateState::Idle && memberOf(g, ps)) busy = true;
        if (busy) continue;
        for (Stargate& g : stargates) {
            if (g.state != GateState::Idle) continue;
            if (!nearGate(g, [&](const PlayerState& o) { return SameTeam(o, ps); })) continue;
            g.state = GateState::Dialing;
            g.t = 0.0f;
            g.toCommand = true;
            g.playerId = ps.id;
            g.team = ps.sgTeam;
            g.awayT = 0.0f;
            SoundAt(Sfx::ScanOn, g.pos, 1.0f, 0.0f, ps.id);
            for (PlayerState& o : players)
                if (SameTeam(o, ps) && o.trooperId >= 0) Notify(o, "Stargate wird angewählt ...", Color{150, 210, 255, 255});
            break;
        }
    }

    for (Stargate& g : stargates) {
        g.t += dt;
        PlayerState* ps = g.playerId >= 0 ? FindPlayer(g.playerId) : nullptr;
        // Ist noch jemand aus dem Team des Tors draußen (mit Figur)?
        bool out = false;
        for (const PlayerState& o : players)
            if (memberOf(g, o) && FindTrooper(o.trooperId)) out = true;
        auto close = [&]() {
            g.state = GateState::Closing;
            g.t = 0.0f;
        };
        switch (g.state) {
            case GateState::Idle: break;
            case GateState::Dialing:
                if (!out) close();
                else if (g.t >= cfg::GATE_DIAL) {
                    g.state = GateState::Open;
                    g.t = 0.0f;
                    SoundAt(Sfx::Discover, g.pos, 1.0f, 0.0f, g.playerId);
                    for (PlayerState& o : players)
                        if (memberOf(g, o) && o.trooperId >= 0)
                            Notify(o, "Stargate offen: die Treppe hinauf und hindurch", Color{150, 210, 255, 255});
                }
                break;
            case GateState::Open:
                if (!g.toCommand) {
                    // Ankunft: zu, wenn alle durch sind (der letzte ist eine
                    // Sekunde unterwegs) oder spätestens nach GATE_ARRIVE_MAX
                    if (!ps || !out || (ps->sgArrive == 0 && ps->sgFollowers.empty() && ps->sgArriveT <= -1.0f) ||
                        g.t >= cfg::GATE_ARRIVE_MAX)
                        close();
                    break;
                }
                if (!out) {
                    close();
                    break;
                }
                g.awayT = nearGate(g, [&](const PlayerState& o) { return memberOf(g, o); }) ? 0.0f : g.awayT + dt;
                // Wer vom Team auf der obersten Stufe steht, geht hindurch. Im
                // Einzelspieler gehen die Kameraden mit dem Spieler (die
                // Ansicht wechselt dann ohnehin ins Command Center); im
                // Mehrspieler bleibt das Tor offen, bis alle durch sind.
                {
                    std::vector<int> through;
                    for (const PlayerState& o : players) {
                        if (!memberOf(g, o)) continue;
                        const Trooper* m = FindTrooper(o.trooperId);
                        if (m && !m->down && InGatePassage(g, m->pos)) through.push_back(o.id);
                    }
                    for (int id : through)
                        if (PlayerState* o = FindPlayer(id)) {
                            SoundAt(Sfx::Dock, g.pos, 1.0f, 0.0f, id);
                            EndSgMission(*o, true);
                        }
                    if (through.empty() && g.awayT >= cfg::GATE_IDLE_CLOSE) close();
                }
                break;
            case GateState::Closing:
                if (g.t >= cfg::GATE_CLOSE) {
                    g.state = GateState::Idle;
                    g.t = 0.0f;
                    g.toCommand = false;
                    g.playerId = -1;
                    g.team = 0;
                }
                break;
        }
    }
}

// ===========================================================================
// Hindernisse
// ===========================================================================
// Jedes Deko- und Stationsteil wird zu einer Kapsel: die Grundfläche aus
// ModelBounds (gedreht und skaliert wie beim Zeichnen), entlang der längeren
// Seite gestreckt, Radius = halbe kürzere Seite, oben flach in Höhe der
// Oberkante. Ausgelassen werden flache Teile (unter 0,1 m), Teile über
// Kopfhöhe, Krater (man läuft über den Rand hinweg), Rover (fahren weg)
// und alles außerhalb des Spielfelds (Felswall, dort endet das Feld ohnehin).
// Stationsteile zählen nur, wenn sie bei der aktuellen Ausbaustufe stehen;
// zu Fuß kann man nichts abliefern, die Stufe ändert sich also nicht.
// Die Treppe vor jedem Stargate besteht aus einer Kapsel je Stufe (quer zur
// Treppe, Radius = halbe Stufentiefe); jede ist nur so viel höher als die
// vorige, dass die Figur ohne Sprung hinaufsteigt.
void World::BuildTrooperBlocks() {
    trooperBlocks.clear();
    auto add = [&](int model, Vector3 pos, float yaw, float scale, float yScale) {
        const BoundingBox b = ModelBounds(model);
        const float ys = yScale > 0.0f ? yScale : scale;
        const float bottom = pos.y + b.min.y * ys, top = pos.y + b.max.y * ys;
        if (top < 0.1f || bottom > cfg::TROOPER_HEIGHT) return;
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
    const float r = cfg::GATE_STAIR_DEPTH / (2.0f * kStairSteps), half = cfg::GATE_STAIR_WIDTH * 0.5f - r;
    for (const Stargate& g : stargates) {
        const float top = GateStairTop(g);
        if (top <= 0.0f) continue;
        const Vector3 n = GateNormal(g), side = GateRight(g);
        for (int i = 0; i < kStairSteps; ++i) {
            const float along = kGateDepth * g.scale + cfg::GATE_STAIR_DEPTH * (kStairSteps - 0.5f - i) / kStairSteps;
            const Vector3 c = Vector3Add(g.pos, Vector3Scale(n, along));
            const Vector3 a = Vector3Subtract(c, Vector3Scale(side, half)), b = Vector3Add(c, Vector3Scale(side, half));
            trooperBlocks.push_back({{a.x, a.z}, {b.x, b.z}, r, StairStepTop(top, i)});
        }
    }
}

bool World::TrooperBlocked(Vector3 p) const {
    const Vector2 q{p.x, p.z};
    for (const TrooperBlock& o : trooperBlocks)
        if (p.y < o.h && Vector2Distance(q, ClosestOnSeg(q, o.a, o.b)) < o.r) return true;
    return false;
}

bool World::TrooperLineClear(Vector3 a, Vector3 b) const {
    const Vector2 a2{a.x, a.z}, b2{b.x, b.z};
    for (const TrooperBlock& o : trooperBlocks) {
        float s, u;
        if (SegSegDist(a2, b2, o.a, o.b, s, u) < o.r && a.y + (b.y - a.y) * s < o.h) return false;
    }
    return true;
}

int World::NearestGate(Vector3 p) const {
    int best = -1;
    float bestD = 1e9f;
    for (size_t i = 0; i < stargates.size(); ++i) {
        const float d = Dist2D(p, stargates[i].pos);
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int World::SampleTarget(Vector3 p, float range) const {
    int id = -1;
    float best = range;
    for (const Deposit& d : deposits) {
        const float dist = Dist2D(p, d.pos);
        if (d.sample == Sample::Yellow && d.amount > 0 && dist <= best) {
            best = dist;
            id = d.id;
        }
    }
    return id;
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
    const float lim = fieldHalf - (instance ? 0.3f : 2.0f);
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
// Schritt aller Figuren: KI, Bewegung, Schild, Probe, Nachladen, Schießen
// ===========================================================================
// KI-Figuren berechnen erst ihre Eingabe (UpdateTrooperAi, Ai.cpp), danach
// läuft für alle dieselbe Physik.
// Probe: E gedrückt halten an einem gelben Kristall (bis SAMPLE_RANGE). Nach
// SAMPLE_TIME Sekunden ist er beprobt (wieder grün, abbaubar), die Figur
// trägt eine Probe mehr. Loslassen oder Weggehen bricht ab.
// Nachladen mit R oder automatisch beim Abdrücken mit leerem Magazin. Ohne
// Ersatzmunition gibt es nur den Ton "Abgelehnt". KI-Figuren haben
// unbegrenzt Ersatzmunition.
// Schild: lädt sich nach TROOPER_REGEN_DELAY Sekunden ohne Treffer wieder auf.
// Gefallene (DamageTrooper) nehmen keine Eingaben mehr an. Der Spieler steht
// nach TROOPER_RESPAWN Sekunden am Ankunftstor wieder auf, Kameraden nach
// MATE_RESPAWN neben ihrem Spieler; Bodenpiraten verschwinden nach 8 s.
// Steht beim Spieler dann kein Kamerad mehr (das ganze Team ist gefallen),
// ist der Einsatz gescheitert: zurück ins Command Center ohne Punkte für Proben.
void World::UpdateTroopers(float dt) {
    UpdateStargates(dt);
    UpdatePirateGroups(dt);
    for (auto& t : troopers) {
        if (t.playerId < 0 && !t.down) UpdateTrooperAi(t, dt);
        const PlayerState* ps = t.playerId >= 0 ? FindPlayer(t.playerId) : nullptr;
        TrooperInput in = t.playerId >= 0 ? (ps ? ps->trooperInput : TrooperInput{}) : t.input;
        if (t.down) {
            in = TrooperInput{};
            in.yaw = t.yaw;
            in.pitch = t.pitch;
        }
        UpdateTrooperPhysics(t, dt, in);
        t.age += dt;
        t.fireCd -= dt;
        t.hitFlash = std::max(0.0f, t.hitFlash - dt * 3.0f);
        const float speed = Vector2Length({t.vel.x, t.vel.z});
        t.walked += speed * dt;
        // Formation der Kameraden (MateSlot): dreht sich langsam in die
        // Laufrichtung, solange der Spieler läuft. Dreht er sich nur auf der
        // Stelle um, bleiben die Kameraden, wo sie sind.
        if (t.playerId >= 0 && speed > 1.0f) {
            const float travel = YawTowards({0.0f, 0.0f, 0.0f}, {t.vel.x, 0.0f, t.vel.z});
            const float turn = 1.5f * dt;
            t.formYaw += std::clamp(AngleDiff(t.formYaw, travel), -turn, turn);
        }
        if (t.down) {
            t.downT += dt;
            continue;
        }
        t.calmT += dt;
        if (t.calmT >= cfg::TROOPER_REGEN_DELAY) t.hp = std::min(t.maxHp, t.hp + cfg::TROOPER_REGEN * dt);
        if (t.playerId < 0) t.ammo = cfg::TROOPER_AMMO;
        const int target = in.sample ? SampleTarget(t.pos, cfg::SAMPLE_RANGE) : -1;
        if (target != t.sampleId) {
            SoundAt(target >= 0 ? Sfx::MineStart : Sfx::MineAbort, t.pos, 1.0f, 0.0f, t.playerId);
            t.sampleId = target;
            t.sampleT = 0.0f;
        }
        if (target >= 0) {
            t.sampleT += dt;
            Deposit* d = FindDeposit(target);
            if (d && t.sampleT >= cfg::SAMPLE_TIME) {
                d->sample = Sample::Taken;
                ++depositRevision;
                ++t.samples;
                t.sampleId = -1;
                t.sampleT = 0.0f;
                SoundAt(Sfx::MineDone, t.pos, 1.0f, 0.0f, t.playerId);
                // Meldung an das ganze Team (Mehrspieler: die Proben zählen zusammen)
                if (PlayerState* owner = FindPlayer(t.playerId)) {
                    const int have = TeamSamples(*owner);
                    for (PlayerState& o : players)
                        if (SameTeam(o, *owner) && o.trooperId >= 0)
                            Notify(o,
                                   have >= cfg::SAMPLE_GOAL ? Fmt("Probe genommen (%d / %d): zurück zu einem Stargate!", have, cfg::SAMPLE_GOAL)
                                                            : Fmt("Probe genommen (%d / %d)", have, cfg::SAMPLE_GOAL),
                                   Color{255, 225, 90, 255});
                }
            }
        }
        if (t.reloadT > 0.0f) {
            t.reloadT -= dt;
            if (t.reloadT <= 0.0f) {
                const int n = std::min(cfg::TROOPER_MAG - t.mag, t.ammo);
                t.mag += n;
                t.ammo -= n;
                t.reloadT = 0.0f;
                if (t.playerId >= 0) SoundAt(Sfx::UiConfirm, t.pos, 0.6f, 0.0f, t.playerId);
            }
        } else if ((in.reload || (in.fire && t.mag == 0)) && t.mag < cfg::TROOPER_MAG && t.ammo > 0) {
            t.reloadT = cfg::TROOPER_RELOAD;
            if (t.playerId >= 0) SoundAt(Sfx::UiMove, t.pos, 0.8f, 0.0f, t.playerId);
        }
        if (!in.fire || t.reloadT > 0.0f || t.fireCd > 0.0f) continue;
        if (t.mag > 0) {
            FireTrooperBullet(t);
            --t.mag;
            t.fireCd = t.playerId >= 0 ? cfg::TROOPER_FIRE_INTERVAL
                       : t.kind == TrooperKind::Pirate ? cfg::GROUND_PIRATE_FIRE_INTERVAL
                                                        : cfg::MATE_FIRE_INTERVAL;
        } else if (t.ammo == 0) {
            SoundAt(Sfx::Denied, t.pos, 0.6f, 0.0f, t.playerId);
            t.fireCd = 0.5f;
        }
    }

    auto revive = [&](Trooper& t, Vector3 pos) {
        t.down = false;
        t.downT = 0.0f;
        t.hp = t.maxHp;
        t.calmT = cfg::TROOPER_REGEN_DELAY;
        t.pos = pos;
        t.vel = {0.0f, 0.0f, 0.0f};
        t.mag = cfg::TROOPER_MAG;
        t.ammo = cfg::TROOPER_AMMO;
        t.reloadT = 0.0f;
        t.targetId = -1;
        t.seesTarget = false;
    };
    std::vector<int> failed;   // Spieler, deren ganzes Team gefallen ist
    for (auto& t : troopers) {
        if (!t.down) continue;
        if (t.playerId >= 0 && t.downT >= cfg::TROOPER_RESPAWN) {
            PlayerState* ps = FindPlayer(t.playerId);
            if (ps && ps->sgArrive == 0 && ps->sgFollowers.empty() && !TeamStanding(*this, t)) {
                failed.push_back(ps->id);
                continue;
            }
            revive(t, t.home);
            if (ps) Notify(*ps, "Zurück am Ankunftstor", Color{150, 210, 255, 255});
        } else if (t.playerId < 0 && t.kind == TrooperKind::Mate && t.downT >= cfg::MATE_RESPAWN) {
            const Trooper* leader = FindTrooper(t.leaderId);
            if (leader && !leader->down) revive(t, MateSlot(*leader, t.slot));
        }
    }
    troopers.erase(std::remove_if(troopers.begin(), troopers.end(),
                                  [](const Trooper& t) { return t.down && t.kind == TrooperKind::Pirate && t.playerId < 0 && t.downT > 8.0f; }),
                   troopers.end());
    for (int id : failed)
        if (PlayerState* ps = FindPlayer(id)) {
            Notify(*ps, "Das ganze Team ist gefallen. Einsatz gescheitert.", Color{255, 110, 90, 255});
            EndSgMission(*ps, false);
        }
}

// Das Geschoss startet etwas rechts unterhalb der Augen (an der Waffe) und
// fliegt parallel zur Blickrichtung: Beim Spieler trifft es knapp neben dem
// Fadenkreuz. KI-Figuren streuen (MATE_SPREAD, GROUND_PIRATE_SPREAD).
void World::FireTrooperBullet(Trooper& t) {
    Vector3 dir = TrooperAim(t);
    const bool pirate = t.playerId < 0 && t.kind == TrooperKind::Pirate;
    if (t.playerId < 0) {
        const float spread = pirate ? cfg::GROUND_PIRATE_SPREAD : cfg::MATE_SPREAD;
        const float yaw = t.yaw + Rand(-spread, spread), pitch = t.pitch + Rand(-spread, spread) * 0.6f;
        const Vector3 f = ForwardFromYaw(yaw);
        dir = {f.x * cosf(pitch), sinf(pitch), f.z * cosf(pitch)};
    }
    const Vector3 fwd = ForwardFromYaw(t.yaw);
    const Vector3 right{-fwd.z, 0.0f, fwd.x};
    Bullet b;
    b.pos = Vector3Add(Vector3Add(TrooperEye(t), Vector3Scale(dir, 0.2f)), Vector3Add(Vector3Scale(right, 0.04f), {0.0f, -0.033f, 0.0f}));
    b.vel = Vector3Scale(dir, cfg::TROOPER_BULLET_SPEED);
    b.life = cfg::TROOPER_BULLET_LIFE;
    b.damage = t.playerId >= 0 ? cfg::TROOPER_DAMAGE : pirate ? cfg::GROUND_PIRATE_DAMAGE : cfg::MATE_DAMAGE;
    b.faction = t.faction;
    b.ownerPlayer = t.playerId;
    b.ownerTrooper = t.id;
    b.ground = true;
    bullets.push_back(b);
    if (t.playerId >= 0) SoundAt(Sfx::PlayerShot, t.pos, 0.7f);
    else SoundAt(pirate ? Sfx::EnemyShot : Sfx::PlayerShot, t.pos, pirate ? 0.5f : 0.4f);
}

// ===========================================================================
// Treffer und Fallen
// ===========================================================================
// Getroffene KI-Figuren nehmen den Schützen ins Visier, auch wenn er weiter
// weg ist, als sie sehen. Fällt der Schild auf 0, liegt die Figur am Boden;
// der Spieler verliert dabei seine getragenen Proben.
void World::DamageTrooper(Trooper& t, const Bullet& b) {
    if (t.down) return;
    t.hp -= b.damage;
    t.hitFlash = 1.0f;
    t.calmT = 0.0f;
    if (t.playerId < 0 && b.ownerTrooper >= 0 && (t.targetId < 0 || !t.seesTarget)) {
        t.targetId = b.ownerTrooper;
        t.thinkT = 0.0f;
    }
    if (t.playerId >= 0) SoundAt(Sfx::ShieldHit, t.pos, 0.7f, 0.0f, t.playerId);
    if (t.hp > 0.0f) return;
    t.hp = 0.0f;
    t.down = true;
    t.downT = 0.0f;
    t.vel.x = t.vel.z = 0.0f;
    t.sampleId = -1;
    t.sampleT = 0.0f;
    t.reloadT = 0.0f;
    t.targetId = -1;
    SoundAt(Sfx::HitMetal, t.pos, 0.9f);
    // Wer bekommt die Meldung? Der Spieler der gefallenen Figur bzw. ihres
    // Anführers, bei einem Bodenpiraten der Spieler des Schützen (auch wenn
    // ein Kamerad geschossen hat).
    auto playerOf = [&](const Trooper* f) -> PlayerState* {
        if (!f) return nullptr;
        if (f->playerId >= 0) return FindPlayer(f->playerId);
        if (const Trooper* leader = FindTrooper(f->leaderId); leader && leader->playerId >= 0) return FindPlayer(leader->playerId);
        return nullptr;
    };
    const Color bad{255, 110, 90, 255};
    if (t.playerId >= 0) {
        if (PlayerState* ps = FindPlayer(t.playerId))
            Notify(*ps, t.samples > 0 ? Fmt("Gefallen! %d Proben verloren", t.samples) : std::string("Gefallen!"), bad);
        t.samples = 0;
    } else if (t.kind == TrooperKind::Mate) {
        if (PlayerState* ps = playerOf(&t)) Notify(*ps, "Ein Kamerad ist gefallen", bad);
    } else if (PlayerState* ps = playerOf(FindTrooper(b.ownerTrooper))) {
        Notify(*ps, t.zombie ? "Zombie ausgeschaltet" : "Bodenpirat ausgeschaltet", Color{150, 255, 170, 255});
        ++ps->sgPirates;
    }
}

// ===========================================================================
// KI-Figuren erzeugen
// ===========================================================================
// Kamerad: kommt am Ankunftstor heraus (pos, Blick yaw) und läuft dann zu
// seinem Platz in der Formation. Häute: die drei anderen Militär-Häute (der
// Spieler selbst wäre Haut 0).
void World::SpawnMate(const Trooper& leader, int slot, Vector3 pos, float yaw) {
    Trooper m;
    m.id = nextTrooperId++;
    m.faction = leader.faction;
    m.kind = TrooperKind::Mate;
    m.ai = TrooperAiState::Follow;
    m.leaderId = leader.id;
    m.slot = slot;
    m.skin = slot % 3 + 1;
    m.pos = pos;
    m.home = pos;
    m.yaw = yaw;
    m.input.yaw = yaw;
    troopers.push_back(m);
}

// Bodenpiraten: Gruppe von GROUND_PIRATE_MIN bis _MAX Figuren rund um einen
// zufälligen gelben Kristall, der mindestens GROUND_PIRATE_SPAWN_DIST vom
// Team entfernt ist und noch keine Gruppe hat. Gibt es keinen, ein
// zufälliger Punkt 60 bis 140 m vom Team. false = kein Platz gefunden.
// Einsatzkarte: an einem freien Gegner-Startplatz (enemySpots), sonst wie
// oben, nur mit kürzeren Abständen. Mit dem Thema Friedhof (World::zombies)
// ist die Gruppe aus ZOMBIE_MIN bis _MAX Zombies.
bool World::SpawnPirateGroup(Vector3 team) {
    const float spacing = instance ? 6.0f : 20.0f;   // Abstand zur nächsten Gruppe
    auto taken = [&](Vector3 p) {
        for (const Trooper& o : troopers)
            if (o.kind == TrooperKind::Pirate && o.playerId < 0 && Dist2D(o.home, p) < spacing) return true;
        return false;
    };
    // Einsatzkarte: zuerst die Gegner-Startplätze aus dem Editor, das kleine
    // Spielfeld erlaubt nur einen kürzeren Abstand zum Team.
    const float minDist = instance ? std::min(cfg::GROUND_PIRATE_SPAWN_DIST, fieldHalf * 0.5f) : cfg::GROUND_PIRATE_SPAWN_DIST;
    std::vector<Vector3> spots;
    for (const Vector3& e : enemySpots)
        if (Dist2D(e, team) >= minDist && !taken(e)) spots.push_back(e);
    if (spots.empty())
        for (const Deposit& d : deposits)
            if (d.sample == Sample::Yellow && d.amount > 0 && Dist2D(d.pos, team) >= minDist && !taken(d.pos))
                spots.push_back(d.pos);
    Vector3 c{};
    if (!spots.empty()) {
        c = spots[static_cast<size_t>(RandInt(0, static_cast<int>(spots.size()) - 1))];
    } else {
        bool found = false;
        for (int tries = 0; tries < 20 && !found; ++tries) {
            const float a = Rand(-PI, PI), r = Rand(minDist, instance ? 2.0f * fieldHalf : 140.0f);
            c = {team.x + cosf(a) * r, 0.0f, team.z + sinf(a) * r};
            const float lim = fieldHalf - (instance ? 2.0f : 10.0f);
            found = fabsf(c.x) < lim && fabsf(c.z) < lim && !taken(c) &&
                    !TrooperBlocked({c.x, 0.3f, c.z});
        }
        if (!found) return false;
    }
    c.y = 0.0f;
    const int n = zombies ? RandInt(cfg::ZOMBIE_MIN, cfg::ZOMBIE_MAX) : RandInt(cfg::GROUND_PIRATE_MIN, cfg::GROUND_PIRATE_MAX);
    const int group = nextPirateGroup++;
    for (int i = 0; i < n; ++i) {
        const float a = 2.0f * PI * i / n + Rand(-0.3f, 0.3f);
        Trooper p;
        p.id = nextTrooperId++;
        p.faction = Faction::Pirate;
        p.kind = TrooperKind::Pirate;
        p.ai = TrooperAiState::Patrol;
        p.group = group;
        p.skin = 4;
        p.hp = p.maxHp = cfg::GROUND_PIRATE_HP;
        if (zombies) {
            p.zombie = true;
            p.skin = RandInt(7, 9);   // zombieA bis C
            p.hp = p.maxHp = cfg::ZOMBIE_HP;
        }
        p.home = c;
        p.pos = {c.x + cosf(a) * 2.5f, 0.0f, c.z + sinf(a) * 2.5f};
        p.goal = p.pos;
        p.waitT = Rand(0.0f, 3.0f);
        p.yaw = Rand(-PI, PI);
        p.input.yaw = p.yaw;
        troopers.push_back(p);
    }
    return true;
}

// Fehlt eine Gruppe (alle ihre Figuren sind gefallen und verschwunden),
// kommt nach GROUND_PIRATE_RESPAWN Sekunden eine neue, weit genug vom Team
// (der ersten Spielerfigur).
void World::UpdatePirateGroups(float dt) {
    Vector3 team{};
    bool any = false;
    std::vector<int> groups;
    for (const Trooper& t : troopers) {
        if (t.playerId >= 0 && !any) {
            team = t.pos;
            any = true;
        }
        if (t.playerId < 0 && t.kind == TrooperKind::Pirate && std::find(groups.begin(), groups.end(), t.group) == groups.end())
            groups.push_back(t.group);
    }
    if (!any) return;
    if (static_cast<int>(groups.size()) >= cfg::GROUND_PIRATE_GROUPS) {
        pirateGroupT = cfg::GROUND_PIRATE_RESPAWN;
        return;
    }
    pirateGroupT -= dt;
    if (pirateGroupT <= 0.0f) {
        SpawnPirateGroup(team);
        pirateGroupT = cfg::GROUND_PIRATE_RESPAWN;
    }
}
