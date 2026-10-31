// ---------------------------------------------------------------------------
// Audio.cpp – Soundeffekte, Dauergeräusche und Musik (Schnittstelle: Audio.hpp).
//
// Wofür ist die Datei da?
//   Hier steht, welche Sounddatei zu welchem Spielereignis gehört (Tabellen
//   kSfx und kLoops) und welche Musik wann läuft (kSongs), und wie die Töne
//   geladen, abgespielt, im Raum verteilt und überblendet werden. Benutzt
//   wird alles über das globale Objekt g_audio (main.cpp, Multiplayer.cpp,
//   Options.cpp, Ui.cpp, Hud.cpp; siehe Audio.hpp).
//
// Abhängigkeiten:
//   - raylib (Audio-Funktionen), raymath.h (Clamp)
//   - shared/SoundEvents.hpp (über Audio.hpp): enum class Sfx, SoundEvent
//   - src/MusicMix.hpp (über Audio.hpp): Überblend-Rechnung der Musik
//   - assets/sounds/scifi/*.ogg, assets/sounds/interface/*.ogg und
//     assets/music/*.mp3
//
// raylib-Audio in Kürze (so wie es hier benutzt wird):
//   - InitAudioDevice() öffnet die Standard-Tonausgabe des Systems. Ab dann
//     mischt raylib alle laufenden Töne in einem eigenen Audio-Thread.
//   - Sound (LoadSound): Die Datei wird beim Laden komplett entpackt und liegt
//     im Arbeitsspeicher. Gut für kurze Effekte, die sofort starten müssen.
//     Ein Sound kann nur einmal gleichzeitig laufen; deshalb gibt es
//     Sound-Aliase (LoadSoundAlias): zusätzliche Abspielköpfe für dieselben
//     Daten, damit sich gleiche Effekte überlagern können.
//   - Music (LoadMusicStream): Die Datei wird nicht ganz geladen, sondern
//     beim Abspielen stückweise entpackt ("Streaming"). Anders als Sound kann
//     Music endlos wiederholen (music.looping) – deshalb sind die
//     Dauergeräusche Music-Streams. Preis dafür: UpdateMusicStream() muss
//     regelmäßig (jedes Frame) aufgerufen werden, um den Puffer nachzufüllen,
//     sonst stockt der Ton.
//   - Lautstärke 0..1, Tonhöhe ("pitch", 1 = Original; raylib spielt dafür
//     einfach schneller oder langsamer ab, 1.1 ist also 10 % höher UND
//     entsprechend kürzer), Stereo-Position ("pan", 0..1, 0,5 = Mitte).
//   - SetMasterVolume(): Gesamtlautstärke über alles (für Ton aus, Taste M).
//
// Ablauf eines Effekts:
//   World::SoundAt (Spiellogik) -> SoundEvent in world.sounds
//   -> main.cpp/Multiplayer.cpp: g_audio.PlayAt(e, Kameraziel)
//   -> PlayAt: Lautstärke/pan aus dem Abstand -> Play
//   -> Play: mit Verzögerung in die Warteliste pending_, sonst sofort Trigger
//   -> Trigger: Mindestabstand prüfen, Variante und Abspielkopf wählen,
//      Tonhöhe leicht zufällig verändern, PlaySound.
// Dauergeräusche: Update -> UpdateLoop je Schleife (weich überblenden,
// starten/pausieren, Stream nachfüllen).
// Musik: main.cpp/Multiplayer.cpp wählen mit SetMusicTrack die Musik (und
// stoßen mit PlayGoalMusic die Zielmusik an) -> Update -> UpdateMusic:
// WantedSong wählt das Stück, MusicMix rechnet die Blenden weiter, danach
// wird jedes Stück gestartet/fortgesetzt, in der Lautstärke gesetzt,
// nachgefüllt oder (bei Lautstärke 0) pausiert – wie die Dauergeräusche.
// ---------------------------------------------------------------------------
#include "Audio.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

Audio g_audio;

