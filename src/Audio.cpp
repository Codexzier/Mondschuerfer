// Soundeffekte und Dauergeräusche (Kenney Sci-Fi Sounds und Interface Sounds).
#include "Audio.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>

Audio g_audio;

namespace {

constexpr int kVoices = 4;            // gleichzeitige Wiedergaben derselben Datei
constexpr float kHearNear = 15.0f;    // bis zu diesem Abstand volle Lautstärke
constexpr float kHearFar = 80.0f;     // ab hier nicht mehr hörbar
constexpr float kPanRange = 45.0f;    // seitlicher Abstand für die stärkste Stereo-Verschiebung
constexpr float kMaxPan = 0.35f;      // nie ganz auf eine Seite
constexpr float kAlarmInterval = 1.6f;

struct SfxDef {
    Sfx id;
    std::vector<const char*> files;  // relativ zu assets/sounds/, ohne ".ogg"
    float volume;
    float pitchVar;                  // zufällige Tonhöhenabweichung (+/-)
    float minGap;                    // Mindestabstand zwischen zwei Wiedergaben in Sekunden
};

// Zuordnung Ereignis -> Sounddatei(en). Reihenfolge wie in enum class Sfx.
// "Notlösung" = nichts wirklich Passendes in den beiden Paketen, nächstbester Ton.
const SfxDef kSfx[] = {
    {Sfx::PlayerShot, {"scifi/laserSmall_000", "scifi/laserSmall_001", "scifi/laserSmall_002", "scifi/laserSmall_003"}, 0.55f, 0.04f, 0.05f},
    {Sfx::EnemyShot, {"scifi/laserRetro_000", "scifi/laserRetro_001", "scifi/laserRetro_003"}, 0.22f, 0.06f, 0.04f},
    {Sfx::TurretShot, {"scifi/laserLarge_000", "scifi/laserLarge_001", "scifi/laserLarge_002", "scifi/laserLarge_003", "scifi/laserLarge_004"}, 0.40f, 0.05f, 0.08f},
    {Sfx::HitMetal, {"scifi/impactMetal_000", "scifi/impactMetal_001", "scifi/impactMetal_002", "scifi/impactMetal_003", "scifi/impactMetal_004"}, 0.90f, 0.08f, 0.04f},
    {Sfx::ShieldHit, {"scifi/forceField_000", "scifi/forceField_001", "scifi/forceField_002", "scifi/forceField_003", "scifi/forceField_004"}, 0.50f, 0.05f, 0.10f},
    {Sfx::ShipExplode, {"scifi/explosionCrunch_000", "scifi/explosionCrunch_001", "scifi/explosionCrunch_002", "scifi/explosionCrunch_003", "scifi/explosionCrunch_004"}, 0.85f, 0.06f, 0.05f},
    {Sfx::CrashBoom, {"scifi/lowFrequency_explosion_000", "scifi/lowFrequency_explosion_001"}, 1.00f, 0.05f, 0.05f},

    {Sfx::ScanOn, {"interface/maximize_006"}, 0.55f, 0.0f, 0.10f},
    {Sfx::ScanOff, {"interface/minimize_006"}, 0.55f, 0.0f, 0.10f},
    {Sfx::ScanEmpty, {"interface/minimize_005"}, 0.75f, 0.0f, 0.20f},
    {Sfx::ScanPing, {"interface/bong_001"}, 0.40f, 0.0f, 0.30f},          // Notlösung für einen Sonar-Ping
    {Sfx::Discover, {"interface/glass_001", "interface/glass_004"}, 0.90f, 0.03f, 0.20f},

    {Sfx::MineStart, {"interface/switch_003"}, 0.75f, 0.0f, 0.10f},
    {Sfx::MineDone, {"interface/confirmation_003"}, 0.75f, 0.0f, 0.10f},
    {Sfx::MineAbort, {"interface/close_002"}, 0.70f, 0.0f, 0.10f},
    {Sfx::CargoFull, {"interface/error_007"}, 0.55f, 0.0f, 0.50f},

    {Sfx::Dock, {"scifi/doorClose_001"}, 0.90f, 0.0f, 0.20f},
    {Sfx::Undock, {"scifi/doorOpen_001"}, 0.90f, 0.0f, 0.20f},
    {Sfx::Sale, {"interface/confirmation_001"}, 0.60f, 0.0f, 0.20f},      // Notlösung für Kasse/Credits
    {Sfx::Repair, {"interface/maximize_005"}, 0.80f, 0.0f, 0.20f},        // Notlösung für Schild aufladen
    {Sfx::Upgrade, {"interface/confirmation_002"}, 0.80f, 0.0f, 0.10f},
    {Sfx::StationGrow, {"interface/maximize_004"}, 0.85f, 0.0f, 0.20f},   // Notlösung für Bau-Fanfare
    {Sfx::Victory, {"interface/confirmation_004"}, 0.80f, 0.0f, 0.50f},   // Notlösung für Sieges-Fanfare
    {Sfx::Denied, {"interface/error_004", "interface/error_001"}, 0.75f, 0.0f, 0.10f},

    {Sfx::LowShield, {"interface/error_005"}, 0.45f, 0.0f, 0.50f},        // Notlösung für Warnsirene

    {Sfx::UiMove, {"interface/select_001", "interface/select_002"}, 0.60f, 0.0f, 0.03f},
    {Sfx::UiConfirm, {"interface/maximize_008"}, 0.55f, 0.0f, 0.10f},
    {Sfx::UiBack, {"interface/back_002"}, 0.80f, 0.0f, 0.10f},
    {Sfx::PauseOpen, {"interface/open_002"}, 0.75f, 0.0f, 0.10f},
    {Sfx::PauseClose, {"interface/close_003"}, 0.75f, 0.0f, 0.10f},
};
static_assert(sizeof(kSfx) / sizeof(kSfx[0]) == static_cast<size_t>(Sfx::Count),
              "Für jeden Eintrag in Sfx muss es eine Zeile in kSfx geben");

struct LoopDef {
    const char* file;
    float volume;
    float pitch;
};

const LoopDef kLoops[Audio::LOOP_COUNT] = {
    {"scifi/spaceEngineSmall_000", 0.22f, 1.0f},  // Triebwerk im Leerlauf
    {"scifi/thrusterFire_001", 0.50f, 1.0f},      // Schub (W)
    {"scifi/computerNoise_002", 0.10f, 1.0f},     // Scanner aktiv
    {"scifi/engineCircular_002", 0.30f, 1.2f},    // Abbau, Notlösung für Bergbau-Laser
};

} // namespace

