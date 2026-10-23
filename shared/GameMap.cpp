// ---------------------------------------------------------------------------
// GameMap.cpp – Umsetzung von GameMap.hpp: Segmentliste, Dateiformat und
// Monorail-Kreise (Erklärung und Aufrufer: GameMap.hpp).
// ---------------------------------------------------------------------------
#include "GameMap.hpp"
#include "Config.hpp"
#include "Models.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <utility>

const char* SegmentGroupName(SegmentGroup g) {
    switch (g) {
        case SegmentGroup::Terrain: return "Gelände";
        case SegmentGroup::Rocks: return "Felsen";
        case SegmentGroup::Roads: return "Straßen";
        case SegmentGroup::Monorail: return "Monorail";
        case SegmentGroup::Buildings: return "Gebäude";
        case SegmentGroup::Misc: return "Sonstiges";
        default: return "";
    }
}

// Die Größen: Gelände, Straßen und Schienen füllen genau ein Rasterfeld
// (Kit-Feld x CELL). Krater sind wie in der Zufallswelt groß und flach,
// Gebäude haben die Größe der Stationsteile (1,8, siehe WorldGen.cpp).
// "Zufall generieren" erzeugt Krater, Felsen und die Fundstücke aus
// "Sonstiges" (World::GenerateTerrain) – auch die müssen hier stehen.
const std::vector<MapSegment>& MapSegments() {
    using G = SegmentGroup;
    const float C = mapgrid::CELL;
    static const std::vector<MapSegment> list = {
        {"crater", "Krater", G::Terrain, 10.0f},
        {"craterLarge", "Großer Krater", G::Terrain, 14.0f},
        {"terrain_ramp", "Rampe", G::Terrain, C},
        {"terrain_rampLarge", "Lange Rampe", G::Terrain, C},
        {"terrain_rampLarge_detailed", "Lange Rampe (Detail)", G::Terrain, C},
        {"terrain_sideCliff", "Klippe", G::Terrain, C},
        {"terrain_sideCorner", "Klippe außen", G::Terrain, C},
        {"terrain_sideCornerInner", "Klippe innen", G::Terrain, C},
        {"terrain_sideEnd", "Klippe Ende", G::Terrain, C},

        {"rock", "Fels", G::Rocks, C},
        {"rock_largeA", "Großer Fels A", G::Rocks, 4.0f},
        {"rock_largeB", "Großer Fels B", G::Rocks, 4.0f},
        {"rocks_smallA", "Geröll A", G::Rocks, C},
        {"rocks_smallB", "Geröll B", G::Rocks, C},
        {"meteor", "Meteorit", G::Rocks, C},
        {"meteor_detailed", "Meteorit (Detail)", G::Rocks, C},
        {"meteor_half", "Halber Meteorit", G::Rocks, C},

        {"terrain_roadStraight", "Gerade", G::Roads, C},
        {"terrain_roadCorner", "Kurve", G::Roads, C},
        {"terrain_roadSplit", "Abzweig", G::Roads, C},
        {"terrain_roadCross", "Kreuzung", G::Roads, C},
        {"terrain_roadEnd", "Ende", G::Roads, C},

        {"monorail_trackStraight", "Gerade", G::Monorail, C},
        {"monorail_trackCornerSmall", "Kurve klein", G::Monorail, C},
        {"monorail_trackCornerLarge", "Kurve groß", G::Monorail, C},

        {"hangar_smallA", "Kleiner Hangar", G::Buildings, 1.8f},
        {"hangar_largeA", "Großer Hangar", G::Buildings, 1.8f},
        {"hangar_roundA", "Kuppel", G::Buildings, 1.8f},
        {"hangar_roundGlass", "Glaskuppel", G::Buildings, 1.8f},
        {"platform_large", "Plattform", G::Buildings, 1.8f},
        {"corridor", "Gang", G::Buildings, 1.8f},
        {"structure_detailed", "Gerüst", G::Buildings, 1.8f},
        {"satelliteDish_large", "Große Antenne", G::Buildings, 1.8f},
        {"machine_generatorLarge", "Generator", G::Buildings, 1.8f},
        {"machine_wireless", "Funkmast", G::Buildings, 1.8f},
        {"chimney", "Schornstein", G::Buildings, 1.8f},
        {"gate_simple", "Tor", G::Buildings, 1.8f},
        {"gate_complex", "Tor (Detail)", G::Buildings, 1.8f},

        {"rover", "Mondfahrzeug", G::Misc, 3.0f},
        {"alien", "Alien", G::Misc, 1.6f},
        {"astronautA", "Astronaut", G::Misc, 1.6f},
        {"bones", "Knochen", G::Misc, 3.0f},
        {"barrels", "Fässer", G::Misc, 2.5f},
    };
    return list;
}

