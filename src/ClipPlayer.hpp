#pragma once
// ---------------------------------------------------------------------------
// ClipPlayer.hpp – Videoclips der Funksprüche unten rechts (Umsetzung:
// ClipPlayer.cpp).
//
// Wofür ist die Datei da?
//   Missionen melden sich mit einem Funkspruch: ein kurzer Avatar-Clip aus
//   assets/clips/ (Dateinamen in cfg::MISSIONS) mit Untertitel. Welcher
//   Funkspruch gerade läuft, steht in PlayerState::cue/cueT (Spiellogik bzw.
//   vom Server). Diese Klasse spielt den passenden Clip ab: Bild als Textur,
//   Ton als raylib-Sound. Es gibt genau ein Objekt davon: g_clip.
//
// Wer benutzt sie?
//   - src/main.cpp: Init()/Shutdown(); Einzelspieler jedes Frame Update()
//     in App::UpdateAudio.
//   - src/Multiplayer.cpp: Update() in MultiplayerScreen::UpdateAudio.
//   - src/Hud.cpp: Frame() zeichnet das Bild im Funkspruch-Kasten.
//   - src/Options.cpp: SetVolume() (Seite "Ton", Clip-Lautstärke); beim
//     Start setzt main.cpp den Wert aus der Einstellungsdatei.
//
// Abhängigkeiten:
//   - raylib (Textur, Sound), third_party/pl_mpeg (MPEG-1-Video, MP2-Ton).
//   - shared/Config.hpp: cfg::MISSIONS (Clip je Missionsart und Phase).
//   Die Clips müssen MPEG-1-Video mit MPEG-1-Layer-II-Ton (MP2, 32, 44,1
//   oder 48 kHz) sein; anderes kann pl_mpeg nicht lesen. Umwandeln, das Bild
//   bleibt dabei unverändert:
//     ffmpeg -i alt.mpg -c:v copy -c:a mp2 -ar 32000 -ac 1 -b:a 128k -f mpeg neu.mpg
//   Fehlt eine Datei, bleibt es beim Untertitel; unlesbarer Ton = stumm.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include <string>
#include <vector>

typedef struct plm_t plm_t;

class ClipPlayer {
public:
    // Nur den Ordner merken (g_assets.AssetDir()); geladen wird ein Clip erst,
    // wenn er gebraucht wird.
    void Init(const std::string& assetDir) { dir_ = assetDir + "clips/"; }
    // Laufenden Clip freigeben (vor dem Schließen von Audiogerät und Fenster).
    void Shutdown() { Stop(); }
    // Optionen, Seite "Ton": Lautstärke der Clips 0..1, wirkt sofort (auch auf
    // einen laufenden Clip). Der Ton jedes Clips ist schon beim Laden auf
    // volle Aussteuerung gebracht (Open), 1 = so laut wie möglich.
    void SetVolume(float v);

    // Einmal pro Frame. cue = Funkspruch (Missionstyp * 3 + Phase, wie
    // PlayerState::cue), -1 = keiner. Ein neuer Wert beendet den alten Clip.
    // show = der Kasten ist zu sehen (nicht angedockt, keine Pause): Erst dann
    // startet der Clip; ist show false, hält er an (Bild und Ton) und läuft
    // später an derselben Stelle weiter. Nach dem Ende bleibt das letzte Bild
    // stehen, bis cue wechselt.
    void Update(float dt, int cue, bool show);

    // Aktuelles Bild für den Funkspruch cue oder nullptr (kein Clip, Datei
    // fehlt, noch nicht gestartet).
    const Texture2D* Frame(int cue) const { return cue == cue_ && hasFrame_ ? &tex_ : nullptr; }

private:
    void Open(const std::string& file);   // Datei laden, Ton vorab ganz entpacken
    void Stop();                          // alles freigeben

    std::string dir_;
    int cue_ = -1;            // Funkspruch, zu dem der geladene Clip gehört
    bool tried_ = false;      // Open() für cue_ schon versucht
    plm_t* plm_ = nullptr;    // Video-Decoder (Ton ist abgeschaltet)
    Texture2D tex_{};
    std::vector<unsigned char> rgb_;   // ein Bild als RGB, für UpdateTexture
    bool hasFrame_ = false;
    bool ended_ = false;      // letztes Bild erreicht
    double t_ = 0.0;          // Laufzeit des Clips (nur, solange er zu sehen ist)
    double fps_ = 30.0;
    int frames_ = 0;          // bisher entpackte Bilder
    Sound sound_{};
    bool hasSound_ = false;
    bool paused_ = false;
    float volume_ = 1.0f;
};

// Das einzige Clip-Objekt (definiert in ClipPlayer.cpp).
extern ClipPlayer g_clip;
