#pragma once
// ---------------------------------------------------------------------------
// GameMap.hpp – selbst gebaute Karten aus dem Karteneditor: Datenformat,
// Liste der setzbaren Segmente und die Erkennung geschlossener
// Monorail-Kreise (Umsetzung: GameMap.cpp).
//
// Was ist eine Karte?
//   Nur Kulisse: eine Liste von Kit-Modellen (Krater, Gelände, Felsen,
//   Straßen, Gebäude, Monorail-Schienen) mit Position, Drehung und Größe.
//   Stationen, Lagerstätten und Gegner legt sie NICHT fest, die entstehen wie
//   bisher (World::BuildStations, SpawnDeposit ...). Wie die Zufalls-Deko hat
//   die Karte keine Kollision, Schiffe fliegen darüber hinweg.
//
// Wer benutzt sie?
//   - src/MapEditor.cpp: Karteneditor (Segmente setzen, Zufall generieren)
//   - src/MapFiles.cpp: Karten als Datei speichern und laden
//   - shared/Protocol.cpp: Karten hochladen und die aktive Karte des Servers
//     an die Clients schicken (MapUploadMsg, MapDataMsg)
//   - server/src/Accounts.cpp: hochgeladene Karten im Datenordner
//   - shared/WorldGen.cpp: World::ApplyMap ersetzt die Zufalls-Deko durch die Karte
//   - src/Render.cpp: Züge auf geschlossenen Monorail-Kreisen (MonorailPoint)
//
// Dateiformat (Text, eine Angabe pro Zeile, siehe MapToText):
//   MONDSCHUERFER_KARTE 1          Kennung und Formatversion
//   name <Name bis Zeilenende>
//   author <Spielername>           nur auf dem Server (wer sie hochgeladen hat)
//   piece <Modell> <x> <z> <Drehung> <Größe>    (eine Zeile je Teil)
//   end
//
// Raster: Der Editor setzt die Teile auf ein Raster mit mapgrid::CELL Metern
// Kantenlänge. Die Schienen und Geländeteile des Kits sind genau ein Kit-Feld
// groß und werden mit CELL skaliert, füllen also genau ein Rasterfeld.
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
} // namespace mapgrid

// Ein Teil der Karte. Position = Bezugspunkt des Modells am Boden; die Höhe
// ergibt sich aus dem Modell (MapPieceY).
struct MapPiece {
    std::string model;          // Kit-Modell (Dateiname ohne .glb), muss in MapSegments() stehen
    float x = 0.0f, z = 0.0f;
    float yaw = 0.0f;           // Drehung um die senkrechte Achse (Bogenmaß)
    float scale = 1.0f;         // Größe (1 = Originalgröße im Kit)
};

struct GameMap {
    std::string name;
    std::string author;         // nur auf dem Server: Konto, das sie hochgeladen hat
    std::vector<MapPiece> pieces;
};

// Gruppen im Werkzeugfenster des Editors (Reihenfolge = Reihenfolge der Reiter)
enum class SegmentGroup { Terrain, Rocks, Roads, Monorail, Buildings, Misc, Count };
const char* SegmentGroupName(SegmentGroup g);

// Ein setzbares Segment. scale = Größe beim Setzen im Editor.
struct MapSegment {
    const char* model;
    const char* label;
    SegmentGroup group;
    float scale;
};
// Alle Segmente, nach Gruppen geordnet. Nur diese Modelle dürfen in einer
// Karte stehen (MapFromText verwirft andere).
const std::vector<MapSegment>& MapSegments();
const MapSegment* FindSegment(const std::string& model);   // nullptr = nicht erlaubt

// Text <-> Karte. MapFromText liefert false, wenn Kennung oder Version nicht
// passen; ungültige Zeilen (unbekanntes Modell, außerhalb der Karte) werden
// übersprungen.
std::string MapToText(const GameMap& m);
bool MapFromText(const std::string& text, GameMap& out);
// Kartenname -> Dateiname ohne Endung (nie leer): nur a-z, A-Z, 0-9, '-' und
// '_', Umlaute umgeschrieben, alles andere wird '_'. Mit reinen ASCII-Namen
// gibt es auch unter Windows keine Probleme mit Sonderzeichen im Pfad.
std::string MapFileStem(const std::string& name);

// Höhe, in der ein Teil steht: nichts ragt unter den Boden, flache Teile
// (Straßen) liegen knapp darüber, sonst flimmern sie mit der Bodenplatte.
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
