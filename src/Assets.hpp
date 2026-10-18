#pragma once
// ---------------------------------------------------------------------------
// Assets.hpp – 3D-Modelle und Beleuchtung des Spiels (Umsetzung: Assets.cpp).
//
// Wofür ist die Datei da?
//   Die Klasse Assets lädt die 3D-Modelle aus dem Kenney Space Kit
//   (assets/models/*.glb), färbt das Gestein passend zur Mondfarbe um und
//   zeichnet die Modelle mit einem eigenen Beleuchtungs-Shader. Es gibt genau
//   ein Objekt davon: g_assets (global, weil es genau ein Fenster mit genau
//   einem Grafikkontext gibt und viele Stellen zeichnen).
//
// Wer benutzt sie?
//   - src/main.cpp: Init() nach InitWindow(), Shutdown() vor CloseWindow(),
//     AssetDir() als Basisordner für die Töne (Audio.cpp lädt "sounds/").
//   - src/Render.cpp (World::Draw): Draw(), SetCamera(), Id(), GetMoonColor().
//   - src/Multiplayer.cpp: SetMoonColor() mit der Einstellung des Servers
//     (Einzelspieler und Hauptmenü: immer grau, siehe auch main.cpp).
//   - src/Hud.cpp und src/Save.cpp binden den Header ein, brauchen aber nur
//     die Header, die er mitbringt (raymath.h, Util.hpp, GameSettings.hpp).
//   Der Server benutzt diese Datei NICHT (er hat keine Grafik). Die
//   Spiellogik (shared/World.hpp) kennt Modelle nur als Nummer, die Modell-ID
//   aus dem Modell-Verzeichnis shared/Models.hpp.
//
// Abhängigkeiten:
//   raylib (LoadModel, DrawModel, Shader-Funktionen; alles braucht ein offenes
//   Fenster mit OpenGL-Kontext), raymath.h (Matrizen und Vektoren),
//   shared/Models.hpp (ID <-> Dateiname), shared/GameSettings.hpp (MoonColor).
//
// Ablauf:
//   1. Init(): Assets-Ordner suchen, Shader übersetzen, Sonnenrichtung
//      setzen. Es wird dabei noch KEIN Modell geladen.
//   2. Jedes Frame: SetCamera() (für das Randlicht im Shader), danach Draw()
//      für jedes Objekt. Beim ersten Draw() einer ID wird das Modell geladen
//      ("lazy loading"): Der Start geht schnell, und nur Modelle, die wirklich
//      vorkommen, belegen Grafikspeicher.
//   3. Shutdown(): alle Modelle und den Shader wieder freigeben.
//
// Alles läuft im Haupt-Thread (dem Thread mit dem OpenGL-Kontext); raylib
// ist nicht für mehrere Threads gebaut.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "raymath.h"
#include "GameSettings.hpp"
#include "Util.hpp"
#include <string>
#include <vector>

// Verwaltet die 3D-Modelle aus dem Kenney Space Kit und den Beleuchtungs-Shader.
//
// Die Modell-IDs kommen aus dem Modell-Verzeichnis (shared/Models.hpp), das
// auch die Spiellogik benutzt. Das 3D-Modell zu einer ID wird erst geladen,
// wenn es zum ersten Mal gezeichnet wird.
class Assets {
public:
    // Nach InitWindow() aufrufen: Shader und Modelle liegen im Grafikspeicher
    // und brauchen den OpenGL-Kontext des Fensters. false = Assets-Ordner nicht
    // gefunden (main.cpp zeigt dann eine Fehlermeldung und beendet sich).
    bool Init();
    void Shutdown();   // vor CloseWindow() aufrufen

    // Liefert die ID eines Modells (Dateiname ohne .glb), siehe ModelId().
    // Unbekannte Namen bekommen eine neue ID; geladen wird erst beim Zeichnen.
    int Id(const std::string& name);

    // Zeichnet ein Modell mit einer Welt-Matrix. Die Matrix bringt das Modell
    // aus seinem eigenen Koordinatensystem (Ursprung = Bezugspunkt des Modells)
    // an seinen Platz in der Welt: erst skalieren, dann drehen, dann
    // verschieben (siehe TRS unten).
    //   tint:     wird mit der Materialfarbe multipliziert (WHITE = unverändert),
    //             z. B. rötlich für Piraten, dunkelgrau für Wracks.
    //   emissive: Farbe, die unabhängig von der Beleuchtung dazukommt (Leuchten),
    //             je Kanal 0..1, z. B. das Aufblitzen bei einem Treffer.
    void Draw(int id, const Matrix& world, Color tint = WHITE, Vector3 emissive = {0, 0, 0});
    // Kurzform: Position, Drehung um die senkrechte Achse (yaw, Bogenmaß) und
    // gleichmäßige Skalierung.
    void Draw(int id, Vector3 pos, float yaw, float scale, Color tint = WHITE, Vector3 emissive = {0, 0, 0});

