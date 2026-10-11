#pragma once
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
    bool Init();     // nach InitWindow() aufrufen
    void Shutdown();

    // Liefert die ID eines Modells (Dateiname ohne .glb), siehe ModelId().
    int Id(const std::string& name);

    // Zeichnet ein Modell mit einer Welt-Matrix (Skalierung * Rotation * Translation).
    void Draw(int id, const Matrix& world, Color tint = WHITE, Vector3 emissive = {0, 0, 0});
    void Draw(int id, Vector3 pos, float yaw, float scale, Color tint = WHITE, Vector3 emissive = {0, 0, 0});

    // Abmessungen des geladenen Modells (lädt es bei Bedarf)
    BoundingBox Bounds(int id);

    // Farbe der Mondoberfläche (Gestein). Wirkt sofort auf alle geladenen Modelle.
    void SetMoonColor(MoonColor c);
    MoonColor GetMoonColor() const { return moon_; }

    // Kamera-/Lichtparameter für den Shader setzen (einmal pro Frame)
    void SetCamera(Vector3 camPos);

    const std::string& AssetDir() const { return assetDir_; }

private:
    struct Entry {
        bool loaded = false;
        Model model{};
        Matrix base{};
        BoundingBox bounds{};
        std::vector<Color> original;   // Originalfarben der Materialien (für die Mondfarbe)
    };
    Entry* Ensure(int id);             // Modell zur ID laden, falls noch nicht geschehen
    void ApplyPalette(Entry& e);

    std::vector<Entry> entries_;       // Index = Modell-ID
    std::string assetDir_;
    MoonColor moon_ = MoonColor::Grey;

    Shader lit_{};
    int locLightDir_ = -1;
    int locViewPos_ = -1;
    int locEmissive_ = -1;
    bool ready_ = false;
};

extern Assets g_assets;

// Hilfsfunktionen für Matrizen
Matrix TRS(Vector3 pos, float yaw, float scale);
Matrix TRS(Vector3 pos, float yaw, float pitch, float roll, float scale);

// ForwardFromYaw, YawTowards und AngleDiff stehen in shared/Util.hpp.
