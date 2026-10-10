// Erzeugung der Mondoberfläche, der Stationen und der Lagerstätten.
#include "World.hpp"
#include "Assets.hpp"
#include <cmath>

namespace {
constexpr float STATION_SCALE = 1.8f;
}

void World::ResolveModels() {
    mdl.player = g_assets.Id("craft_speederA");
    mdl.pirateA = g_assets.Id("craft_speederB");
    mdl.pirateB = g_assets.Id("craft_speederC");
    mdl.elite = g_assets.Id("craft_speederD");
    mdl.miner = g_assets.Id("craft_miner");
    mdl.cargoA = g_assets.Id("craft_cargoA");
    mdl.cargoB = g_assets.Id("craft_cargoB");
    mdl.terrain = g_assets.Id("terrain");
    mdl.crystalSmall = g_assets.Id("rock_crystals");
    mdl.crystalA = g_assets.Id("rock_crystalsLargeA");
    mdl.crystalB = g_assets.Id("rock_crystalsLargeB");
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
        if (ok && !ships.empty() && minDistPlayer > 0.0f) {
            if (Vector2Distance({p.x, p.z}, {Player().pos.x, Player().pos.z}) < minDistPlayer) ok = false;
        }
        if (ok) return p;
    }
    return p;
}

void World::GenerateTerrain() {
    decos.clear();
    auto add = [&](const char* name, Vector3 pos, float yaw, float scale) {
        decos.push_back({g_assets.Id(name), pos, yaw, scale, 0.0f});
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

void World::BuildStations() {
    // -------------------------------------------------------------------
    // Handelsstation – wächst in 8 Stufen (0..7)
    // Koordinaten im Raster des Kits (1 Einheit = ein Kit-Feld)
    // -------------------------------------------------------------------
    Station& t = stations[0];
    t = Station{};
    t.name = "Handelsstation";
    t.pos = {0, 0, 0};
    const float H = PI * 0.5f;
    auto part = [&](Station& st, const char* name, float x, float z, float yaw, int stage, float y = 0.0f,
                    float scale = 1.0f) {
        st.parts.push_back({g_assets.Id(name), {x, y, z}, yaw, scale, stage});
    };

    // Stufe 0: Kuppel auf Plattform, kleine Antenne, ein Geschützturm
    part(t, "platform_large", -1, -1, 0, 0);
    part(t, "platform_large", 1, -1, 0, 0);
    part(t, "platform_large", -1, 1, 0, 0);
    part(t, "platform_large", 1, 1, 0, 0);
    part(t, "hangar_roundA", 0, 0, 0, 0, 0.1f);
    part(t, "satelliteDish", 1.5f, 1.5f, -0.6f, 0, 0.1f);
    part(t, "barrels", -1.5f, 1.5f, 0.3f, 0, 0.1f);
    t.turrets.push_back({{-1.6f, 0.1f, -1.6f}, 0, 0, 0, true});

    // Stufe 1: Ostflügel mit kleinem Hangar
    part(t, "corridor", 2.5f, 0, H, 1);
    part(t, "corridor_window", 3.5f, 0, H, 1);
    part(t, "platform_large", 5.5f, 0, 0, 1);
    part(t, "hangar_smallA", 5.5f, 0, H, 1, 0.1f);
    part(t, "machine_generator", 3.5f, 1.4f, 0, 1);
    part(t, "machine_barrel", 2.6f, -1.4f, 0, 1);

    // Stufe 2: Westflügel mit großer Kuppel, zweiter Geschützturm
    part(t, "corridor", -2.5f, 0, H, 2);
    part(t, "corridor_window", -3.5f, 0, H, 2);
    part(t, "platform_large", -6.2f, -1, 0, 2);
    part(t, "platform_large", -6.2f, 1, 0, 2);
    part(t, "hangar_roundB", -6.1f, 0, 0, 2, 0.1f);
    part(t, "satelliteDish_large", -3.6f, 1.6f, 2.4f, 2);
    t.turrets.push_back({{-4.0f, 0.0f, -1.8f}, 0, 0, 2, true});

    // Stufe 3: Nordflügel mit großem Hangar und Lager
    part(t, "corridor", 0, -2.5f, 0, 3);
    part(t, "corridor_window", 0, -3.5f, 0, 3);
    part(t, "platform_large", 0, -4.5f, 0, 3);
    part(t, "platform_large", 0, -6.5f, 0, 3);
    part(t, "hangar_largeA", 0, -5.5f, 0, 3, 0.1f);
    part(t, "machine_barrelLarge", 1.8f, -3.2f, 0, 3);
    part(t, "machine_barrelLarge", -1.6f, -3.4f, 0, 3);
    part(t, "barrels", 1.8f, -4.6f, 0.4f, 3);

    // Stufe 4: Südflügel mit Glaskuppel, Generator, dritter Geschützturm
    part(t, "corridor", 0, 2.5f, 0, 4);
    part(t, "corridor_window", 0, 3.5f, 0, 4);
    part(t, "platform_large", -1, 5.4f, 0, 4);
    part(t, "platform_large", 1, 5.4f, 0, 4);
    part(t, "hangar_roundGlass", 0, 5.4f, 0, 4, 0.1f);
    part(t, "machine_generatorLarge", 2.6f, 3.2f, 0, 4);
    part(t, "satelliteDish_detailed", -2.4f, 3.0f, 0.8f, 4);
    t.turrets.push_back({{3.0f, 0.0f, -1.8f}, 0, 0, 4, true});

    // Stufe 5: Raketenstartplatz Nord-Ost
    part(t, "platform_large", 5.5f, -4.0f, 0, 5);
    part(t, "rocket_baseA", 5.5f, -4.0f, 0, 5, 0.1f);
    part(t, "rocket_fuelA", 5.5f, -4.0f, 0, 5, 1.1f);
    part(t, "rocket_sidesA", 5.5f, -4.0f, 0, 5, 1.6f);
    part(t, "rocket_topA", 5.5f, -4.0f, 0, 5, 2.6f);
    part(t, "structure_detailed", 4.0f, -4.0f, 0, 5);
    part(t, "structure", 4.0f, -4.0f, 0, 5, 1.0f);
    part(t, "structure", 4.0f, -4.0f, 0, 5, 2.0f);

    // Stufe 6: Werkhallen Nord-West, Schornsteine, vierter Turm
    part(t, "platform_large", -5.5f, -4.5f, 0, 6);
    part(t, "hangar_largeB", -5.5f, -4.5f, H, 6, 0.1f);
    part(t, "chimney_detailed", -7.2f, -3.0f, 0, 6);
    part(t, "chimney", -7.2f, -6.0f, 0, 6);
    part(t, "machine_wireless", -3.6f, -4.6f, H, 6);
    t.turrets.push_back({{-4.0f, 0.0f, 3.2f}, 0, 0, 6, false});

    // Stufe 7: zweite Rakete und großer Kuppelkomplex
    part(t, "platform_large", 5.5f, 4.5f, 0, 7);
    part(t, "rocket_baseB", 5.5f, 4.5f, 0, 7, 0.1f);
    part(t, "rocket_fuelB", 5.5f, 4.5f, 0, 7, 1.1f);
    part(t, "rocket_sidesB", 5.5f, 4.5f, 0, 7, 2.1f);
    part(t, "rocket_topB", 5.5f, 4.5f, 0, 7, 3.1f);
    part(t, "platform_large", -5.8f, 4.6f, 0, 7);
    part(t, "hangar_roundB", -5.8f, 4.6f, 0, 7, 0.1f);
    part(t, "satelliteDish_large", 3.5f, 6.2f, -0.6f, 7);
    t.turrets.push_back({{3.4f, 0.0f, 3.0f}, 0, 0, 7, false});

    // -------------------------------------------------------------------
    // Werft (Upgrades)
    // -------------------------------------------------------------------
    Station& y = stations[1];
    y = Station{};
    y.name = "Werft";
    y.pos = {72.0f, 0.0f, -58.0f};
    for (int ix = -1; ix <= 1; ix += 2)
        for (int iz = -1; iz <= 1; iz += 2) part(y, "platform_large", ix * 1.0f, iz * 1.0f, 0, 0);
    part(y, "platform_large", 3.0f, -1.0f, 0, 0);
    part(y, "platform_large", 3.0f, 1.0f, 0, 0);
    part(y, "hangar_largeA", -1.0f, 0.0f, 0, 0, 0.1f);
    part(y, "structure_detailed", 2.2f, -1.4f, 0, 0, 0.1f);
    part(y, "structure_detailed", 3.8f, -1.4f, 0, 0, 0.1f);
    part(y, "structure_detailed", 2.2f, 1.4f, 0, 0, 0.1f);
    part(y, "structure_detailed", 3.8f, 1.4f, 0, 0, 0.1f);
    part(y, "craft_cargoB", 3.0f, 0.0f, H, 0, 0.35f, 0.9f);
    part(y, "gate_complex", 1.2f, 2.3f, 0, 0, 0.0f);
    part(y, "machine_wireless", -2.6f, 1.6f, H, 0);
    part(y, "desk_computerScreen", 1.0f, -2.2f, PI, 0);
    part(y, "satelliteDish_large", -2.6f, -1.8f, 0.6f, 0);
    part(y, "barrels", 4.6f, 2.4f, 0.0f, 0);
    y.turrets.push_back({{-2.8f, 0.0f, 0.0f}, 0, 0, 0, true});
    y.turrets.push_back({{5.0f, 0.0f, -2.4f}, 0, 0, 0, false});

    // Skalieren und Hindernisse (hohe Teile) bestimmen
    for (auto& st : stations) {
        for (auto& p : st.parts) {
            p.offset = Vector3Scale(p.offset, STATION_SCALE);
            p.scale *= STATION_SCALE;
            BoundingBox b = g_assets.Bounds(p.model);
            float height = b.max.y * p.scale + p.offset.y;
            if (height > cfg::SHIP_ALTITUDE - 0.6f) {
                float r = fmaxf(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f * p.scale;
                st.obstacles.push_back({p.offset.x, r * 0.9f + 1.0f, p.offset.z});
            }
        }
        for (auto& tu : st.turrets) tu.offset = Vector3Scale(tu.offset, STATION_SCALE);
    }
}

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
}
