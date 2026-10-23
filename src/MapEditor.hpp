#pragma once
// ---------------------------------------------------------------------------
// MapEditor.hpp – der Karteneditor (Umsetzung: MapEditor.cpp).
//
// Wofür ist die Datei da?
//   Aus dem Hauptmenü ("Karteneditor") baut man eigene Karten. Eine Karte ist
//   nur Kulisse (Gelände, Krater, Felsen, Straßen, Gebäude, Monorail), siehe
//   shared/GameMap.hpp. Zwei Ansichten:
//     1. Kartenliste: gespeicherte Karten (Ordner "karten" neben der .exe),
//        Knöpfe "Karte laden" (wenn eine gewählt ist), "Neu" (leere Karte)
//        und "Zurück".
//     2. Bearbeiten: links das Werkzeugfenster mit "Zufall generieren" und
//        den Segmenten (Reiter je Gruppe), oben Name, "Speichern" und
//        "Zurück", in der Mitte die Karte von schräg oben.
//   Bildet die Monorail einen geschlossenen Kreis, fährt darauf ein Zug aus
//   monorail_trainFront, monorail_trainPassenger und monorail_trainEnd –
//   schon im Editor und später im Spiel (Render.cpp).
//
// Bedienung beim Bearbeiten:
//   Linksklick setzt das gewählte Segment (gedrückt halten: malen),
//   Rechtsklick löscht das Teil unter dem Zeiger (gedrückt halten: radieren),
//   R dreht um 90°, WASD/Pfeiltasten oder mittlere Maustaste verschieben die
//   Ansicht, Mausrad zoomt, Strg+S speichert, ESC hebt die Auswahl auf bzw.
//   geht zurück zur Kartenliste.
//
// Wer benutzt sie?
//   src/main.cpp: Bildschirm Screen::Editor (wie der Mehrspieler ein eigenes
//   Objekt, das Update/Draw bekommt) und die Kartenauswahl bei "Neues Spiel"
//   (MapListView).
//
// Abhängigkeiten: shared/World.hpp (Kulisse zeichnen: Boden, Stationen,
// Teile der Karte), shared/GameMap.hpp, MapFiles.hpp, Ui.hpp, Audio.hpp,
// Assets.hpp, raylib/rlgl.
// ---------------------------------------------------------------------------
#include "MapFiles.hpp"
#include "Ui.hpp"
#include "World.hpp"
#include <string>
#include <vector>

// Liste der gespeicherten Karten mit Auswahl (Klick, Pfeiltasten) und
// Mausrad-Bildlauf. Doppelklick oder Enter "öffnet" den gewählten Eintrag.
class MapListView {
public:
    void Refresh();   // Ordner neu einlesen, Auswahl zurücksetzen
    const std::vector<MapFileInfo>& Maps() const { return maps_; }
    // Gewählte Karte als Index in Maps(); -1 = keine Karte (nichts gewählt
    // oder der zusätzliche erste Eintrag, siehe Draw).
    int SelectedMap() const { return row_ - (hasFirst_ ? 1 : 0); }
    bool HasSelection() const { return row_ >= 0; }
    // Zeichnet die Liste in r (s = ui::Scale()). firstEntry: optionaler
    // Eintrag vor den Karten (Hauptmenü: "Standardkarte"), der dann
    // vorgewählt ist. Liefert true bei Doppelklick oder Enter.
    bool Draw(Rectangle r, float s, const char* firstEntry = nullptr);

private:
    std::vector<MapFileInfo> maps_;
    int row_ = -1;            // gewählte Zeile der Anzeige
    float scroll_ = 0.0f;     // erste sichtbare Zeile
    bool hasFirst_ = false;
    bool fresh_ = true;       // nach Refresh: erste Zeile vorwählen, falls firstEntry
    double lastClick_ = -10.0;
    int lastClickRow_ = -1;
};

class MapEditorScreen {
public:
    MapEditorScreen();
    void Update(float dt);   // Kamera, Zeit der Welt (Zug fährt)
    void Draw();             // zwischen BeginDrawing/ClearBackground und EndDrawing
    bool WantsMenu() const { return wantsMenu_; }

private:
    enum class Mode { List, Edit };

    // Wo ein Segment unter dem Zeiger landen würde
    struct Placement {
        bool valid = false;     // ganz im Spielfeld?
        float x = 0, z = 0;     // Bezugspunkt des Modells
        float cx = 0, cz = 0;   // Mitte des belegten Rasterbereichs
        int fw = 1, fd = 1;     // belegte Rasterfelder in x und z
        float yaw = 0, scale = 1;
    };

    void OpenMap(const GameMap& m, const std::string& stem);
    void Rebuild();                       // Kulisse nach jeder Änderung neu aufbauen
    bool Dirty() const { return dirty_ || nameField_.text != map_.name; }
    Placement PlacementAt(Vector3 g) const;
    int PieceAt(Vector3 g) const;         // Index in map_.pieces oder -1
    void Place(const Placement& p);
    void RandomFill();
    void Save();
    void Back();
    void Message(const std::string& text, Color c);

    void DrawList();
    void DrawEdit();
    void DrawScene();                     // 3D: Welt, Raster, Vorschau
    void DrawToolPanel(Rectangle r, float s);

    Mode mode_ = Mode::List;
    MapListView list_;
    World world_;              // nur zum Zeichnen: Boden, Stationen aller Bauplätze, Teile der Karte
    GameMap map_;
    std::string stem_;         // Datei, aus der die Karte geladen wurde ("" = neu)
    ui::TextField nameField_;
    bool dirty_ = false;

    int group_ = 0;            // offener Reiter im Werkzeugfenster (SegmentGroup)
    int segment_ = -1;         // gewähltes Segment (Index in MapSegments()), -1 = keins
    int rot_ = 0;              // Drehung in 90-Grad-Schritten
    int loopPieces_ = 0;       // Schienenteile in geschlossenen Kreisen
    int trackPieces_ = 0;      // alle Schienenteile

    Camera3D cam_{};
    Vector3 camTarget_{0, 0, 0};
    float camHeight_ = 80.0f;
    bool groundOk_ = false;    // liegt der Zeiger über dem Boden?
    Vector3 ground_{};
    bool painting_ = false, erasing_ = false;
    float lastX_ = 1e9f, lastZ_ = 1e9f;   // zuletzt gemalter Bezugspunkt

    std::string msg_;
    Color msgColor_ = WHITE;
    float msgT_ = 0.0f;
    float backArmed_ = 0.0f;   // > 0: "Zurück" trotz ungespeicherter Änderungen ein zweites Mal drücken
    bool wantsMenu_ = false;
};
