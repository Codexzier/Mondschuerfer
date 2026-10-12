#pragma once
// ---------------------------------------------------------------------------
// Audio.hpp – Ton des Spiels: Soundeffekte und Dauergeräusche
// (Umsetzung: Audio.cpp).
//
// Wofür ist die Datei da?
//   Die Klasse Audio spielt alle Geräusche des Spiels ab. Es gibt genau ein
//   Objekt davon: g_audio (global, weil es genau ein Audiogerät gibt und viele
//   Stellen Töne auslösen: Menüs, HUD, Spielwelt). Es gibt zwei Arten Töne:
//     - Effekte (Sfx): kurze Töne, die einmal ablaufen (Schuss, Treffer,
//       Menü-Klick). Play() spielt sie direkt, PlayAt() leiser und seitlich
//       verschoben, je nachdem, wo sie in der Welt passieren.
//     - Dauergeräusche (enum Loop): Triebwerk, Schub, Scanner, Abbau-Laser.
//       Sie laufen als Endlosschleife und werden jedes Frame über LoopState
//       lauter oder leiser gestellt (0 = aus).
//
// Wer benutzt sie?
//   - src/main.cpp: Init() beim Start, Shutdown() beim Beenden, ToggleMute()
//     (Taste M). Einzelspieler: jedes Frame PlayAt() für world.sounds und
//     danach Update() (App::UpdateAudio).
//   - src/Multiplayer.cpp: dasselbe mit den Tönen, die der Server schickt
//     (NetClient::TakeSounds), plus Menütöne mit Play().
//   - src/Ui.cpp, src/Hud.cpp: Menü- und Fehlertöne mit Play().
//   Der Server benutzt diese Datei NICHT (er hat kein Audiogerät); er
//   verschickt nur SoundEvents, die der Client hier abspielt.
//
// Abhängigkeiten:
//   - raylib, Audio-Teil (InitAudioDevice, Sound, Music; intern arbeitet die
//     Bibliothek "miniaudio").
//   - shared/SoundEvents.hpp: enum class Sfx und struct SoundEvent.
//   - Sounddateien in assets/sounds/ (Kenney "Sci-Fi Sounds" und "Interface
//     Sounds", Format .ogg).
//
// Ablauf:
//   1. Init(): Audiogerät öffnen, alle Effekte und Schleifen laden. Gibt es
//      kein Audiogerät, tun alle Funktionen einfach nichts – das Spiel läuft
//      dann stumm weiter.
//   2. Jedes Frame: Play()/PlayAt() für neue Ereignisse, danach genau einmal
//      Update() mit dem gewünschten Zustand der Dauergeräusche.
//   3. Shutdown(): alles freigeben, Audiogerät schließen.
//
// Threads: Alle Funktionen hier werden nur vom Haupt-Thread aufgerufen.
// raylib mischt den Ton intern in einem eigenen Audio-Thread von miniaudio;
// davon merkt man nur, dass Music-Streams regelmäßig (in Update) nachgefüllt
// werden müssen.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include <string>
#include <vector>

// Soundeffekte aus den Kenney-Paketen "Sci-Fi Sounds" und "Interface Sounds".
// Welche Datei zu welchem Ereignis gehört, steht in der Tabelle in Audio.cpp.
// Die Liste der Effekte (enum class Sfx) und SoundEvent stehen in
// shared/SoundEvents.hpp, weil auch die Spiellogik und der Server sie kennen.
#include "SoundEvents.hpp"

// Gewünschter Zustand der Dauergeräusche, jedes Frame neu aus dem Spielzustand
// gesetzt (0 = aus, 1 = volle Lautstärke laut Tabelle kLoops in Audio.cpp).
// Gefüllt wird er in main.cpp (App::UpdateAudio, Einzelspieler) und in
// Multiplayer.cpp (MultiplayerScreen::UpdateAudio) aus dem eigenen Schiff.
// Ein frisch angelegter LoopState bedeutet "alles aus" – so im Menü, in der
// Pause, angedockt und nach dem Absturz. Audio::Update blendet dann weich aus.
struct LoopState {
    float engine = 0.0f;       // Triebwerk im Leerlauf (1, solange man fliegt)
    float enginePitch = 1.0f;  // Tonhöhe von Triebwerk und Schub: 0,9 im Stand bis 1,15 bei Höchstgeschwindigkeit
    float thrust = 0.0f;       // Schubgeräusch, folgt Ship::thrustVisual (weich nachgeführter Schub 0..1)
    float scanner = 0.0f;      // Scanner eingeschaltet
    float mining = 0.0f;       // Abbau läuft
    bool alarm = false;    // Schild kritisch (unter 30 %): Warnton in festem Abstand
};