namespace {

// ===========================================================================
// Einstellungen (Entfernungen in Welt-Einheiten, wie Schiffspositionen)
// ===========================================================================
// Zum Vergleich: Bei normalem Zoom zeigt das Bild etwa 30–35 Einheiten links
// und rechts der Bildmitte. Effekte am Bildrand sind also noch gut halb so
// laut, und Kämpfe knapp außerhalb des Bildes hört man leise. Der Server
// schickt Ereignisse bis 120 Einheiten (net::AOI_EVENTS), also mehr als hier
// hörbar ist – am Rand wird nichts abgeschnitten.
constexpr int kVoices = 4;            // gleichzeitige Wiedergaben derselben Datei (1 Original + 3 Aliase)
constexpr float kHearNear = 15.0f;    // bis zu diesem Abstand volle Lautstärke
constexpr float kHearFar = 80.0f;     // ab hier nicht mehr hörbar (praktisch schon ab ca. 74, siehe PlayAt)
constexpr float kPanRange = 45.0f;    // seitlicher Abstand für die stärkste Stereo-Verschiebung
constexpr float kMaxPan = 0.35f;      // nie ganz auf eine Seite (pan bleibt zwischen 0,15 und 0,85),
                                      // ein Ton nur auf einem Ohr klingt im Kopfhörer unangenehm
constexpr float kAlarmInterval = 1.6f; // Sekunden zwischen zwei Warntönen bei kritischem Schild

// ===========================================================================
// Tabelle: Ereignis -> Sounddatei(en)
// ===========================================================================
struct SfxDef {
    Sfx id;
    std::vector<const char*> files;  // relativ zu assets/sounds/, ohne ".ogg"; mehrere = Varianten
    float volume;                    // Grundlautstärke (gleicht die unterschiedlich lauten Dateien aus)
    float pitchVar;                  // zufällige Tonhöhenabweichung (+/-), 0.04 = bis zu 4 % höher oder tiefer
    float minGap;                    // Mindestabstand zwischen zwei Wiedergaben in Sekunden
};

// Zuordnung Ereignis -> Sounddatei(en). Reihenfolge wie in enum class Sfx,
// denn der Index in dieser Tabelle ist die Sfx-Nummer (Init prüft das).
// "Notlösung" = nichts wirklich Passendes in den beiden Paketen, nächstbester Ton.
//
// Zu den Zahlen:
//   - volume: nach Gehör abgestimmt. Häufige Töne sind leiser (Gegnerschüsse
//     0.22, weil viele Piraten gleichzeitig schießen), wichtige Rückmeldungen
//     lauter (Treffer, Explosionen, Entdeckung).
//   - pitchVar: nur bei Kampfgeräuschen, die sehr oft kommen. Die kleine
//     zufällige Tonhöhe verhindert, dass zehn gleiche Schüsse wie ein
//     Maschinengewehr aus einer Tonbandschleife klingen. Menütöne bleiben
//     immer gleich (Wiedererkennung).
//   - minGap: verhindert, dass derselbe Effekt mehrfach im selben Moment
//     startet (z. B. fünf Treffer im gleichen Frame) – das klänge nur lauter
//     und verzerrt, nicht "mehr". Warn- und Fanfarentöne (CargoFull,
//     LowShield, Victory) haben 0,5 s Abstand, damit sie nicht nerven.
const SfxDef kSfx[] = {
    // Kampf
    {Sfx::PlayerShot, {"scifi/laserSmall_000", "scifi/laserSmall_001", "scifi/laserSmall_002", "scifi/laserSmall_003"}, 0.55f, 0.04f, 0.05f},
    {Sfx::EnemyShot, {"scifi/laserRetro_000", "scifi/laserRetro_001", "scifi/laserRetro_003"}, 0.22f, 0.06f, 0.04f},
    {Sfx::TurretShot, {"scifi/laserLarge_000", "scifi/laserLarge_001", "scifi/laserLarge_002", "scifi/laserLarge_003", "scifi/laserLarge_004"}, 0.40f, 0.05f, 0.08f},
    {Sfx::HitMetal, {"scifi/impactMetal_000", "scifi/impactMetal_001", "scifi/impactMetal_002", "scifi/impactMetal_003", "scifi/impactMetal_004"}, 0.90f, 0.08f, 0.04f},
    {Sfx::ShieldHit, {"scifi/forceField_000", "scifi/forceField_001", "scifi/forceField_002", "scifi/forceField_003", "scifi/forceField_004"}, 0.50f, 0.05f, 0.10f},
    {Sfx::ShipExplode, {"scifi/explosionCrunch_000", "scifi/explosionCrunch_001", "scifi/explosionCrunch_002", "scifi/explosionCrunch_003", "scifi/explosionCrunch_004"}, 0.85f, 0.06f, 0.05f},
    {Sfx::CrashBoom, {"scifi/lowFrequency_explosion_000", "scifi/lowFrequency_explosion_001"}, 1.00f, 0.05f, 0.05f},

    // Scanner
    {Sfx::ScanOn, {"interface/maximize_006"}, 0.55f, 0.0f, 0.10f},
    {Sfx::ScanOff, {"interface/minimize_006"}, 0.55f, 0.0f, 0.10f},
    {Sfx::ScanEmpty, {"interface/minimize_005"}, 0.75f, 0.0f, 0.20f},
    {Sfx::ScanPing, {"interface/bong_001"}, 0.40f, 0.0f, 0.30f},          // Notlösung für einen Sonar-Ping
    {Sfx::Discover, {"interface/glass_001", "interface/glass_004"}, 0.90f, 0.03f, 0.20f},

    // Abbau
    {Sfx::MineStart, {"interface/switch_003"}, 0.75f, 0.0f, 0.10f},
    {Sfx::MineDone, {"interface/confirmation_003"}, 0.75f, 0.0f, 0.10f},
    {Sfx::MineAbort, {"interface/close_002"}, 0.70f, 0.0f, 0.10f},
    {Sfx::CargoFull, {"interface/error_007"}, 0.55f, 0.0f, 0.50f},

    // Stationen
    {Sfx::Dock, {"scifi/doorClose_001"}, 0.90f, 0.0f, 0.20f},
    {Sfx::Undock, {"scifi/doorOpen_001"}, 0.90f, 0.0f, 0.20f},
    {Sfx::Sale, {"interface/confirmation_001"}, 0.60f, 0.0f, 0.20f},      // Notlösung für Kasse/Credits
    {Sfx::Repair, {"interface/maximize_005"}, 0.80f, 0.0f, 0.20f},        // Notlösung für Schild aufladen
    {Sfx::Upgrade, {"interface/confirmation_002"}, 0.80f, 0.0f, 0.10f},
    {Sfx::StationGrow, {"interface/maximize_004"}, 0.85f, 0.0f, 0.20f},   // Notlösung für Bau-Fanfare
    {Sfx::Victory, {"interface/confirmation_004"}, 0.80f, 0.0f, 0.50f},   // Notlösung für Sieges-Fanfare
    {Sfx::Denied, {"interface/error_004", "interface/error_001"}, 0.75f, 0.0f, 0.10f},

    // Warnung (wiederholt von Update(), solange LoopState::alarm gesetzt ist)
    {Sfx::LowShield, {"interface/error_005"}, 0.45f, 0.0f, 0.50f},        // Notlösung für Warnsirene

    // Menüs
    {Sfx::UiMove, {"interface/select_001", "interface/select_002"}, 0.60f, 0.0f, 0.03f},
    {Sfx::UiConfirm, {"interface/maximize_008"}, 0.55f, 0.0f, 0.10f},
    {Sfx::UiBack, {"interface/back_002"}, 0.80f, 0.0f, 0.10f},
    {Sfx::PauseOpen, {"interface/open_002"}, 0.75f, 0.0f, 0.10f},
    {Sfx::PauseClose, {"interface/close_003"}, 0.75f, 0.0f, 0.10f},
};
// Prüfung beim Übersetzen: Kommt in Sfx ein Eintrag dazu, ohne dass hier eine
// Zeile ergänzt wird, bricht der Build ab (statt später falsche Töne zu spielen).
static_assert(sizeof(kSfx) / sizeof(kSfx[0]) == static_cast<size_t>(Sfx::Count),
              "Für jeden Eintrag in Sfx muss es eine Zeile in kSfx geben");

// ===========================================================================
// Tabelle: Dauergeräusche (Index = Audio::Loop)
// ===========================================================================
struct LoopDef {
    const char* file;
    float volume;   // Lautstärke bei LoopState-Wert 1
    float pitch;    // Grund-Tonhöhe, wird mit der Tonhöhe aus LoopState multipliziert
};

// Triebwerk und Scanner sind bewusst leise: Sie laufen ständig und sollen
// die Effekte nicht übertönen. Der Schub ist lauter, weil er nur beim
// Gasgeben zu hören ist und dabei Rückmeldung geben soll.
const LoopDef kLoops[Audio::LOOP_COUNT] = {
    {"scifi/spaceEngineSmall_000", 0.22f, 1.0f},  // Triebwerk im Leerlauf
    {"scifi/thrusterFire_001", 0.50f, 1.0f},      // Schub (W)
    {"scifi/computerNoise_002", 0.10f, 1.0f},     // Scanner aktiv
    {"scifi/engineCircular_002", 0.30f, 1.2f},    // Abbau, Notlösung für Bergbau-Laser (höher = mehr "Bohrer")
};

// ===========================================================================
// Tabelle: Musikstücke (Index = enum Song)
// ===========================================================================
// Alle Stücke stammen von dekker187 (erstellt mit Suno) und wurden vom
// Projektinhaber für das Spiel bereitgestellt (MP3, je einige Minuten).
// Warum Music (Streaming) und nicht Sound? Ein ganzes Stück entpackt wären
// über 70 MB Sampledaten pro Datei (197 s * 48000 Abtastwerte/s * 2 Kanäle *
// 4 Byte); als Stream liegen immer nur wenige Kilobyte im Speicher.
//   loop     Endlosschleife. Sonst läuft das Stück einmal bis zum Ende, dann
//            geht es mit dem nächsten weiter (siehe Audio::UpdateMusic).
//   restart  Beim nächsten Mal von vorn statt an derselben Stelle weiter
//            (Situationsmusik: jede Piraten-Mission beginnt mit dem Anfang
//            des Stücks).
// Die Spielmusik (SONG_GAME_1..3) läuft der Reihe nach im Kreis.
enum Song { SONG_MENU, SONG_GAME_1, SONG_GAME_2, SONG_GAME_3, SONG_PIRATES, SONG_GOAL, SONG_STARGATE, SONG_COUNT };
constexpr int kPlaylist = SONG_GAME_3 - SONG_GAME_1 + 1;   // Stücke der Spielmusik

struct SongDef {
    const char* file;
    bool loop;
    bool restart;
};
const SongDef kSongs[] = {
    {"music/main_menu_theme.mp3", true, false},           // Menu: "Main Menu Theme"
    {"music/lunar_silence_01.mp3", false, false},         // Game: "Lunar Silence 01"
    {"music/lunar_silence_02.mp3", false, false},         // Game: "Lunar Silence 02"
    {"music/lunar_silence_03.mp3", false, false},         // Game: "Lunar Silence 03"
    {"music/enteringPirateTerritory.mp3", true, true},    // Pirates: "Entering Pirate Territory"
    {"music/deepSpaceHorizon.mp3", false, true},          // Ziel erreicht: "Deep Space Horizon"
    {"music/breathingPulse.mp3", true, true},             // Stargate: "Breathing Pulse"
};
static_assert(sizeof(kSongs) / sizeof(kSongs[0]) == SONG_COUNT, "kSongs passt nicht zu enum Song");
static_assert(SONG_COUNT == MusicMix::kTracks, "MusicMix::kTracks passt nicht zu kSongs");

} // namespace

