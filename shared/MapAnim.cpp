// ---------------------------------------------------------------------------
// MapAnim.cpp – Kartenanimation: Leben auf der Mondoberfläche (Teil der
// Klasse World, Deklarationen in World.hpp).
//
// Wofür ist die Datei da?
//   Alles hier ist reine Kulisse ohne Einfluss aufs Spiel (keine Kollision,
//   keine Punkte):
//     - Monorail zwischen den Stationen: Jede Station ist mit der
//       Handelsstation in der Mitte verbunden, der Raumschiffsverkäufer mit
//       der nächsten Station. Auf jeder Strecke pendelt ein Zug, der an jedem
//       Ende 10 Sekunden hält und dann zur anderen Station fährt.
//     - Raketenstartplätze, einer neben jeder Station: Alle 20 bis 30
//       Sekunden startet dort eine Rakete oder es landet eine.
//     - Tore (gate_simple, gate_complex): leuchten einmal pro Minute auf, als
//       öffnete sich ein Wurmloch (Render.cpp). In der Zufallskarte stehen
//       cfg::MAP_GATES Tore, in eigenen Karten alle gesetzten Tore.
//     - Mondfahrzeuge (rover): fahren von Ziel zu Ziel um Felsen, Krater,
//       Gebäude und Schienen herum und hinterlassen Reifenspuren. Wie lange
//       die Spuren zu sehen sind, stellt man in den Optionen ein (Seite
//       "Grafik", Render.cpp); gemerkt werden höchstens cfg::TRACK_TIME_MAX.
//     - Mehr Geröll und kleine Krater, gleichmäßig über die Karte verteilt:
//       Auch auf freier Fläche ist immer etwas zu sehen, an dem man merkt,
//       dass man sich bewegt.
//
// Wer ruft das auf?
//   - BuildStationExtras: am Ende von World::BuildStations (WorldGen.cpp).
//   - AddTerrainLife: am Ende von World::GenerateTerrain (WorldGen.cpp).
//   - SpawnRovers: am Ende von AddTerrainLife und World::ApplyMap, im
//     Mehrspieler-Client auch beim Wechsel zurück zur Standardkarte
//     (NetClient::HandleMapData).
//   - UpdateMapAnim: World::Update (nicht auf dem Server), UpdateAmbient und
//     UpdateCosmetics, also im Einzelspieler, im Hauptmenü und im
//     Mehrspieler-Client. Im Karteneditor stehen die Rover still.
//   - RailTrainPos, RocketAt: Render.cpp. Züge und Raketen ergeben sich nur
//     aus der Weltzeit (wie der Zug auf den Monorail-Kreisen der eigenen
//     Karte); es muss nichts gespeichert oder weitergerechnet werden.
//
// Mehrspieler: Nichts davon geht über das Netzwerk. Jeder Client baut die
// Kulisse selbst auf; Rover, Züge und Raketen stehen deshalb bei zwei
// Spielern nicht an derselben Stelle. Das ist bewusst so einfach gehalten,
// sie haben ja keinen Einfluss auf das Spiel.
//
// Zufall: eigener Zufallsgenerator animRng (Seed aus dem Welt-Seed). So
// bleibt die Zahlenfolge von rng unverändert: Lagerstätten, Piraten und
// Spielstände kommen genau wie vorher heraus (siehe Kopf von WorldGen.cpp),
// auch wenn der Server gar keine Rover kennt.
//
// Abhängigkeiten: World.hpp (Config.hpp, Util.hpp, raymath.h), Models.hpp
// (ModelId, ModelBounds), GameMap.hpp (mapgrid::CELL). Keine Grafik.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "Models.hpp"
#include <algorithm>
#include <cmath>

