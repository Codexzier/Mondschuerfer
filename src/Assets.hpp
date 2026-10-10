#pragma once
#include "raylib.h"
#include "raymath.h"
#include <string>
#include <unordered_map>
#include <vector>

// Verwaltet die 3D-Modelle aus dem Kenney Space Kit und den Beleuchtungs-Shader.
class Assets {
public:
    bool Init();     // nach InitWindow() aufrufen
    void Shutdown();

    // Liefert die ID eines Modells (Dateiname ohne .glb). Lädt bei Bedarf nach.
    int Id(const std::string& name);

    // Zeichnet ein Modell mit einer Welt-Matrix (Skalierung * Rotation * Translation).
    void Draw(int id, const Matrix& world, Color tint = WHITE, Vector3 emissive = {0, 0, 0});
    void Draw(int id, Vector3 pos, float yaw, float scale, Color tint = WHITE, Vector3 emissive = {0, 0, 0});

    BoundingBox Bounds(int id) const { return entries_[id].bounds; }

    // Kamera-/Lichtparameter für den Shader setzen (einmal pro Frame)
    void SetCamera(Vector3 camPos);

    const std::string& AssetDir() const { return assetDir_; }

private:
    struct Entry {
        Model model{};
        Matrix base{};
        BoundingBox bounds{};
    };
    std::vector<Entry> entries_;
    std::unordered_map<std::string, int> byName_;
    std::string assetDir_;

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

// Vorwärtsrichtung eines Kit-Raumschiffs (Nase zeigt im Modell nach -Z)
inline Vector3 ForwardFromYaw(float yaw) { return {-sinf(yaw), 0.0f, -cosf(yaw)}; }
inline float YawTowards(Vector3 from, Vector3 to) { return atan2f(-(to.x - from.x), -(to.z - from.z)); }
float AngleDiff(float a, float b); // kleinster Winkel von a nach b (-PI..PI)