// ===========================================================================
// Laden und Freigeben
// ===========================================================================

// Audiogerät öffnen und alle Dateien aus den Tabellen laden. Pro Datei
// entstehen kVoices Abspielköpfe (Original + Aliase, siehe Audio::Clip).
// Fehlt eine Datei, fehlt nur diese Variante; hat ein Effekt gar keine
// Datei, bleibt er stumm (Trigger prüft clips.empty()).
bool Audio::Init(const std::string& assetDir) {
    InitAudioDevice();
    // Ohne Tonausgabe (z. B. Rechner ohne Soundkarte) bleibt
    // ready_ false: Play/Update/... kehren dann sofort zurück.
    if (!IsAudioDeviceReady()) {
        TraceLog(LOG_WARNING, "Kein Audiogerät gefunden, das Spiel läuft ohne Ton");
        return false;
    }
    const std::string dir = assetDir + "sounds/";

    for (int i = 0; i < static_cast<int>(Sfx::Count); ++i) {
        const SfxDef& def = kSfx[i];
        // Die Anzahl der Zeilen prüft schon static_assert, die Reihenfolge
        // nicht. Eine vertauschte Zeile würde sonst still den falschen Ton
        // zu einem Ereignis spielen.
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
        // looping ist bei raylib schon voreingestellt; hier ausdrücklich,
        // weil die Dauergeräusche ohne Wiederholung nach ein paar Sekunden
        // einfach aufhören würden.
        if (loops_[i].valid) loops_[i].music.looping = true;
        else TraceLog(LOG_WARNING, "Sound konnte nicht geladen werden: %s", path.c_str());
    }

    // Musik: wie die Dauergeräusche als Stream, mit Endlosschleife nur laut
    // kSongs. Fehlt eine Datei, bleibt nur dieses Stück stumm (UpdateMusic
    // prüft valid).
    // LoadMusicStream liest bei MP3 die Datei einmal kurz durch, um ihre Länge
    // zu bestimmen – das dauert einen Augenblick, passiert aber nur beim Start.
    for (int i = 0; i < MusicMix::kTracks; ++i) {
        const std::string path = assetDir + kSongs[i].file;
        songs_[i].music = LoadMusicStream(path.c_str());
        songs_[i].valid = IsMusicValid(songs_[i].music);
        if (songs_[i].valid) songs_[i].music.looping = kSongs[i].loop;
        else TraceLog(LOG_WARNING, "Musik konnte nicht geladen werden: %s", path.c_str());
    }
    ready_ = true;
    return true;
}

