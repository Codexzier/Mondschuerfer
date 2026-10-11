// Erzeugung der Mondoberfläche, der Stationen und der Lagerstätten.
#include "World.hpp"
#include "Models.hpp"
#include <cmath>

namespace {
constexpr float STATION_SCALE = 1.8f;
}

void World::ResolveModels() {
    mdl.player = ModelId("craft_speederA");
    mdl.pirateA = ModelId("craft_speederB");
    mdl.pirateB = ModelId("craft_speederC");
    mdl.elite = ModelId("craft_speederD");
    mdl.miner = ModelId("craft_miner");
    mdl.cargoA = ModelId("craft_cargoA");
    mdl.cargoB = ModelId("craft_cargoB");
    mdl.terrain = ModelId("terrain");
    mdl.crystalSmall = ModelId("rock_crystals");
    mdl.crystalA = ModelId("rock_crystalsLargeA");
    mdl.crystalB = ModelId("rock_crystalsLargeB");
}

Vector3 World::RandomFreePos(float minDistStations, float minDistPlayer) {
    const float lim = cfg::WORLD_HALF - 10.0f;
    Vector3 p{};
    for (int tries = 0; tries < 200; ++tries) {
        p = {Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        bool ok = true;
        for (const auto& st : stations) {
            if (Vector2Distance({p.x, p.z}, {st.pos.x, st.pos.z}) < minDistStations) ok = false;
        }
        if (ok && minDistPlayer > 0.0f) {
            // Nicht zu nah an einem Spielerschiff (Einzelspieler: ships[0])
            for (const auto& s : ships) {
                if (s.kind != ShipKind::Player) continue;
                if (Vector2Distance({p.x, p.z}, {s.pos.x, s.pos.z}) < minDistPlayer) ok = false;
            }
        }
        if (ok) return p;
    }
    return p;
}

void World::GenerateTerrain() {
    decos.clear();
    auto add = [&](const char* name, Vector3 pos, float yaw, float scale) {
        decos.push_back({ModelId(name), pos, yaw, scale, 0.0f});
    };
    auto farFromStations = [&](Vector3 p, float d) {
        for (const auto& st : stations)
            if (Vector2Distance({p.x, p.z}, {st.pos.x, st.pos.z}) < d) return false;
        return true;
    };
    const float lim = cfg::WORLD_HALF - 4.0f;

    // Krater
    for (int i = 0; i < 110; ++i) {
        Vector3 p{Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        if (!farFromStations(p, 28.0f)) continue;
        add(Rand(0, 1) < 0.5f ? "crater" : "craterLarge", p, Rand(0, 2 * PI), Rand(5.0f, 15.0f));
    }
    // Gestein und Geröll
    const char* rocks[] = {"rock", "rock_largeA", "rock_largeB", "rocks_smallA", "rocks_smallB", "meteor_half"};
    for (int i = 0; i < 320; ++i) {
        Vector3 p{Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        if (!farFromStations(p, 22.0f)) continue;
        add(rocks[RandInt(0, 5)], p, Rand(0, 2 * PI), Rand(1.6f, 4.2f));
    }
    // Große Brocken (Meteoriten)
    for (int i = 0; i < 24; ++i) {
        Vector3 p{Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        if (!farFromStations(p, 30.0f)) continue;
        add(Rand(0, 1) < 0.5f ? "meteor" : "meteor_detailed", p, Rand(0, 2 * PI), Rand(2.2f, 3.6f));
    }
    // Kleinigkeiten zum Entdecken
    for (int i = 0; i < 5; ++i) {
        Vector3 p{Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        if (farFromStations(p, 40.0f)) add("bones", p, Rand(0, 2 * PI), 3.0f);
    }
    {
        Vector3 p = RandomFreePos(70.0f, 0.0f);
        add("alien", p, Rand(0, 2 * PI), 1.6f);
        add("rover", Vector3Add(p, {3.0f, 0, 1.0f}), Rand(0, 2 * PI), 3.0f);
    }
    for (int i = 0; i < 3; ++i) {
        Vector3 p = RandomFreePos(35.0f, 0.0f);
        add("rover", p, Rand(0, 2 * PI), 3.0f);
        add("barrels", Vector3Add(p, {2.5f, 0, -1.5f}), Rand(0, 2 * PI), 2.5f);
    }

    // Grenze des Einsatzgebiets: Felswall
    const float edge = cfg::WORLD_HALF + 3.0f;
    for (float t = -edge; t <= edge; t += 7.0f) {
        const char* r = Rand(0, 1) < 0.5f ? "rock_largeA" : "rock_largeB";
        add(r, {t + Rand(-2, 2), 0, -edge + Rand(-2, 2)}, Rand(0, 2 * PI), Rand(6.0f, 9.0f));
        add(r, {t + Rand(-2, 2), 0, edge + Rand(-2, 2)}, Rand(0, 2 * PI), Rand(6.0f, 9.0f));
        add(r, {-edge + Rand(-2, 2), 0, t + Rand(-2, 2)}, Rand(0, 2 * PI), Rand(6.0f, 9.0f));
        add(r, {edge + Rand(-2, 2), 0, t + Rand(-2, 2)}, Rand(0, 2 * PI), Rand(6.0f, 9.0f));
    }
}

// ---------------------------------------------------------------------------
// Stationen
//
// Es gibt 9 feste Bauplätze: die Kartenmitte und 8 Plätze auf einem Ring
// (Abstand ~92 zur Mitte). Die erste Handelsstation steht immer in der Mitte,
// die erste Werft immer bei (72, -58) – genau wie im Einzelspieler. Weitere
// Stationen (Mehrspieler-Einstellungen) wechseln sich auf dem Ring ab und
// werden möglichst weit voneinander verteilt.
//
// Die Reihenfolge in "stations" ist die Bauplatz-Reihenfolge. Sie ist im
// Client und im Server gleich, weil beide dieselbe Funktion mit denselben
// Einstellungen aufrufen.
// ---------------------------------------------------------------------------
namespace {

// Ring-Bauplätze in der Reihenfolge, in der sie belegt werden (möglichst weit verteilt)
constexpr int kRingOrder[8] = {0, 4, 2, 6, 1, 5, 3, 7};

Vector3 RingSlot(int k) {
    // Platz 0 ist (72, -58); die anderen entstehen durch Drehen um je 45 Grad
    const float a = k * (PI / 4.0f);
    const float c = cosf(a), s = sinf(a);
    return {72.0f * c - (-58.0f) * s, 0.0f, 72.0f * s + (-58.0f) * c};
}

} // namespace

namespace {
void BuildTradeStation(Station& t);
void BuildShipyard(Station& y);
} // namespace

void World::BuildStations() {
    stations.clear();
    const int trades = settings.tradeStations;
    const int yards = settings.shipyards;

    // Handelsstation 1 in der Mitte
    Station first;
    first.type = StationType::Trade;
    first.pos = {0, 0, 0};
    BuildTradeStation(first);
    stations.push_back(first);

    // Ring: abwechselnd Werft, Handelsstation, Werft, ... bis alle verteilt sind
    int tradeLeft = trades - 1, yardLeft = yards;
    bool nextIsYard = true;
    for (int slot = 0; slot < 8 && (tradeLeft > 0 || yardLeft > 0); ++slot) {
        bool yard = nextIsYard ? yardLeft > 0 : tradeLeft <= 0;
        Station st;
        st.pos = RingSlot(kRingOrder[slot]);
        if (yard) {
            st.type = StationType::Shipyard;
            BuildShipyard(st);
            --yardLeft;
        } else {
            st.type = StationType::Trade;
            BuildTradeStation(st);
            --tradeLeft;
        }
        stations.push_back(st);
        nextIsYard = !nextIsYard;
    }

    // Namen mit Nummer, wenn es mehrere gibt
    int tn = 0, yn = 0;
    for (auto& st : stations) {
        if (st.type == StationType::Trade) st.name = trades > 1 ? Fmt("Handelsstation %d", ++tn) : "Handelsstation";
        else st.name = yards > 1 ? Fmt("Werft %d", ++yn) : "Werft";
    }

    // Skalieren und Hindernisse (hohe Teile) bestimmen
    for (auto& st : stations) {
        for (auto& p : st.parts) {
            p.offset = Vector3Scale(p.offset, STATION_SCALE);
            p.scale *= STATION_SCALE;
            BoundingBox b = ModelBounds(p.model);
            float height = b.max.y * p.scale + p.offset.y;
            if (height > cfg::SHIP_ALTITUDE - 0.6f) {
                float r = fmaxf(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f * p.scale;
                st.obstacles.push_back({p.offset.x, r * 0.9f + 1.0f, p.offset.z});
            }
        }
        for (auto& tu : st.turrets) tu.offset = Vector3Scale(tu.offset, STATION_SCALE);
    }
}

namespace {
const float H = PI * 0.5f;

void Part(Station& st, const char* name, float x, float z, float yaw, int stage, float y = 0.0f, float scale = 1.0f) {
    st.parts.push_back({ModelId(name), {x, y, z}, yaw, scale, stage});
}

void BuildTradeStation(Station& t) {
    // -------------------------------------------------------------------
    // Handelsstation – wächst in 8 Stufen (0..7)
    // Koordinaten im Raster des Kits (1 Einheit = ein Kit-Feld)
    // -------------------------------------------------------------------
    // Stufe 0: Kuppel auf Plattform, kleine Antenne, ein Geschützturm
    Part(t, "platform_large", -1, -1, 0, 0);
    Part(t, "platform_large", 1, -1, 0, 0);
    Part(t, "platform_large", -1, 1, 0, 0);
    Part(t, "platform_large", 1, 1, 0, 0);
    Part(t, "hangar_roundA", 0, 0, 0, 0, 0.1f);
    Part(t, "satelliteDish", 1.5f, 1.5f, -0.6f, 0, 0.1f);
    Part(t, "barrels", -1.5f, 1.5f, 0.3f, 0, 0.1f);
    t.turrets.push_back({{-1.6f, 0.1f, -1.6f}, 0, 0, 0, true});

    // Stufe 1: Ostflügel mit kleinem Hangar
    Part(t, "corridor", 2.5f, 0, H, 1);
    Part(t, "corridor_window", 3.5f, 0, H, 1);
    Part(t, "platform_large", 5.5f, 0, 0, 1);
    Part(t, "hangar_smallA", 5.5f, 0, H, 1, 0.1f);
    Part(t, "machine_generator", 3.5f, 1.4f, 0, 1);
    Part(t, "machine_barrel", 2.6f, -1.4f, 0, 1);

    // Stufe 2: Westflügel mit großer Kuppel, zweiter Geschützturm
    Part(t, "corridor", -2.5f, 0, H, 2);
    Part(t, "corridor_window", -3.5f, 0, H, 2);
    Part(t, "platform_large", -6.2f, -1, 0, 2);
    Part(t, "platform_large", -6.2f, 1, 0, 2);
    Part(t, "hangar_roundB", -6.1f, 0, 0, 2, 0.1f);
    Part(t, "satelliteDish_large", -3.6f, 1.6f, 2.4f, 2);
    t.turrets.push_back({{-4.0f, 0.0f, -1.8f}, 0, 0, 2, true});

    // Stufe 3: Nordflügel mit großem Hangar und Lager
    Part(t, "corridor", 0, -2.5f, 0, 3);
    Part(t, "corridor_window", 0, -3.5f, 0, 3);
    Part(t, "platform_large", 0, -4.5f, 0, 3);
    Part(t, "platform_large", 0, -6.5f, 0, 3);
    Part(t, "hangar_largeA", 0, -5.5f, 0, 3, 0.1f);
    Part(t, "machine_barrelLarge", 1.8f, -3.2f, 0, 3);
    Part(t, "machine_barrelLarge", -1.6f, -3.4f, 0, 3);
    Part(t, "barrels", 1.8f, -4.6f, 0.4f, 3);

    // Stufe 4: Südflügel mit Glaskuppel, Generator, dritter Geschützturm
    Part(t, "corridor", 0, 2.5f, 0, 4);
    Part(t, "corridor_window", 0, 3.5f, 0, 4);
    Part(t, "platform_large", -1, 5.4f, 0, 4);
    Part(t, "platform_large", 1, 5.4f, 0, 4);
    Part(t, "hangar_roundGlass", 0, 5.4f, 0, 4, 0.1f);
    Part(t, "machine_generatorLarge", 2.6f, 3.2f, 0, 4);
    Part(t, "satelliteDish_detailed", -2.4f, 3.0f, 0.8f, 4);
    t.turrets.push_back({{3.0f, 0.0f, -1.8f}, 0, 0, 4, true});

    // Stufe 5: Raketenstartplatz Nord-Ost
    Part(t, "platform_large", 5.5f, -4.0f, 0, 5);
    Part(t, "rocket_baseA", 5.5f, -4.0f, 0, 5, 0.1f);
    Part(t, "rocket_fuelA", 5.5f, -4.0f, 0, 5, 1.1f);
    Part(t, "rocket_sidesA", 5.5f, -4.0f, 0, 5, 1.6f);
    Part(t, "rocket_topA", 5.5f, -4.0f, 0, 5, 2.6f);
    Part(t, "structure_detailed", 4.0f, -4.0f, 0, 5);
    Part(t, "structure", 4.0f, -4.0f, 0, 5, 1.0f);
    Part(t, "structure", 4.0f, -4.0f, 0, 5, 2.0f);

    // Stufe 6: Werkhallen Nord-West, Schornsteine, vierter Turm
    Part(t, "platform_large", -5.5f, -4.5f, 0, 6);
    Part(t, "hangar_largeB", -5.5f, -4.5f, H, 6, 0.1f);
    Part(t, "chimney_detailed", -7.2f, -3.0f, 0, 6);
    Part(t, "chimney", -7.2f, -6.0f, 0, 6);
    Part(t, "machine_wireless", -3.6f, -4.6f, H, 6);
    t.turrets.push_back({{-4.0f, 0.0f, 3.2f}, 0, 0, 6, false});

    // Stufe 7: zweite Rakete und großer Kuppelkomplex
    Part(t, "platform_large", 5.5f, 4.5f, 0, 7);
    Part(t, "rocket_baseB", 5.5f, 4.5f, 0, 7, 0.1f);
    Part(t, "rocket_fuelB", 5.5f, 4.5f, 0, 7, 1.1f);
    Part(t, "rocket_sidesB", 5.5f, 4.5f, 0, 7, 2.1f);
    Part(t, "rocket_topB", 5.5f, 4.5f, 0, 7, 3.1f);
    Part(t, "platform_large", -5.8f, 4.6f, 0, 7);
    Part(t, "hangar_roundB", -5.8f, 4.6f, 0, 7, 0.1f);
    Part(t, "satelliteDish_large", 3.5f, 6.2f, -0.6f, 7);
    t.turrets.push_back({{3.4f, 0.0f, 3.0f}, 0, 0, 7, false});
}

void BuildShipyard(Station& y) {
    // -------------------------------------------------------------------
    // Werft (Upgrades)
    // -------------------------------------------------------------------
    for (int ix = -1; ix <= 1; ix += 2)
        for (int iz = -1; iz <= 1; iz += 2) Part(y, "platform_large", ix * 1.0f, iz * 1.0f, 0, 0);
    Part(y, "platform_large", 3.0f, -1.0f, 0, 0);
    Part(y, "platform_large", 3.0f, 1.0f, 0, 0);
    Part(y, "hangar_largeA", -1.0f, 0.0f, 0, 0, 0.1f);
    Part(y, "structure_detailed", 2.2f, -1.4f, 0, 0, 0.1f);
    Part(y, "structure_detailed", 3.8f, -1.4f, 0, 0, 0.1f);
    Part(y, "structure_detailed", 2.2f, 1.4f, 0, 0, 0.1f);
    Part(y, "structure_detailed", 3.8f, 1.4f, 0, 0, 0.1f);
    Part(y, "craft_cargoB", 3.0f, 0.0f, H, 0, 0.35f, 0.9f);
    Part(y, "gate_complex", 1.2f, 2.3f, 0, 0, 0.0f);
    Part(y, "machine_wireless", -2.6f, 1.6f, H, 0);
    Part(y, "desk_computerScreen", 1.0f, -2.2f, PI, 0);
    Part(y, "satelliteDish_large", -2.6f, -1.8f, 0.6f, 0);
    Part(y, "barrels", 4.6f, 2.4f, 0.0f, 0);
    y.turrets.push_back({{-2.8f, 0.0f, 0.0f}, 0, 0, 0, true});
    y.turrets.push_back({{5.0f, 0.0f, -2.4f}, 0, 0, 0, false});
}

} // namespace

void World::SpawnDeposit(bool initial) {
    Deposit d;
    d.id = nextId++;
    // Nicht direkt neben den Stationen
    d.pos = RandomFreePos(30.0f, initial ? 0.0f : 25.0f);
    float dist = Vector2Length({d.pos.x, d.pos.z});
    float richness = 1.0f + dist / 85.0f;            // weiter draußen = ergiebiger
    d.amount = static_cast<int>(Rand(450.0f, 1100.0f) * richness);
    d.amount = (d.amount / 10) * 10;
    d.initial = d.amount;
    d.yaw = Rand(0, 2 * PI);
    if (d.amount > 1600) {
        d.model = Rand(0, 1) < 0.5f ? mdl.crystalA : mdl.crystalB;
        d.scale = 3.4f;
    } else if (d.amount > 1000) {
        d.model = Rand(0, 1) < 0.5f ? mdl.crystalA : mdl.crystalB;
        d.scale = 2.8f;
    } else {
        d.model = mdl.crystalSmall;
        d.scale = 2.8f;
    }
    deposits.push_back(d);
    ++depositRevision;
}
