#pragma once
// ---------------------------------------------------------------------------
// GameMap.hpp – selbst gebaute Karten aus dem Karteneditor: Datenformat,
// Liste der setzbaren Segmente und die Erkennung geschlossener
// Monorail-Kreise (Umsetzung: GameMap.cpp).
//
// Was ist eine Karte?
//   Zwei Arten (GameMap::kind):
//   - Mondkarte: nur Kulisse, eine Liste von Kit-Modellen (Krater, Gelände,
//     Felsen, Straßen, Gebäude, Monorail-Schienen) mit Position, Drehung und
//     Größe. Stationen, Lagerstätten und Gegner legt sie NICHT fest, die
//     entstehen wie bisher (World::BuildStations, SpawnDeposit ...). Wie die
//     Zufalls-Deko hat die Karte keine Kollision, Schiffe fliegen darüber hinweg.
//   - Einsatzkarte (FPS-Modus): eigenes kleines Spielfeld (size) mit Teilen
//     aus dem Fantasy Town Kit und dem Graveyard Kit, Thema, Boden und Himmel.
//     Dazu Markierungen: genau ein Stargate, Kristallplätze und
//     Gegner-Startplätze. Ein SG-Team spielt sie als eigene Welt
//     (World::NewInstance), Mauern, Treppen und Deko sind für die Figuren
//     Hindernisse (World::BuildTrooperBlocks).
//
// Wer benutzt sie?
//   - src/MapEditor.cpp: Karteneditor (Segmente setzen, Zufall generieren)
//   - src/MapFiles.cpp: Karten als Datei speichern und laden
//   - shared/Protocol.cpp: Karten hochladen und die aktive Karte des Servers
//     an die Clients schicken (MapUploadMsg, MapDataMsg)
//   - server/src/Accounts.cpp: hochgeladene Karten im Datenordner
//   - shared/WorldGen.cpp: World::ApplyMap ersetzt die Zufalls-Deko durch die
//     Mondkarte, World::NewInstance baut aus einer Einsatzkarte eine eigene Welt
//   - src/CommandCenter.cpp: Ziel des Stargates (Mondkarte oder Einsatzkarte)
//   - src/Render.cpp: Züge auf geschlossenen Monorail-Kreisen (MonorailPoint)
//
// Dateiformat (Text, eine Angabe pro Zeile, siehe MapToText):
//   MONDSCHUERFER_KARTE 1          Kennung und Formatversion
//   name <Name bis Zeilenende>
//   author <Spielername>           nur auf dem Server (wer sie hochgeladen hat)
//   kind einsatz                   nur Einsatzkarten (fehlt = Mondkarte, so bleiben alte Karten gültig)
//   theme <dorf|wald|friedhof>     nur Einsatzkarten
//   size <Kantenlänge in Metern>   nur Einsatzkarten
//   ground <gras|erde|stein|sand>  nur Einsatzkarten
//   sky <tag|abend|nacht>          nur Einsatzkarten
//   piece <Modell> <x> <z> <Drehung> <Größe> [<Höhe>]   (eine Zeile je Teil)
//   end
//
// Raster: Der Editor setzt die Teile auf ein Raster mit MapCell Metern
// Kantenlänge (Mondkarte mapgrid::CELL, Einsatzkarte mapgrid::MISSION_CELL).
// Die Schienen und Geländeteile des Space Kits sind genau ein Kit-Feld groß
// und werden mit CELL skaliert, füllen also genau ein Rasterfeld. Die Teile
// der beiden anderen Kits sind schon in Originalgröße ein Feld groß (Größe 1).
// Höhe (nur Einsatzkarten): Ebene über dem Boden, damit Dächer auf den Mauern
// stehen (eine Ebene = mapgrid::LEVEL Meter, eine Mauer des Fantasy Town Kits).
//
// Abhängigkeiten: raylib.h/raymath.h (Datentypen, Vektor-Mathe), Models.hpp
// (Abmessungen der Modelle), Config.hpp (Kartengröße). Keine Grafik, damit
// auch der Server Karten lesen kann.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include <string>
#include <vector>