namespace {
// Fahrplan der Monorail zwischen den Stationen
constexpr float TRAIN_WAIT = 10.0f;        // Halt an jedem Ende: alle 10 s fährt der Zug zur anderen Station
constexpr float TRAIN_TOP_SPEED = 8.0f;    // Höchstgeschwindigkeit in m/s
// Raketen: Sekunden vom Zünden, bis sie außer Sicht ist (die Landung ist
// derselbe Ablauf rückwärts); davon die ersten ROCKET_IGNITE Sekunden noch
// auf dem Startplatz, während das Triebwerk hochläuft.
constexpr float ROCKET_FLIGHT = 9.0f;
constexpr float ROCKET_IGNITE = 1.5f;
constexpr float PAD_TOP = 0.18f;           // Oberkante der Startplattform (platform_large: 0,1 hoch, mal 1,8)

// Abstand vom Stationsmittelpunkt, an dem die Schiene endet bzw. der
// Startplatz steht, und wie weit Rover wegbleiben. Die Handelsstation ist
// ganz ausgebaut gut 13 m groß, Werft und Verkäufer kleiner (WorldGen.cpp).
float RailInset(StationType t) { return t == StationType::Trade ? 17.0f : t == StationType::Shipyard ? 12.0f : 10.0f; }
float PadDistance(StationType t) { return t == StationType::Trade ? 24.0f : t == StationType::Shipyard ? 19.0f : 16.0f; }
float StationBlock(StationType t) { return t == StationType::Trade ? 19.0f : 14.0f; }

// Pseudozufall 0..1 aus einer Zahl: feste Werte je Station bzw. Strecke, ohne
// einen Zufallsgenerator weiterzudrehen. Gleiche Eingabe -> gleiches Ergebnis.
float Hash01(float n) {
    const float x = sinf(n * 12.9898f) * 43758.5453f;
    return x - floorf(x);
}

// Weicher Verlauf von 0 nach 1 (Anfahren und Bremsen): Steigung am Anfang und
// am Ende 0, in der Mitte 1,5.
float Smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Abstand des Punkts p von der Strecke a-b (auf dem Boden)
float SegDist(Vector2 p, Vector2 a, Vector2 b) {
    const Vector2 ab = Vector2Subtract(b, a);
    const float len2 = Vector2LengthSqr(ab);
    const float t = len2 > 0.0f ? std::clamp(Vector2DotProduct(Vector2Subtract(p, a), ab) / len2, 0.0f, 1.0f) : 0.0f;
    return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, t)));
}

// Grundfläche eines Deko-Teils als Kreis: größte Ausdehnung vom Ursprung
float DecoRadius(const Deco& d) {
    const BoundingBox b = ModelBounds(d.model);
    const float r = std::max(std::max(fabsf(b.min.x), fabsf(b.max.x)), std::max(fabsf(b.min.z), fabsf(b.max.z)));
    return r * d.scale;
}

bool InField(Vector3 p) { return fabsf(p.x) <= cfg::WORLD_HALF && fabsf(p.z) <= cfg::WORLD_HALF; }
} // namespace