    // Abmessungen des geladenen Modells (lädt es bei Bedarf), schon um den
    // Kit-Versatz korrigiert. Zur Zeit nirgends benutzt: Die Spiellogik nimmt
    // ModelBounds() aus shared/Models.hpp, weil der Server keine Modelle lädt.
    BoundingBox Bounds(int id);

    // Farbe der Mondoberfläche (Gestein). Wirkt sofort auf alle geladenen Modelle.
    // Einzelspieler: immer grau; Mehrspieler: Einstellung des Servers.
    void SetMoonColor(MoonColor c);
    MoonColor GetMoonColor() const { return moon_; }

    // Kameraposition an den Shader geben (einmal pro Frame, vor dem ersten
    // Draw()). Der Shader braucht sie für das Randlicht. Die Sonnenrichtung
    // ist fest und wird nur einmal in Init() gesetzt.
    void SetCamera(Vector3 camPos);

    // Gefundener Assets-Ordner mit "/" am Ende, z. B. "assets/" oder der
    // Ordner neben der ausführbaren Datei.
    const std::string& AssetDir() const { return assetDir_; }

private:
    // Ein Eintrag pro Modell-ID.
    struct Entry {
        // true, sobald ein Ladeversuch stattgefunden hat – auch wenn er
        // fehlgeschlagen ist. So wird eine fehlende Datei nicht jedes Frame
        // erneut gesucht (ein leeres Modell zeichnet einfach nichts).
        bool loaded = false;
        // raylib-Model: Meshes (Geometrie, liegt im Grafikspeicher),
        // Materialien (Farbe + Shader) und eine transform-Matrix, die bei
        // jedem Draw() neu gesetzt wird.
        Model model{};
        Matrix base{};                 // gleicht den Versatz der Kit-Modelle aus (siehe Ensure)
        BoundingBox bounds{};
        std::vector<Color> original;   // Originalfarben der Materialien (für die Mondfarbe)
    };
    Entry* Ensure(int id);             // Modell zur ID laden, falls noch nicht geschehen
    // Gesteinsfarben eines Modells aus den Originalfarben neu berechnen (nach moon_).
    // Immer von den Originalen aus, damit mehrfaches Umfärben nichts verfälscht.
    void ApplyPalette(Entry& e);

    std::vector<Entry> entries_;       // Index = Modell-ID; wächst mit, wenn neue IDs dazukommen
    std::string assetDir_;
    MoonColor moon_ = MoonColor::Grey;

    // Beleuchtungs-Shader, von allen Modellen gemeinsam benutzt.
    Shader lit_{};
    // "Locations" der eigenen Shader-Uniforms. Ein Uniform ist ein Wert, den
    // das Programm an den Shader übergibt und der für alle Pixel eines
    // Zeichenaufrufs gleich ist (z. B. die Sonnenrichtung). Die Location
    // (Platznummer im Shader-Programm) fragt man einmal mit GetShaderLocation()
    // ab; -1 bedeutet "gibt es nicht", dann ignoriert raylib das Setzen.
    int locLightDir_ = -1;
    int locViewPos_ = -1;
    int locEmissive_ = -1;
    bool ready_ = false;               // Init() war erfolgreich; vorher zeichnet Draw() nichts
};

// Das einzige Assets-Objekt (definiert in Assets.cpp).
extern Assets g_assets;

// Hilfsfunktionen für Welt-Matrizen ("TRS" = Translation · Rotation · Scale).
//
// Eine 4x4-Matrix beschreibt, wie die Eckpunkte eines Modells verschoben,
// gedreht und skaliert werden. Die Reihenfolge ist wichtig: Das Modell wird
// zuerst skaliert, dann um seinen eigenen Ursprung gedreht und zuletzt an die
// Position verschoben. Würde man zuerst verschieben, würde sich das Modell
// beim Drehen um den Weltmittelpunkt bewegen statt um sich selbst.
// In raylib bedeutet MatrixMultiply(a, b): "erst a, dann b". Deshalb steht in
// Assets.cpp MatrixMultiply(Skalierung, Drehung) und danach die Verschiebung.
//
// Winkel in Bogenmaß:
//   yaw   = Drehung um die senkrechte Y-Achse (Kurs, siehe ForwardFromYaw)
//   pitch = Drehung um die Querachse X (negativ = Nase nach unten, weil die
//           Kit-Schiffe mit der Nase in Richtung -Z zeigen)
//   roll  = Drehung um die Längsachse Z (seitliches Neigen in Kurven)
// scale: gleichmäßig in alle Richtungen (1 = Originalgröße des Kits).
Matrix TRS(Vector3 pos, float yaw, float scale);
Matrix TRS(Vector3 pos, float yaw, float pitch, float roll, float scale);

// ForwardFromYaw, YawTowards und AngleDiff stehen in shared/Util.hpp.