namespace mapgrid {
constexpr float CELL = 3.0f;            // Rasterweite des Editors in Metern
constexpr int MAX_PIECES = 6000;        // höchstens so viele Teile je Karte (größere Dateien werden gekürzt)
constexpr int MAX_NAME = 40;            // Länge des Kartennamens in Zeichen
constexpr float TRAIN_SPEED = 6.0f;     // Monorail-Zug: Meter pro Sekunde
constexpr float TRAIN_CAR_GAP = 3.15f;  // Abstand der Wagenmitten (ein Wagen ist 1 Kit-Feld = CELL lang)
// Einsatzkarten
constexpr float MISSION_CELL = 1.0f;    // Rasterweite: ein Feld des Fantasy Town und Graveyard Kits
constexpr float LEVEL = 1.0f;           // Höhe einer Ebene (Mauer des Fantasy Town Kits)
constexpr int MAX_LEVEL = 3;            // höchste Ebene im Editor
constexpr int MISSION_SIZES[4] = {60, 90, 120, 160};   // wählbare Kantenlängen in Metern
constexpr int MISSION_SIZE_DEFAULT = 90;
constexpr int MISSION_SIZE_MIN = 40, MISSION_SIZE_MAX = 200;   // Grenzen beim Einlesen
constexpr float GATE_SCALE = 6.0f;      // Größe des Stargates (wie die Tore der Mondkarte)
constexpr float CRYSTAL_SCALE = 2.8f;   // Größe der Kristalle an einem Kristallplatz (kleine Lagerstätte)
} // namespace mapgrid

// Kartenart und die Kopfzeilen der Einsatzkarten. Die Zahlen gehen so über
// das Netzwerk (Protocol.cpp), also nur hinten anfügen.
enum class MapKind { Moon = 0, Mission = 1 };
enum class MapTheme { Village = 0, Forest = 1, Graveyard = 2, Count };   // dorf, wald, friedhof
enum class MapGround { Grass = 0, Earth = 1, Stone = 2, Sand = 3, Count };
enum class MapSky { Day = 0, Evening = 1, Night = 2, Count };
const char* MapThemeName(MapTheme t);     // "Dorf" ... (Anzeige)
const char* MapGroundName(MapGround g);   // "Gras" ...
const char* MapSkyName(MapSky s);         // "Tag" ...

// Ein Teil der Karte. Position = Bezugspunkt des Modells am Boden; die Höhe
// ergibt sich aus dem Modell (MapPieceY).
struct MapPiece {
    std::string model;          // Kit-Modell (Dateiname ohne .glb), muss in MapSegments() stehen
    float x = 0.0f, z = 0.0f;
    float yaw = 0.0f;           // Drehung um die senkrechte Achse (Bogenmaß)
    float scale = 1.0f;         // Größe (1 = Originalgröße im Kit)
    float lift = 0.0f;          // Höhe über dem Boden (Einsatzkarte: Ebene * mapgrid::LEVEL)
};

struct GameMap {
    std::string name;
    std::string author;         // nur auf dem Server: Konto, das sie hochgeladen hat
    MapKind kind = MapKind::Moon;
    // nur Einsatzkarten:
    MapTheme theme = MapTheme::Village;
    int size = mapgrid::MISSION_SIZE_DEFAULT;   // Kantenlänge des quadratischen Spielfelds in Metern
    MapGround ground = MapGround::Grass;
    MapSky sky = MapSky::Day;
    std::vector<MapPiece> pieces;
};

// Halbe Kantenlänge des Spielfelds (Mondkarte: cfg::WORLD_HALF) und Rasterweite
float MapHalf(const GameMap& m);
float MapCell(MapKind k);