// ===========================================================================
// Monorail und Raketenstartplätze an den Stationen
//
// Nur aus den Stationen berechnet (keine Zufallszahlen): Client, Server und
// Editor kommen auf dieselben Strecken. Strecken gehen von Rand zu Rand der
// Stationen (RailInset); die Schienen liegen wie im Editor flach am Boden.
// Der Startplatz einer Station steht in der größten Lücke zwischen ihren
// Strecken, also nie auf einer Schiene.
// ===========================================================================
void World::BuildStationExtras() {
    rails.clear();
    pads.clear();
    for (size_t i = 1; i < stations.size(); ++i) {
        // Partner: die Handelsstation in der Mitte; der Verkäufer (südlich
        // außerhalb des Rings) nimmt die nächste Station, sonst läge seine
        // Strecke im Mehrspieler fast auf der von Ring-Platz 3.
        size_t partner = 0;
        if (stations[i].type == StationType::Dealer) {
            float best = 1e9f;
            for (size_t j = 0; j < stations.size(); ++j) {
                const float d = Dist2D(stations[i].pos, stations[j].pos);
                if (j != i && d < best) {
                    best = d;
                    partner = j;
                }
            }
        }
        const Station& from = stations[partner];
        const Station& to = stations[i];
        Vector3 dir = Vector3Subtract(to.pos, from.pos);
        dir.y = 0.0f;
        const float len = Vector3Length(dir);
        if (len < 1.0f) continue;
        dir = Vector3Scale(dir, 1.0f / len);
        StationRail r;
        r.a = Vector3Add(from.pos, Vector3Scale(dir, RailInset(from.type)));
        r.b = Vector3Subtract(to.pos, Vector3Scale(dir, RailInset(to.type)));
        r.from = static_cast<int>(partner);
        r.to = static_cast<int>(i);
        r.cargo = static_cast<int>(i % 3);
        r.phase = Hash01(static_cast<float>(i) + 0.5f) * 40.0f;
        if (Dist2D(r.a, r.b) > mapgrid::CELL) rails.push_back(r);
    }

    for (size_t i = 0; i < stations.size(); ++i) {
        const Station& st = stations[i];
        // Richtungen (Winkel auf dem Boden) aller Strecken dieser Station
        std::vector<float> ang;
        for (const StationRail& r : rails) {
            if (r.from == static_cast<int>(i)) ang.push_back(atan2f(r.b.z - r.a.z, r.b.x - r.a.x));
            if (r.to == static_cast<int>(i)) ang.push_back(atan2f(r.a.z - r.b.z, r.a.x - r.b.x));
        }
        // Mitte der größten Lücke zwischen zwei Strecken (bei einer Strecke:
        // genau gegenüber). Ohne Strecke: von der Kartenmitte weg.
        float dirAng = atan2f(st.pos.z, st.pos.x);
        if (!ang.empty()) {
            std::sort(ang.begin(), ang.end());
            float bestGap = -1.0f;
            for (size_t k = 0; k < ang.size(); ++k) {
                const float a1 = k + 1 < ang.size() ? ang[k + 1] : ang[0] + 2.0f * PI;
                if (a1 - ang[k] > bestGap) {
                    bestGap = a1 - ang[k];
                    dirAng = ang[k] + bestGap * 0.5f;
                }
            }
        }
        RocketPad p;
        p.out = {cosf(dirAng), 0.0f, sinf(dirAng)};
        p.pos = Vector3Add(st.pos, Vector3Scale(p.out, PadDistance(st.type)));
        p.typeB = i % 2 == 1;
        const float h = static_cast<float>(i);
        p.stand = 20.0f + 10.0f * Hash01(h * 3.1f + 1.0f);
        p.away = 20.0f + 10.0f * Hash01(h * 5.7f + 2.0f);
        p.phase = Hash01(h * 1.3f + 3.0f) * 60.0f;
        pads.push_back(p);
    }
}

bool World::NearRail(Vector3 p, float margin) const {
    const Vector2 q{p.x, p.z};
    for (const StationRail& r : rails)
        if (SegDist(q, {r.a.x, r.a.z}, {r.b.x, r.b.z}) < margin) return true;
    // Startplatz: Plattform 3,6 m und Gerüst daneben, rund 4 m um die Mitte
    for (const RocketPad& pd : pads)
        if (Vector2Distance(q, {pd.pos.x, pd.pos.z}) < margin + 4.0f) return true;
    return false;
}

// Mitte des Zugs: an jedem Ende TRAIN_WAIT Sekunden Halt, dazwischen mit
// Anfahren und Bremsen (Smooth) zum anderen Ende. Der Zug ist drei Wagen lang
// (Wagenmitten im Abstand TRAIN_CAR_GAP, ein Wagen ist ein Rasterfeld lang).
float RailTrainPos(const StationRail& r, float t) {
    const float half = mapgrid::TRAIN_CAR_GAP + 0.55f * mapgrid::CELL;   // halbe Zuglänge (Spitze des Triebwagens)
    const float travel = Dist2D(r.a, r.b) - 2.0f * half;
    if (travel < 2.0f) return -1.0f;
    // Smooth erreicht in der Mitte das 1,5-fache der Durchschnittsgeschwindigkeit
    const float drive = std::max(4.0f, 1.5f * travel / TRAIN_TOP_SPEED);
    const float period = 2.0f * (TRAIN_WAIT + drive);
    float u = fmodf(t + r.phase, period);
    if (u < 0.0f) u += period;
    float s;
    if (u < TRAIN_WAIT) s = 0.0f;
    else if (u < TRAIN_WAIT + drive) s = travel * Smooth((u - TRAIN_WAIT) / drive);
    else if (u < 2.0f * TRAIN_WAIT + drive) s = travel;
    else s = travel * (1.0f - Smooth((u - 2.0f * TRAIN_WAIT - drive) / drive));
    return half + s;
}