// Reihenfolge ist wichtig: Erst die Aliase freigeben, dann das Original –
// die Aliase benutzen die Sampledaten des Originals mit und dürfen nicht
// länger leben als dieses. Danach erst das Audiogerät schließen.
// "t = Track{}" setzt auch valid zurück, damit ein zweiter Aufruf nichts
// doppelt freigibt.
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
    for (auto& t : songs_) {
        if (t.valid) UnloadMusicStream(t.music);
        t = Track{};
    }
    pending_.clear();
    if (IsAudioDeviceReady()) CloseAudioDevice();
    ready_ = false;
}

// ===========================================================================
// Effekte abspielen
// ===========================================================================

// Verzögerte Effekte kommen in die Warteliste pending_ und werden in Update()
// heruntergezählt. Kein Timer-Thread: Alles bleibt im Haupt-Thread, und die
// Wartezeit läuft mit der Spielzeit (dt) mit. Verzögerungen setzt die
// Spiellogik, um Töne nacheinander erklingen zu lassen (z. B. beim Andocken:
// Tür, dann Kasse, dann Ausbau-Fanfare, siehe World::Dock).
void Audio::Play(Sfx id, float volume, float pan, float delay) {
    if (!ready_) return;
    if (delay > 0.0f) {
        pending_.push_back({id, volume, pan, delay});
        return;
    }
    Trigger(id, volume, pan);
}