class Audio {
public:
    // Öffnet das Audiogerät und lädt alle Töne aus assetDir + "sounds/".
    // Nach Assets::Init() aufrufen, weil erst dort der Assets-Ordner gesucht
    // wird (main.cpp übergibt g_assets.AssetDir()). Gibt false zurück, wenn es
    // kein Audiogerät gibt; das Spiel läuft dann stumm. Fehlende einzelne
    // Dateien führen nur zu einer Warnung im Log, der Ton fehlt dann.
    bool Init(const std::string& assetDir);
    // Alles freigeben und das Audiogerät schließen (vor CloseWindow).
    void Shutdown();

    // Einmal pro Frame, in JEDEM Bildschirm (auch Menü/Pause):
    //   - verzögerte Effekte abspielen, deren Wartezeit abgelaufen ist,
    //   - Dauergeräusche weich auf die Lautstärke aus loops überblenden und
    //     ihre Music-Streams nachfüllen (sonst stockt der Ton),
    //   - Warnton bei kritischem Schild wiederholen.
    void Update(float dt, const LoopState& loops);

    // Effekt abspielen, ohne Ortsbezug (Menütöne, Hinweise).
    //   volume: 0..1, wird mit der Grundlautstärke aus der Tabelle multipliziert
    //   pan:    0 = rechts, 0.5 = Mitte, 1 = links (wie in raylib 5.5)
    //   delay:  Sekunden bis zur Wiedergabe (z. B. Kassenton kurz nach dem Andocken)
    void Play(Sfx id, float volume = 1.0f, float pan = 0.5f, float delay = 0.0f);
    // Effekt aus der Spielwelt: Lautstärke und Stereo-Position abhängig vom
    // Abstand zwischen e.pos und dem Zuhörer (dem Kameraziel, also der
    // Bildmitte). Zuschauer im Mehrspieler übergeben listener = e.pos und
    // hören damit alles ungedämpft in der Mitte.
    void PlayAt(const SoundEvent& e, Vector3 listener);

    // Ton an/aus (Taste M) über die Gesamtlautstärke von raylib.
    void ToggleMute();
    bool Muted() const { return muted_; }

    // Nummern der Dauergeräusche. Öffentlich, weil die Tabelle kLoops in
    // Audio.cpp (außerhalb der Klasse) LOOP_COUNT als Größe braucht.
    enum Loop { LOOP_ENGINE, LOOP_THRUST, LOOP_SCANNER, LOOP_MINING, LOOP_COUNT };

private:
    // Eine Sounddatei. raylib kann einen Sound nur einmal gleichzeitig
    // abspielen (PlaySound auf einen laufenden Sound startet ihn von vorn).
    // Damit sich z. B. schnell aufeinanderfolgende Schüsse überlagern, gibt es
    // "Aliase": weitere Abspielköpfe, die dieselben Sampledaten benutzen
    // (kein zusätzlicher Speicher für die Daten). next zeigt reihum auf den
    // nächsten Abspielkopf; sind alle belegt, wird der älteste neu gestartet.
    struct Clip {
        std::vector<Sound> voices;  // [0] = Original, danach Aliase für Überlagerung
        int next = 0;
    };
    // Ein Effekt (ein Eintrag aus enum Sfx) mit seinen Dateivarianten.
    struct Effect {
        std::vector<Clip> clips;    // Varianten, zufällig gewählt
        int last = -1;              // zuletzt gespielte Variante (nicht zweimal hintereinander)
        double lastPlay = -1e9;     // Zeitpunkt (clock_) der letzten Wiedergabe, für den Mindestabstand
    };
    // Ein Dauergeräusch als raylib-Music-Stream (siehe Dateikopf von Audio.cpp).
    struct Track {
        Music music{};
        bool valid = false;         // Datei geladen
        bool started = false;       // PlayMusicStream schon einmal aufgerufen
        bool playing = false;       // läuft gerade (false = pausiert)
        float volume = 0.0f;        // aktuelle, weich nachgeführte Lautstärke
    };
    // Ein verzögerter Effekt, der in Update() abgespielt wird, sobald delay <= 0.
    struct Pending {
        Sfx id;
        float volume, pan, delay;
    };

    // Spielt einen Effekt sofort ab (ohne Verzögerung, ohne Ortsberechnung).
    void Trigger(Sfx id, float volume, float pan);
    // Führt ein Dauergeräusch auf die Ziel-Lautstärke level (0..1) nach.
    void UpdateLoop(int loop, float level, float pitch, float dt);

    Effect effects_[static_cast<int>(Sfx::Count)];
    Track loops_[LOOP_COUNT];
    std::vector<Pending> pending_;   // wartende verzögerte Effekte
    double clock_ = 0.0;             // eigene Uhr (Summe aller dt aus Update), für den Mindestabstand
    float alarmT_ = 0.0f;            // Sekunden bis zum nächsten Warnton
    bool ready_ = false;             // Audiogerät offen, Töne geladen
    bool muted_ = false;
};

// Das einzige Audio-Objekt (definiert in Audio.cpp).
extern Audio g_audio;