// Ablauf an einem Startplatz (Länge stand + Flug + away + Flug):
//   steht -> startet (ROCKET_FLIGHT s) -> unterwegs (away s) -> landet -> steht ...
// Die Landung ist der Start rückwärts abgespielt. Beim Aufstieg steigt die
// Rakete beschleunigt (h = 2,2 * t^2) und kippt dabei immer mehr nach "out"
// (seitlicher Weg = h^2 / 260, die Neigung folgt der Bahn). In den letzten
// 2 Sekunden wird sie kleiner, als verschwände sie in der Ferne.
RocketPose RocketAt(const RocketPad& p, float t) {
    RocketPose r;
    const float period = p.stand + ROCKET_FLIGHT + p.away + ROCKET_FLIGHT;
    float u = fmodf(t + p.phase, period);
    if (u < 0.0f) u += period;
    float f;   // Sekunden seit dem Zünden
    if (u < p.stand) f = 0.0f;
    else if (u < p.stand + ROCKET_FLIGHT) f = u - p.stand;
    else if (u < p.stand + ROCKET_FLIGHT + p.away) return r;   // unterwegs
    else f = period - u;                                       // Landung: rückwärts
    r.visible = true;
    r.pos = {p.pos.x, PAD_TOP, p.pos.z};
    if (u < p.stand) return r;
    if (f < ROCKET_IGNITE) {
        r.thrust = f / ROCKET_IGNITE;
        return r;
    }
    const float tau = f - ROCKET_IGNITE;
    const float h = 2.2f * tau * tau;
    const float side = h * h / 260.0f;
    r.pos = Vector3Add(r.pos, {p.out.x * side, h, p.out.z * side});
    r.tilt = atanf(2.0f * h / 260.0f);
    r.size = std::clamp((ROCKET_FLIGHT - f) / 2.0f, 0.0f, 1.0f);
    r.thrust = 1.0f;
    return r;
}

// ===========================================================================
// Mehr Mondoberfläche, Tore und Rover (Zufallskarte)
//
// Läuft am Ende von GenerateTerrain, also nachdem rng alle Zahlen für die
// Deko gezogen hat. Erst werden Schienen und Startplätze frei geräumt (ein
// Fels auf der Schiene sähe falsch aus), dann kommt mit animRng dazu:
//   - Kleinzeug in einem Raster aus 15-m-Feldern: in jedem Feld genau ein
//     Teil an zufälliger Stelle (Geröll, Fels oder kleiner Krater). Anders
//     als rein zufällig verteilte Teile lässt das keine großen leeren Flächen.
//   - cfg::MAP_GATES Tore, weit auseinander und mit freiem Platz drumherum.
//     Sie schauen ungefähr nach Süden, zur Kamera.
//   - cfg::MAP_ROVERS Rover (als Deko; SpawnRovers lässt sie fahren).
// ===========================================================================
void World::AddTerrainLife() {
    animRng.seed(seed ^ 0x2545F491u);
    auto rnd = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(animRng); };
    auto add = [&](const char* name, Vector3 pos, float yaw, float scale) {
        decos.push_back({ModelId(name), pos, yaw, scale, 0.0f});
    };
    auto farFromStations = [&](Vector3 p, float d) {
        for (const auto& st : stations)
            if (Dist2D(p, st.pos) < d) return false;
        return true;
    };

    decos.erase(std::remove_if(decos.begin(), decos.end(),
                               [&](const Deco& d) { return InField(d.pos) && NearRail(d.pos, 2.0f + DecoRadius(d)); }),
                decos.end());

    const float lim = cfg::WORLD_HALF - 4.0f;
    const float cell = 15.0f;
    const char* small[] = {"rocks_smallA", "rocks_smallB", "rock", "meteor_half"};
    for (float gx = -lim; gx < lim; gx += cell) {
        for (float gz = -lim; gz < lim; gz += cell) {
            const Vector3 p{gx + rnd(0.0f, cell), 0.0f, gz + rnd(0.0f, cell)};
            const float kind = rnd(0.0f, 1.0f), yaw = rnd(0.0f, 2.0f * PI), size = rnd(0.0f, 1.0f);
            if (fabsf(p.x) > lim || fabsf(p.z) > lim) continue;
            if (!farFromStations(p, 22.0f) || NearRail(p, 3.0f)) continue;
            if (kind < 0.25f) add(size < 0.5f ? "crater" : "craterLarge", p, yaw, 3.0f + 4.0f * size);
            else if (kind < 0.75f) add(small[kind < 0.5f ? 0 : 1], p, yaw, 1.6f + 1.8f * size);
            else add(small[kind < 0.9f ? 2 : 3], p, yaw, 1.0f + 1.4f * size);
        }
    }

    std::vector<Vector3> gates;
    for (int tries = 0; tries < 300 && static_cast<int>(gates.size()) < cfg::MAP_GATES; ++tries) {
        const Vector3 p{rnd(-250.0f, 250.0f), 0.0f, rnd(-250.0f, 250.0f)};
        const float yaw = rnd(-0.7f, 0.7f);
        if (!farFromStations(p, 45.0f) || NearRail(p, 12.0f)) continue;
        bool apart = true;
        for (const Vector3& g : gates)
            if (Dist2D(g, p) < 90.0f) apart = false;
        if (!apart) continue;
        decos.erase(std::remove_if(decos.begin(), decos.end(),
                                   [&](const Deco& d) { return InField(d.pos) && Dist2D(d.pos, p) < 5.0f + DecoRadius(d); }),
                    decos.end());
        add(gates.size() % 2 == 0 ? "gate_complex" : "gate_simple", p, yaw, 6.0f);
        gates.push_back(p);
    }

    for (int n = 0, tries = 0; n < cfg::MAP_ROVERS && tries < 400; ++tries) {
        const Vector3 p{rnd(-lim + 10.0f, lim - 10.0f), 0.0f, rnd(-lim + 10.0f, lim - 10.0f)};
        const float yaw = rnd(0.0f, 2.0f * PI);
        if (!farFromStations(p, 40.0f) || NearRail(p, 6.0f)) continue;
        add("rover", p, yaw, 3.0f);
        ++n;
    }
    SpawnRovers();
}