// Einfaches "Raumklang"-Modell, nur in der Ebene (x/z; die Höhe y spielt
// keine Rolle, alle Schiffe fliegen gleich hoch):
//   - Lautstärke: bis kHearNear voll, dann bis kHearFar auf 0. Der Verlauf
//     wird quadriert, damit es nach außen schneller leiser wird als linear;
//     das klingt natürlicher. Unter 1 % Lautstärke wird gar nicht erst
//     abgespielt – wegen des Quadrats ist das schon ab ca. 74 Einheiten so.
//   - Stereo: nur der seitliche Abstand dx zählt. Das funktioniert, weil die
//     Kamera im Spiel sich nie dreht: Sie schaut immer in Richtung -z, +x ist
//     also immer "rechts auf dem Bildschirm" (main.cpp und Multiplayer.cpp;
//     die kreisende Kamera im Hauptmenü ist stumm).
void Audio::PlayAt(const SoundEvent& e, Vector3 listener) {
    float dx = e.pos.x - listener.x;
    float dz = e.pos.z - listener.z;
    float d = sqrtf(dx * dx + dz * dz);
    float fade = 1.0f - Clamp((d - kHearNear) / (kHearFar - kHearNear), 0.0f, 1.0f);
    fade *= fade;
    if (fade <= 0.01f) return;
    // Quelle rechts vom Zuhörer (dx > 0) -> pan kleiner als 0.5
    // (raylib 5.5: kleinerer pan-Wert = mehr auf dem rechten Lautsprecher)
    float pan = 0.5f - Clamp(dx / kPanRange, -1.0f, 1.0f) * kMaxPan;
    Play(e.id, e.volume * fade, pan, e.delay);
}

