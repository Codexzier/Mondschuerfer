#pragma once
#include "raylib.h"
#include <string>
#include <vector>

// Soundeffekte aus den Kenney-Paketen "Sci-Fi Sounds" und "Interface Sounds".
// Welche Datei zu welchem Ereignis gehört, steht in der Tabelle in Audio.cpp.
enum class Sfx {
    // Kampf
    PlayerShot, EnemyShot, TurretShot, HitMetal, ShieldHit, ShipExplode, CrashBoom,
    // Scanner
    ScanOn, ScanOff, ScanEmpty, ScanPing, Discover,
    // Abbau
    MineStart, MineDone, MineAbort, CargoFull,
    // Stationen
    Dock, Undock, Sale, Repair, Upgrade, StationGrow, Victory, Denied,
    // Warnung
    LowShield,
    // Menüs
    UiMove, UiConfirm, UiBack, PauseOpen, PauseClose,
    Count
};

// Ein Geräusch aus der Spiellogik. World sammelt sie, main.cpp spielt sie ab.
struct SoundEvent {
    Sfx id = Sfx::UiMove;
    Vector3 pos{};
    float volume = 1.0f;
    float delay = 0.0f;  // Sekunden
};

// Dauergeräusche, jedes Frame aus dem Spielzustand gesetzt (0 = aus, 1 = voll)
struct LoopState {
    float engine = 0.0f;
    float enginePitch = 1.0f;
    float thrust = 0.0f;
    float scanner = 0.0f;
    float mining = 0.0f;
    bool alarm = false;    // Schild kritisch
};

class Audio {
public:
    bool Init(const std::string& assetDir);  // nach Assets::Init() aufrufen
    void Shutdown();

    // Einmal pro Frame: verzögerte Effekte, Dauergeräusche, Warnton
    void Update(float dt, const LoopState& loops);

    // pan: 0 = rechts, 0.5 = Mitte, 1 = links (wie in raylib)
    void Play(Sfx id, float volume = 1.0f, float pan = 0.5f, float delay = 0.0f);
    // Lautstärke und Stereo-Position abhängig vom Abstand zum Zuhörer
    void PlayAt(const SoundEvent& e, Vector3 listener);

    void ToggleMute();
    bool Muted() const { return muted_; }

    enum Loop { LOOP_ENGINE, LOOP_THRUST, LOOP_SCANNER, LOOP_MINING, LOOP_COUNT };

private:
    struct Clip {
        std::vector<Sound> voices;  // [0] = Original, danach Aliase für Überlagerung
        int next = 0;
    };
    struct Effect {
        std::vector<Clip> clips;    // Varianten, zufällig gewählt
        int last = -1;
        double lastPlay = -1e9;
    };
    struct Track {
        Music music{};
        bool valid = false;
        bool started = false;
        bool playing = false;
        float volume = 0.0f;
    };
    struct Pending {
        Sfx id;
        float volume, pan, delay;
    };

    void Trigger(Sfx id, float volume, float pan);
    void UpdateLoop(int loop, float level, float pitch, float dt);

    Effect effects_[static_cast<int>(Sfx::Count)];
    Track loops_[LOOP_COUNT];
    std::vector<Pending> pending_;
    double clock_ = 0.0;
    float alarmT_ = 0.0f;
    bool ready_ = false;
    bool muted_ = false;
};

extern Audio g_audio;