const MapSegment* FindSegment(const std::string& model) {
    for (const auto& s : MapSegments())
        if (model == s.model) return &s;
    return nullptr;
}

bool IsTrack(const std::string& model) {
    const MapSegment* s = FindSegment(model);
    return s && s->group == SegmentGroup::Monorail;
}

// ===========================================================================
// Dateiformat
// ===========================================================================
namespace {
const char* kMagic = "MONDSCHUERFER_KARTE";
const int kVersion = 1;
}

std::string MapToText(const GameMap& m) {
    std::ostringstream f;
    f << kMagic << ' ' << kVersion << '\n';
    f << "name " << m.name << '\n';
    if (!m.author.empty()) f << "author " << m.author << '\n';
    for (const MapPiece& p : m.pieces)
        f << "piece " << p.model << ' ' << p.x << ' ' << p.z << ' ' << p.yaw << ' ' << p.scale << '\n';
    f << "end\n";
    return f.str();
}

// Jede Zeile einzeln prüfen: Eine kaputte Zeile kostet nur dieses eine Teil.
// Die Prüfungen schützen auch vor Dateien, die nicht aus dem Editor stammen
// (später: hochgeladene Karten auf dem Server).
bool MapFromText(const std::string& text, GameMap& out) {
    std::istringstream f(text);
    std::string magic;
    int version = 0;
    f >> magic >> version;
    if (magic != kMagic || version != kVersion) return false;
    GameMap m;
    std::string line;
    std::getline(f, line);   // Rest der ersten Zeile
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // Datei mit Windows-Zeilenenden
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "name") {
            std::getline(ls, m.name);
            if (!m.name.empty() && m.name[0] == ' ') m.name.erase(0, 1);
            if (m.name.size() > static_cast<size_t>(mapgrid::MAX_NAME) * 4) m.name.resize(mapgrid::MAX_NAME * 4);
        } else if (key == "author") {
            ls >> m.author;
            if (m.author.size() > 16) m.author.resize(16);   // net::MAX_NAME
        } else if (key == "piece") {
            if (static_cast<int>(m.pieces.size()) >= mapgrid::MAX_PIECES) continue;
            MapPiece p;
            if (!(ls >> p.model >> p.x >> p.z >> p.yaw >> p.scale)) continue;
            if (!FindSegment(p.model)) continue;
            if (!std::isfinite(p.x) || !std::isfinite(p.z) || !std::isfinite(p.yaw) || !std::isfinite(p.scale)) continue;
            if (fabsf(p.x) > cfg::WORLD_HALF || fabsf(p.z) > cfg::WORLD_HALF) continue;
            if (p.scale < 0.2f || p.scale > 20.0f) continue;
            m.pieces.push_back(p);
        }
    }
    out = std::move(m);
    return true;
}

std::string MapFileStem(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size() && out.size() < 48; ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        // Umlaute und ß (UTF-8: zwei Bytes, erstes 0xC3) umschreiben
        if (c == 0xC3 && i + 1 < name.size()) {
            unsigned char d = static_cast<unsigned char>(name[i + 1]);
            const char* r = nullptr;
            switch (d) {
                case 0xA4: r = "ae"; break;   // ä
                case 0xB6: r = "oe"; break;   // ö
                case 0xBC: r = "ue"; break;   // ü
                case 0x84: r = "Ae"; break;   // Ä
                case 0x96: r = "Oe"; break;   // Ö
                case 0x9C: r = "Ue"; break;   // Ü
                case 0x9F: r = "ss"; break;   // ß
                default: break;
            }
            if (r) { out += r; ++i; continue; }
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out += static_cast<char>(c);
        else if (c < 0x80 || (c & 0xC0) == 0xC0) out += '_';   // Folgebytes eines UTF-8-Zeichens nicht noch einmal
    }
    if (out.empty()) out = "karte";
    return out;
}