bool Audio::Init(const std::string& assetDir) {
    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        TraceLog(LOG_WARNING, "Kein Audiogerät gefunden, das Spiel läuft ohne Ton");
        return false;
    }
    const std::string dir = assetDir + "sounds/";

    for (int i = 0; i < static_cast<int>(Sfx::Count); ++i) {
        const SfxDef& def = kSfx[i];
        if (static_cast<int>(def.id) != i) {
            TraceLog(LOG_WARNING, "Sound-Tabelle: Zeile %d passt nicht zur Reihenfolge von Sfx", i);
            continue;
        }
        for (const char* f : def.files) {
            const std::string path = dir + f + ".ogg";
            Sound s = LoadSound(path.c_str());
            if (!IsSoundValid(s)) {
                TraceLog(LOG_WARNING, "Sound konnte nicht geladen werden: %s", path.c_str());
                continue;
            }
            Clip c;
            c.voices.push_back(s);
            for (int v = 1; v < kVoices; ++v) c.voices.push_back(LoadSoundAlias(s));
            effects_[i].clips.push_back(c);
        }
    }

    for (int i = 0; i < LOOP_COUNT; ++i) {
        const std::string path = dir + kLoops[i].file + ".ogg";
        loops_[i].music = LoadMusicStream(path.c_str());
        loops_[i].valid = IsMusicValid(loops_[i].music);
        if (loops_[i].valid) loops_[i].music.looping = true;
        else TraceLog(LOG_WARNING, "Sound konnte nicht geladen werden: %s", path.c_str());
    }
    ready_ = true;
    return true;
}

void Audio::Shutdown() {
    for (auto& fx : effects_) {
        for (auto& c : fx.clips) {
            for (size_t v = 1; v < c.voices.size(); ++v) UnloadSoundAlias(c.voices[v]);
            UnloadSound(c.voices[0]);
        }
        fx.clips.clear();
    }
    for (auto& t : loops_) {
        if (t.valid) UnloadMusicStream(t.music);
        t = Track{};
    }
    pending_.clear();
    if (IsAudioDeviceReady()) CloseAudioDevice();
    ready_ = false;
}