// Die eigentliche Wiedergabe. Wird von Play() (sofort) und Update()
// (verzögert, Warnton) aufgerufen; ready_ ist hier immer true.
void Audio::Trigger(Sfx id, float volume, float pan) {
    const int idx = static_cast<int>(id);
    Effect& fx = effects_[idx];
    const SfxDef& def = kSfx[idx];
    if (fx.clips.empty() || volume <= 0.01f) return;
    // Mindestabstand: clock_ läuft nur in Update() weiter. Alle Aufrufe im
    // selben Frame sehen also dieselbe Zeit – von mehreren gleichen Effekten
    // in einem Frame kommt nur der erste durch.
    if (clock_ - fx.lastPlay < def.minGap) return;
    fx.lastPlay = clock_;

    // Variante zufällig wählen, aber nie dieselbe wie beim letzten Mal
    // (trifft der Zufall die letzte, wird die nächste genommen). Zwei identische Töne
    // direkt hintereinander fallen dem Ohr sofort auf.
    const int n = static_cast<int>(fx.clips.size());
    int pick = GetRandomValue(0, n - 1);
    if (n > 1 && pick == fx.last) pick = (pick + 1) % n;
    fx.last = pick;

    // Abspielkopf reihum: Sind alle kVoices belegt, wird der älteste neu
    // gestartet (er ist am ehesten schon fast ausgeklungen).
    Clip& c = fx.clips[pick];
    Sound& s = c.voices[c.next];
    c.next = (c.next + 1) % static_cast<int>(c.voices.size());
    // Zufällige Tonhöhe 1 +/- pitchVar, in 1-%-Schritten von pitchVar
    // (GetRandomValue liefert nur ganze Zahlen).
    float pitch = 1.0f;
    if (def.pitchVar > 0.0f) pitch += def.pitchVar * static_cast<float>(GetRandomValue(-100, 100)) / 100.0f;
    // Lautstärke, Tonhöhe und pan gehören zum Abspielkopf, nicht zur Datei –
    // jeder Alias kann also anders klingen. 1 ist die Höchstlautstärke.
    SetSoundVolume(s, std::min(1.0f, def.volume * volume));
    SetSoundPitch(s, pitch);
    SetSoundPan(s, pan);
    PlaySound(s);
}

// ===========================================================================
// Jedes Frame: Warteliste, Dauergeräusche, Warnton
// ===========================================================================
void Audio::Update(float dt, const LoopState& st) {
    if (!ready_) return;
    clock_ += dt;

    // Verzögerte Effekte: herunterzählen, fällige abspielen, dann entfernen.
    // remove_if schiebt die zu behaltenden Einträge nach vorn und gibt das
    // neue Ende zurück; erase schneidet den Rest ab ("Erase-Remove-Idiom").
    for (auto& p : pending_) p.delay -= dt;
    for (const auto& p : pending_) if (p.delay <= 0.0f) Trigger(p.id, p.volume, p.pan);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.delay <= 0.0f; }),
                   pending_.end());

    // Triebwerk und Schub folgen beide der Geschwindigkeit (enginePitch),
    // Scanner und Abbau klingen immer gleich.
    UpdateLoop(LOOP_ENGINE, st.engine, st.enginePitch, dt);
    UpdateLoop(LOOP_THRUST, st.thrust, st.enginePitch, dt);
    UpdateLoop(LOOP_SCANNER, st.scanner, 1.0f, dt);
    UpdateLoop(LOOP_MINING, st.mining, 1.0f, dt);

    UpdateMusic(dt);

    // Warnton bei kritischem Schild alle kAlarmInterval Sekunden. Endet der
    // Alarm, wird der Zähler auf 0 gesetzt: Beim nächsten Alarm piept es
    // sofort und nicht erst nach einer Restwartezeit.
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

// Ein Dauergeräusch auf die gewünschte Lautstärke bringen.
//   level: 0..1 aus LoopState, pitch: Tonhöhe aus LoopState
// Der Stream läuft nur, solange er hörbar ist. Bei Lautstärke 0 wird er
// pausiert (nicht gestoppt): Beim Weiterlaufen geht es an derselben Stelle
// der Schleife weiter, und ein stummer Stream kostet keine Rechenzeit zum
// Entpacken.
void Audio::UpdateLoop(int loop, float level, float pitch, float dt) {
    Track& t = loops_[loop];
    if (!t.valid) return;
    float target = Clamp(level, 0.0f, 1.0f) * kLoops[loop].volume;
    // Sanft ein- und ausblenden, damit es nicht knackt: Jedes Frame wird der
    // Anteil dt * 8 der Restdifferenz aufgeholt (exponentielles Annähern mit
    // ca. 1/8 s Zeitkonstante, nach ca. 0,3 s sind rund 90 % erreicht).
    // Ein harter Sprung der Lautstärke wäre als Klicken zu hören.
    t.volume += (target - t.volume) * std::min(1.0f, dt * 8.0f);
    // Exponentielles Annähern erreicht 0 nie ganz; unter der Hörschwelle
    // auf 0 setzen, damit der Stream unten pausiert werden kann.
    if (target <= 0.0f && t.volume < 0.002f) t.volume = 0.0f;

    if (t.volume > 0.0f) {
        // Erster Start mit PlayMusicStream (beginnt am Dateianfang), danach
        // nur noch fortsetzen.
        if (!t.started) {
            PlayMusicStream(t.music);
            t.started = true;
            t.playing = true;
        } else if (!t.playing) {
            ResumeMusicStream(t.music);
            t.playing = true;
        }
        // "::" = die raylib-Funktion SetMusicVolume(Music, float). Ohne "::"
        // würde der Übersetzer innerhalb der Klasse Audio nur die
        // gleichnamige Methode Audio::SetMusicVolume(float) (Regler in den
        // Optionen) finden – sie verdeckt die raylib-Funktion.
        ::SetMusicVolume(t.music, t.volume);
        SetMusicPitch(t.music, pitch * kLoops[loop].pitch);
        // Puffer mit frisch entpackten Daten nachfüllen (siehe Dateikopf)
        UpdateMusicStream(t.music);
    } else if (t.playing) {
        PauseMusicStream(t.music);
        t.playing = false;
    }
}