float MapPieceY(const MapPiece& p) {
    BoundingBox b = ModelBounds(ModelId(p.model));
    float y = std::max(0.0f, -b.min.y * p.scale);
    if (b.max.y - b.min.y < 0.01f) y += 0.03f;
    return y;
}

// Gleiche Formel wie raymaths MatrixRotateY (so dreht auch TRS beim Zeichnen):
//   x' = x * cos + z * sin,   z' = -x * sin + z * cos
Vector2 MapRotate(float x, float z, float yaw) {
    const float c = cosf(yaw), s = sinf(yaw);
    return {x * c + z * s, -x * s + z * c};
}

// ===========================================================================
// Monorail-Kreise
//
// Die drei Schienenteile im Kit-Raster (Drehung 0, Ursprung = Modellursprung),
// abgelesen aus den Abmessungen in ModelBounds.inc (Schiene 0,2 breit):
//   Gerade:      von (0, -0,5) nach (0, 0,5)
//   Kurve klein: Viertelkreis um (-0,5, -0,5) mit Radius 0,5,
//                von (0, -0,5) nach (-0,5, 0) – verbindet zwei Seitenmitten
//                eines Rasterfelds.
//   Kurve groß:  Viertelkreis um (-1, -1) mit Radius 1,5,
//                von (0,5, -1) nach (-1, 0,5) – belegt 2 x 2 Felder, der
//                Ursprung liegt auf einer Rasterecke.
// ===========================================================================
namespace {

struct TrackShape {
    const char* model;
    Vector2 a, b;      // Enden (x, z)
    Vector2 center;    // Kreismitte (nur Kurven)
    float radius;      // 0 = Gerade
};
const TrackShape kTracks[] = {
    {"monorail_trackStraight", {0.0f, -0.5f}, {0.0f, 0.5f}, {0, 0}, 0.0f},
    {"monorail_trackCornerSmall", {0.0f, -0.5f}, {-0.5f, 0.0f}, {-0.5f, -0.5f}, 0.5f},
    {"monorail_trackCornerLarge", {0.5f, -1.0f}, {-1.0f, 0.5f}, {-1.0f, -1.0f}, 1.5f},
};
constexpr float kTrackTop = 0.13f;   // Höhe der Schiene im Kit

const TrackShape* Shape(const std::string& model) {
    for (const auto& t : kTracks)
        if (model == t.model) return &t;
    return nullptr;
}

// Punkte der Mittellinie von Ende a nach Ende b, schon in Weltkoordinaten.
std::vector<Vector3> TrackSamples(const MapPiece& p, const TrackShape& t) {
    std::vector<Vector2> local;
    if (t.radius <= 0.0f) {
        local = {t.a, t.b};
    } else {
        // Winkel von a und b um die Kreismitte, dazwischen gleichmäßig verteilt
        const float a0 = atan2f(t.a.y - t.center.y, t.a.x - t.center.x);
        const float a1 = atan2f(t.b.y - t.center.y, t.b.x - t.center.x);
        const int n = t.radius > 1.0f ? 12 : 6;
        for (int i = 0; i <= n; ++i) {
            float ang = a0 + (a1 - a0) * static_cast<float>(i) / n;
            local.push_back({t.center.x + t.radius * cosf(ang), t.center.y + t.radius * sinf(ang)});
        }
    }
    std::vector<Vector3> out;
    const float y = MapPieceY(p) + kTrackTop * p.scale;
    for (Vector2 l : local) {
        Vector2 r = MapRotate(l.x * p.scale, l.y * p.scale, p.yaw);
        out.push_back({p.x + r.x, y, p.z + r.y});
    }
    return out;
}

// Ende als ganzzahliger Schlüssel (auf 5 cm gerundet), damit Rundungsfehler
// zweier Teile am selben Punkt nicht stören.
std::pair<int, int> EndKey(Vector3 v) {
    return {static_cast<int>(lroundf(v.x * 20.0f)), static_cast<int>(lroundf(v.z * 20.0f))};
}

} // namespace