void Audio::Play(Sfx id, float volume, float pan, float delay) {
    if (!ready_) return;
    if (delay > 0.0f) {
        pending_.push_back({id, volume, pan, delay});
        return;
    }
    Trigger(id, volume, pan);
}

void Audio::PlayAt(const SoundEvent& e, Vector3 listener) {
    float dx = e.pos.x - listener.x;
    float dz = e.pos.z - listener.z;
    float d = sqrtf(dx * dx + dz * dz);
    float fade = 1.0f - Clamp((d - kHearNear) / (kHearFar - kHearNear), 0.0f, 1.0f);
    fade *= fade;
    if (fade <= 0.01f) return;
    // Quelle rechts vom Zuhörer (dx > 0) -> pan kleiner als 0.5
    float pan = 0.5f - Clamp(dx / kPanRange, -1.0f, 1.0f) * kMaxPan;
    Play(e.id, e.volume * fade, pan, e.delay);
}

void Audio::Trigger(Sfx id, float volume, float pan) {
    const int idx = static_cast<int>(id);
    Effect& fx = effects_[idx];
    const SfxDef& def = kSfx[idx];
    if (fx.clips.empty() || volume <= 0.01f) return;
    if (clock_ - fx.lastPlay < def.minGap) return;
    fx.lastPlay = clock_;

    const int n = static_cast<int>(fx.clips.size());
    int pick = GetRandomValue(0, n - 1);
    if (n > 1 && pick == fx.last) pick = (pick + 1) % n;
    fx.last = pick;

    Clip& c = fx.clips[pick];
    Sound& s = c.voices[c.next];
    c.next = (c.next + 1) % static_cast<int>(c.voices.size());
    float pitch = 1.0f;
    if (def.pitchVar > 0.0f) pitch += def.pitchVar * static_cast<float>(GetRandomValue(-100, 100)) / 100.0f;
    SetSoundVolume(s, std::min(1.0f, def.volume * volume));
    SetSoundPitch(s, pitch);
    SetSoundPan(s, pan);
    PlaySound(s);
}

void Audio::Update(float dt, const LoopState& st) {
    if (!ready_) return;
    clock_ += dt;

    for (auto& p : pending_) p.delay -= dt;
    for (const auto& p : pending_) if (p.delay <= 0.0f) Trigger(p.id, p.volume, p.pan);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.delay <= 0.0f; }),
                   pending_.end());

    UpdateLoop(LOOP_ENGINE, st.engine, st.enginePitch, dt);
    UpdateLoop(LOOP_THRUST, st.thrust, st.enginePitch, dt);
    UpdateLoop(LOOP_SCANNER, st.scanner, 1.0f, dt);
    UpdateLoop(LOOP_MINING, st.mining, 1.0f, dt);

    if (st.alarm) {
        alarmT_ -= dt;
        if (alarmT_ <= 0.0f) {
            Trigger(Sfx::LowShield, 1.0f, 0.5f);
            alarmT_ = kAlarmInterval;
        }
    } else {
        alarmT_ = 0.0f;
    }
}

void Audio::UpdateLoop(int loop, float level, float pitch, float dt) {
    Track& t = loops_[loop];
    if (!t.valid) return;
    float target = Clamp(level, 0.0f, 1.0f) * kLoops[loop].volume;
    // Sanft ein- und ausblenden, damit es nicht knackt
    t.volume += (target - t.volume) * std::min(1.0f, dt * 8.0f);
    if (target <= 0.0f && t.volume < 0.002f) t.volume = 0.0f;

    if (t.volume > 0.0f) {
        if (!t.started) {
            PlayMusicStream(t.music);
            t.started = true;
            t.playing = true;
        } else if (!t.playing) {
            ResumeMusicStream(t.music);
            t.playing = true;
        }
        SetMusicVolume(t.music, t.volume);
        SetMusicPitch(t.music, pitch * kLoops[loop].pitch);
        UpdateMusicStream(t.music);
    } else if (t.playing) {
        PauseMusicStream(t.music);
        t.playing = false;
    }
}

void Audio::ToggleMute() {
    muted_ = !muted_;
    if (ready_) SetMasterVolume(muted_ ? 0.0f : 1.0f);
}