// Gruppen im Werkzeugfenster des Editors (Reihenfolge = Reihenfolge der
// Reiter). Die ersten sechs gehören zur Mondkarte, die übrigen zur Einsatzkarte.
enum class SegmentGroup {
    Terrain, Rocks, Roads, Monorail, Buildings, Misc,
    Walls, Roofs, Village, Paths, Nature, Graves, GraveWalls, Markers, Farm, Crypts,
    Count
};
const char* SegmentGroupName(SegmentGroup g);
bool GroupForKind(SegmentGroup g, MapKind k);   // Reiter dieser Kartenart?

// Ein setzbares Segment. scale = Größe beim Setzen im Editor.
struct MapSegment {
    const char* model;
    const char* label;
    SegmentGroup group;
    float scale;
};
// Alle Segmente, nach Gruppen geordnet. Nur diese Modelle dürfen in einer
// Karte stehen (MapFromText verwirft andere). Neue Segmente nur hinten
// anfügen: Über das Netzwerk geht die Stelle in dieser Liste (Protocol.cpp).
const std::vector<MapSegment>& MapSegments();
const MapSegment* FindSegment(const std::string& model);   // nullptr = nicht erlaubt

// Markierungen der Einsatzkarte (Gruppe Markers): keine Modelle, sondern
// Plätze, die World::NewInstance füllt (Stargate, Kristalle, Gegner).
extern const char* const MARKER_GATE;
extern const char* const MARKER_CRYSTAL;
extern const char* const MARKER_ENEMY;
bool IsMarker(const std::string& model);
// Abmessungen eines Segments ohne Größe (wie ModelBounds; Markierungen: ihre
// Fläche am Boden, das Stargate mit seiner Treppe)
BoundingBox SegmentBounds(const std::string& model);

// Text <-> Karte. MapFromText liefert false, wenn Kennung oder Version nicht
// passen; ungültige Zeilen (unbekanntes Modell, außerhalb der Karte) werden
// übersprungen.
std::string MapToText(const GameMap& m);
bool MapFromText(const std::string& text, GameMap& out);
// Kartenname -> Dateiname ohne Endung (nie leer): nur a-z, A-Z, 0-9, '-' und
// '_', Umlaute umgeschrieben, alles andere wird '_'. Mit reinen ASCII-Namen
// gibt es auch unter Windows keine Probleme mit Sonderzeichen im Pfad.
std::string MapFileStem(const std::string& name);

// Höhe, in der ein Teil steht: nichts ragt unter den Boden (bzw. unter seine
// Ebene, MapPiece::lift), flache Teile (Straßen) liegen knapp darüber, sonst
// flimmern sie mit der Bodenplatte.
float MapPieceY(const MapPiece& p);
// Dreht einen Punkt (x, z) um yaw, genau wie MatrixRotateY beim Zeichnen.
Vector2 MapRotate(float x, float z, float yaw);
bool IsTrack(const std::string& model);   // Monorail-Schiene?

// Ein geschlossener Monorail-Kreis: die Mittellinie der Schienen als
// Punktfolge (letzter Punkt = erster), y = Oberkante der Schiene.
struct MonorailLoop {
    std::vector<Vector3> points;
    std::vector<float> dist;   // Weglänge vom ersten Punkt bis Punkt i
    float length = 0.0f;
    int pieces = 0;            // Anzahl Schienenteile im Kreis
};
// Sucht alle geschlossenen Kreise. Zwei Schienen sind verbunden, wenn ihre
// Enden am selben Punkt liegen. Ein Kreis zählt nur, wenn jedes Ende genau
// einen Partner hat (keine Abzweigungen, keine offenen Enden).
std::vector<MonorailLoop> FindMonorailLoops(const std::vector<MapPiece>& pieces);
// Punkt nach s Metern Weg auf dem Kreis (läuft im Kreis weiter), dir = Fahrtrichtung.
Vector3 MonorailPoint(const MonorailLoop& loop, float s, Vector3* dir);