std::vector<MonorailLoop> FindMonorailLoops(const std::vector<MapPiece>& pieces) {
    // Alle Schienen mit ihren Mittellinien sammeln
    struct Track {
        std::vector<Vector3> pts;   // von Ende 0 nach Ende 1
        std::pair<int, int> end[2];
    };
    std::vector<Track> tracks;
    for (const MapPiece& p : pieces) {
        const TrackShape* t = Shape(p.model);
        if (!t) continue;
        Track tr;
        tr.pts = TrackSamples(p, *t);
        tr.end[0] = EndKey(tr.pts.front());
        tr.end[1] = EndKey(tr.pts.back());
        tracks.push_back(std::move(tr));
    }
    // Welche Schienenenden liegen an welchem Punkt? (Teil, Ende)
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> at;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        for (int e = 0; e < 2; ++e) at[tracks[i].end[e]].push_back({i, e});

    // Von jeder noch nicht besuchten Schiene aus über Ende 1 weiterfahren,
    // bis man wieder bei ihrem Ende 0 ankommt (Kreis) oder es nicht eindeutig
    // weitergeht (offenes Ende, Abzweigung).
    std::vector<MonorailLoop> loops;
    std::vector<bool> visited(tracks.size(), false);
    for (int start = 0; start < static_cast<int>(tracks.size()); ++start) {
        if (visited[start]) continue;
        std::vector<std::pair<int, int>> path;   // (Teil, Ende, an dem der Zug hineinfährt)
        std::vector<bool> inPath(tracks.size(), false);
        int cur = start, enter = 0;
        bool closed = false;
        while (true) {
            path.push_back({cur, enter});
            inPath[cur] = true;
            const int exitEnd = 1 - enter;
            std::vector<std::pair<int, int>> next;
            for (const auto& pe : at[tracks[cur].end[exitEnd]])
                if (!(pe.first == cur && pe.second == exitEnd)) next.push_back(pe);
            if (next.size() != 1) break;
            if (next[0].first == start) { closed = next[0].second == 0; break; }
            if (inPath[next[0].first] || visited[next[0].first]) break;
            cur = next[0].first;
            enter = next[0].second;
        }
        for (const auto& pe : path) visited[pe.first] = true;
        if (!closed) continue;

        MonorailLoop loop;
        loop.pieces = static_cast<int>(path.size());
        for (const auto& pe : path) {
            std::vector<Vector3> pts = tracks[pe.first].pts;
            if (pe.second == 1) std::reverse(pts.begin(), pts.end());
            // Der erste Punkt eines Teils ist der letzte des vorigen
            for (size_t k = loop.points.empty() ? 0 : 1; k < pts.size(); ++k) loop.points.push_back(pts[k]);
        }
        loop.dist.resize(loop.points.size(), 0.0f);
        for (size_t k = 1; k < loop.points.size(); ++k)
            loop.dist[k] = loop.dist[k - 1] + Vector3Distance(loop.points[k - 1], loop.points[k]);
        loop.length = loop.dist.back();
        if (loop.length > 0.1f) loops.push_back(std::move(loop));
    }
    return loops;
}

Vector3 MonorailPoint(const MonorailLoop& loop, float s, Vector3* dir) {
    if (loop.points.size() < 2 || loop.length <= 0.0f) {
        if (dir) *dir = {0, 0, -1};
        return loop.points.empty() ? Vector3{0, 0, 0} : loop.points[0];
    }
    s = fmodf(s, loop.length);
    if (s < 0.0f) s += loop.length;
    // Abschnitt suchen, in dem s liegt (dist ist aufsteigend sortiert)
    size_t k = static_cast<size_t>(std::upper_bound(loop.dist.begin(), loop.dist.end(), s) - loop.dist.begin());
    k = std::clamp<size_t>(k, 1, loop.points.size() - 1);
    const Vector3 a = loop.points[k - 1], b = loop.points[k];
    const float len = loop.dist[k] - loop.dist[k - 1];
    const float t = len > 0.0f ? (s - loop.dist[k - 1]) / len : 0.0f;
    if (dir) *dir = Vector3Normalize(Vector3Subtract(b, a));
    return Vector3Lerp(a, b, t);
}