// ===========================================================================
// Musik
// ===========================================================================
// Welches Stück soll laufen? Vorrang von oben nach unten:
//   - Menu: Menümusik, None: Stille (beide beenden eine laufende Zielmusik,
//     ebenso "Musik aus" in den Optionen)
//   - Zielmusik, solange angestoßen (PlayGoalMusic) und nicht zu Ende
//   - Pirates: Piratenmusik, Stargate: Musik des SG-Teams
//   - sonst das aktuelle Stück der Spielmusik
// Fehlt die Datei der Ziel-, Piraten- bzw. Stargate-Musik, gilt die nächste Stufe;
// fehlende Stücke der Spielmusik werden übersprungen.
int Audio::WantedSong() {
    if (track_ == MusicTrack::None || track_ == MusicTrack::Menu || !mix_.enabled) goalMusic_ = false;
    if (track_ == MusicTrack::None) return -1;
    if (track_ == MusicTrack::Menu) return SONG_MENU;
    if (goalMusic_ && songs_[SONG_GOAL].valid) return SONG_GOAL;
    goalMusic_ = false;
    if (track_ == MusicTrack::Pirates && songs_[SONG_PIRATES].valid) return SONG_PIRATES;
    if (track_ == MusicTrack::Stargate && songs_[SONG_STARGATE].valid) return SONG_STARGATE;
    for (int n = 0; n < kPlaylist && !songs_[SONG_GAME_1 + playlist_].valid; ++n) playlist_ = (playlist_ + 1) % kPlaylist;
    return SONG_GAME_1 + playlist_;
}

// Ablauf pro Frame:
//   1. WantedSong() wählt das Stück, mix_.Step(dt) rechnet die Blenden
//      weiter (gewünschtes Stück lauter, alle anderen leiser; Musik aus =
//      alle leiser). Die Rechnung steht in MusicMix.hpp, damit sie ohne
//      Audiogerät testbar ist.
//   2. Für jedes Stück die fertige Lautstärke (Gain = Blende * Regler *
//      Grundpegel) übernehmen:
//        > 0: beim ersten Mal starten (PlayMusicStream, beginnt am Anfang),
//             sonst ggf. fortsetzen (ResumeMusicStream, an derselben Stelle),
//             dann den Puffer nachfüllen (UpdateMusicStream, jedes Frame
//             nötig, siehe Dateikopf).
//        = 0: pausieren. Ein stummes Stück kostet so keine Rechenzeit, und
//             wer aus dem Spiel kurz ins Menü und zurück geht, hört das
//             Spielstück dort weiter, wo es aufgehört hat. Situationsmusik
//             (restart in kSongs) wird stattdessen angehalten und
//             zurückgespult, damit sie beim nächsten Mal von vorn beginnt.
//   3. Stück ohne Endlosschleife zu Ende: raylib hat es selbst angehalten
//      und zurückgespult (IsMusicStreamPlaying ist dann false). Nach einem
//      Stück der Spielmusik folgt ohne Einblenden das nächste (nach dem
//      letzten wieder das erste), nach der Zielmusik blendet die Spiel- bzw.
//      Piratenmusik wieder ein.
// Die Lautstärke wird VOR dem Starten/Fortsetzen gesetzt: Sonst könnte der
// Mischer im Audio-Thread den ersten Pufferrest noch mit der alten
// Lautstärke abspielen (ein kurzes, lautes Knacken).
void Audio::UpdateMusic(float dt) {
    mix_.wanted = WantedSong();
    mix_.Step(dt);
    for (int i = 0; i < MusicMix::kTracks; ++i) {
        Track& t = songs_[i];
        if (!t.valid) continue;
        t.volume = mix_.Gain(i);
        if (t.volume > 0.0f) {
            ::SetMusicVolume(t.music, t.volume);   // raylib-Funktion, "::" siehe UpdateLoop
            if (!t.started) {
                PlayMusicStream(t.music);
                t.started = true;
                t.playing = true;
            } else if (!t.playing) {
                ResumeMusicStream(t.music);
                t.playing = true;
            }
            UpdateMusicStream(t.music);
            if (!kSongs[i].loop && !IsMusicStreamPlaying(t.music)) {
                const float level = mix_.fade[i];
                mix_.fade[i] = 0.0f;
                t.started = false;
                t.playing = false;
                t.volume = 0.0f;
                if (i == SONG_GOAL) {
                    goalMusic_ = false;
                } else if (i == SONG_GAME_1 + playlist_) {
                    playlist_ = (playlist_ + 1) % kPlaylist;
                    if (mix_.wanted == i) mix_.fade[SONG_GAME_1 + playlist_] = level;
                }
            }
        } else if (t.playing) {
            if (kSongs[i].restart) {
                StopMusicStream(t.music);
                t.started = false;
            } else {
                PauseMusicStream(t.music);
            }
            t.playing = false;
        }
    }
}