// ===========================================================================
// Rover
// ===========================================================================
// Jede Deko "rover" im Spielfeld wird zu einem fahrenden Rover (die Deko
// bleibt in decos stehen, Render.cpp zeichnet sie aber nicht). Hindernisse
// sind Stationen, Schienen, Startplätze und alle Deko-Teile, die höher als
// 0,5 m sind; flaches Geröll und Straßen überfährt der Rover.
void World::SpawnRovers() {
    rovers.clear();
    roverBlocks.clear();
    if (isServer) return;
    auto rnd = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(animRng); };
    const int roverModel = ModelId("rover");
    for (const Station& st : stations) roverBlocks.push_back({{st.pos.x, st.pos.z}, {st.pos.x, st.pos.z}, StationBlock(st.type)});
    for (const StationRail& r : rails) roverBlocks.push_back({{r.a.x, r.a.z}, {r.b.x, r.b.z}, 2.0f});
    for (const RocketPad& p : pads) roverBlocks.push_back({{p.pos.x, p.pos.z}, {p.pos.x, p.pos.z}, 4.5f});
    for (const Deco& d : decos) {
        if (!InField(d.pos)) continue;
        if (d.model == roverModel) {
            Rover r;
            r.pos = {d.pos.x, 0.0f, d.pos.z};
            r.yaw = d.yaw;
            r.scale = d.scale;
            r.target = r.pos;
            r.wait = rnd(0.2f, 3.0f);   // erst nach der Pause ein Ziel suchen (UpdateMapAnim)
            r.topSpeed = rnd(2.4f, 3.8f);
            rovers.push_back(r);
            continue;
        }
        const BoundingBox b = ModelBounds(d.model);
        if (b.max.y * (d.yScale > 0.0f ? d.yScale : d.scale) < 0.5f) continue;
        roverBlocks.push_back({{d.pos.x, d.pos.z}, {d.pos.x, d.pos.z}, DecoRadius(d) * 0.85f});
    }
}