// Nur für die Testoption --music-test (main.cpp): eine Zeile, an der man
// ablesen kann, ob die Musik wirklich läuft und wie sie überblendet.
//   track  gewünschte Musik (MusicTrack: -1 keine, 0 Menü, 1 Spiel, 2 Piraten, 3 Stargate)
//   goal   Zielmusik angestoßen, list = aktuelles Stück der Spielmusik (0..2)
//   want   gewünschtes Stück (Index in kSongs: 0 Menü, 1-3 Spiel,
//          4 Piraten, 5 Ziel, 6 Stargate; -1 keins)
//   on/vol Optionen (Musik an, Regler)
//   fade, gain, play, pos: je Stück in der Reihenfolge von kSongs, durch "/"
//          getrennt: Blende, fertige Lautstärke, läuft der Stream laut raylib
//          (1) oder ist er pausiert (0), Position im Stück in Sekunden
//   ready  Audiogerät offen
std::string Audio::MusicReport() const {
    std::string fade, gain, play, pos;
    char buf[256];
    for (int i = 0; i < MusicMix::kTracks; ++i) {
        const char* sep = i > 0 ? "/" : "";
        const bool playing = songs_[i].valid && IsMusicStreamPlaying(songs_[i].music);
        snprintf(buf, sizeof(buf), "%s%.2f", sep, mix_.fade[i]);
        fade += buf;
        snprintf(buf, sizeof(buf), "%s%.3f", sep, mix_.Gain(i));
        gain += buf;
        snprintf(buf, sizeof(buf), "%s%d", sep, playing ? 1 : 0);
        play += buf;
        snprintf(buf, sizeof(buf), "%s%.1f", sep, songs_[i].valid ? GetMusicTimePlayed(songs_[i].music) : -1.0f);
        pos += buf;
    }
    snprintf(buf, sizeof(buf), "track=%d goal=%d list=%d want=%d on=%d vol=%.2f ", static_cast<int>(track_),
             goalMusic_ ? 1 : 0, playlist_, mix_.wanted, mix_.enabled ? 1 : 0, mix_.volume);
    return buf + ("fade=" + fade + " gain=" + gain + " play=" + play + " pos=" + pos) + (ready_ ? " ready=1" : " ready=0");
}

// Stummschalten über die Gesamtlautstärke: Effekte, Dauergeräusche und Musik
// laufen intern normal weiter (Wartelisten, Überblenden), sind nur nicht zu
// hören. Die Musik-Einstellung in den Optionen bleibt davon unberührt.
// Ohne Audiogerät wird nur der Merker umgeschaltet, damit die Anzeige
// "Ton an/aus" trotzdem stimmt.
void Audio::ToggleMute() {
    muted_ = !muted_;
    if (ready_) SetMasterVolume(muted_ ? 0.0f : 1.0f);
}