// Ist die gerade Strecke a-b frei? Geprüft wird alle 1,5 m mit 1,2 m
// Sicherheitsabstand (der Rover fährt Kurven, nicht genau die Gerade).
// Hindernisse weit weg werden vorher mit einer einzigen Abstandsrechnung
// übersprungen. Steht der Rover schon in einem Hindernis (z. B. in einem
// selbst gesetzten Teil der eigenen Karte), darf er hinausfahren.
bool World::RoverPathClear(Vector2 a, Vector2 b) const {
    const float len = Vector2Distance(a, b);
    const Vector2 mid = Vector2Scale(Vector2Add(a, b), 0.5f);
    const int steps = std::max(1, static_cast<int>(len / 1.5f));
    for (const RoverBlock& o : roverBlocks) {
        const float margin = o.r + 1.2f;
        if (SegDist(mid, o.a, o.b) > len * 0.5f + margin) continue;
        if (SegDist(a, o.a, o.b) < o.r) continue;
        for (int i = 1; i <= steps; ++i) {
            const Vector2 p = Vector2Lerp(a, b, static_cast<float>(i) / steps);
            if (SegDist(p, o.a, o.b) < margin) return false;
        }
    }
    return true;
}

// Neues Ziel 12..45 m entfernt, meist ungefähr geradeaus (so entstehen
// natürliche Bögen), nach acht Fehlversuchen in jede Richtung. Ohne freien
// Weg: kurz stehen bleiben und dann neu suchen.
void World::PickRoverTarget(Rover& r) {
    auto rnd = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(animRng); };
    r.legT = 0.0f;
    const float lim = cfg::WORLD_HALF - 8.0f;
    const Vector2 from{r.pos.x, r.pos.z};
    for (int tries = 0; tries < 14; ++tries) {
        const float spread = tries < 8 ? 1.3f : PI;
        const Vector3 f = ForwardFromYaw(r.yaw + rnd(-spread, spread));
        const float d = rnd(12.0f, 45.0f);
        const Vector2 to{from.x + f.x * d, from.y + f.z * d};
        if (fabsf(to.x) > lim || fabsf(to.y) > lim) continue;
        if (!RoverPathClear(from, to)) continue;
        r.target = {to.x, 0.0f, to.y};
        return;
    }
    r.target = r.pos;
    r.wait = 1.5f;
}

// Fahren wie ein einfaches Auto: zum Ziel lenken (höchstens 1,3 rad/s), in
// engen Kurven und kurz vor dem Ziel langsamer, sanft beschleunigen und
// bremsen. Am Ziel bleibt der Rover manchmal ein paar Sekunden stehen.
// Alle 0,6 m Fahrt kommt ein Punkt an die Reifenspur.
void World::UpdateMapAnim(float dt) {
    if (dt <= 0.0f) return;
    auto rnd = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(animRng); };
    for (Rover& r : rovers) {
        while (!r.track.empty() && time - r.track.front().t > cfg::TRACK_TIME_MAX) r.track.pop_front();
        r.legT += dt;
        if (r.wait > 0.0f) {
            r.wait -= dt;
            if (r.wait <= 0.0f) PickRoverTarget(r);
            continue;
        }
        const float dist = Dist2D(r.pos, r.target);
        // Angekommen, oder seit 40 s unterwegs (festgefahren, kreist ums Ziel)
        if (dist < 2.0f || r.legT > 40.0f) {
            r.speed = 0.0f;
            if (rnd(0.0f, 1.0f) < 0.35f) r.wait = rnd(2.0f, 6.0f);
            else PickRoverTarget(r);
            continue;
        }
        const float diff = AngleDiff(r.yaw, YawTowards(r.pos, r.target));
        r.yaw += std::clamp(diff, -1.3f * dt, 1.3f * dt);
        float want = r.topSpeed * std::clamp(1.0f - fabsf(diff) / 1.4f, 0.2f, 1.0f);
        want = std::min(want, std::max(0.8f, dist * 0.6f));
        r.speed += std::clamp(want - r.speed, -3.0f * dt, 1.5f * dt);
        const Vector3 f = ForwardFromYaw(r.yaw);
        r.pos.x += f.x * r.speed * dt;
        r.pos.z += f.z * r.speed * dt;
        if (r.track.empty() || hypotf(r.pos.x - r.track.back().x, r.pos.z - r.track.back().z) >= 0.6f)
            r.track.push_back({r.pos.x, r.pos.z, -f.z, f.x, time});
    }
}
