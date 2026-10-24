#pragma once
// ---------------------------------------------------------------------------
// MusicMix.hpp – Mischpult für die Hintergrundmusik: Welches Stück ist wie
// laut? (reine Rechenlogik, ohne raylib)
//
// Wofür ist die Datei da?
//   Das Spiel hat mehrere Musikstücke: eins für die Menüs, drei für das
//   Spiel (nacheinander im Kreis) und Situationsmusik für die Piraten-Mission
//   und das erreichte Ziel (Dateien, Auswahl und Abspielen: Audio.cpp). Beim
//   Wechsel zwischen den Stücken soll die Musik nicht hart umschalten,
//   sondern weich überblenden: Das neue Stück wird lauter, das alte leiser.
//   Außerdem lässt sich die Musik in den Optionen ausschalten und in der
//   Lautstärke einstellen.
//   Diese kleine Struktur rechnet nur die Zahlen dafür aus – sie spielt
//   selbst nichts ab. Deshalb braucht sie kein Audiogerät und keine
//   raylib-Funktion und lässt sich mit einem winzigen Testprogramm prüfen
//   (Werte vorgeben, Step() aufrufen, Ergebnis ansehen).
//
// Wer benutzt sie?
//   Nur src/Audio.cpp (die Klasse Audio besitzt ein MusicMix-Objekt).
//   MusicTrack benutzen außerdem main.cpp und Multiplayer.cpp, um über
//   g_audio.SetMusicTrack() zu sagen, welche Musik gerade laufen soll;
//   Audio.cpp wählt daraus das Stück (Index in seiner Tabelle kSongs).
//
// Abhängigkeiten: keine (nur Standard-C++). Läuft im Spiel-Thread.
//
// Ablauf (einmal pro Frame, in Audio::Update):
//   1. Wunsch setzen: wanted (welches Stück, Index in kSongs), enabled (Musik an/aus),
//      volume (Regler in den Optionen).
//   2. Step(dt): Jede "Blende" (fade, 0 = stumm, 1 = voll) läuft ein Stück
//      auf ihr Ziel zu: das gewünschte Stück auf 1, alle anderen auf 0.
//   3. Gain(i): fertige Lautstärke für raylibs SetMusicVolume.
// ---------------------------------------------------------------------------

// Welche Musik gerade laufen soll.
//   None     Stille (z. B. wenn ein Stück fehlt oder für Tests)
//   Menu     "Main Menu Theme": Hauptmenü, Optionen aus dem Hauptmenü,
//            Mehrspieler-Menüs (Adresse, Lobby, Anmeldung)
//   Game     "Lunar Silence 01" bis "03" nacheinander im Kreis: im Spiel
//            (auch Pause, Endbildschirme, Simulation, Mehrspieler-Spiel und
//            Zuschauen)
//   Pirates  "Entering Pirate Territory": im Spiel, solange die eigene
//            Mini-Mission "Piraten abschießen" läuft
// Das erreichte Ziel ("Deep Space Horizon") ist kein eigener Wert: Es wird
// einmal über Audio::PlayGoalMusic() angestoßen und läuft dann anstelle von
// Game/Pirates bis zu seinem Ende.
enum class MusicTrack { None = -1, Menu = 0, Game = 1, Pirates = 2 };

struct MusicMix {
    static constexpr int kTracks = 6;         // Anzahl der Stücke (Tabelle kSongs in Audio.cpp)
    static constexpr float kFadeIn = 1.5f;    // Sekunden von stumm bis voll (Einblenden)
    static constexpr float kFadeOut = 1.0f;   // Sekunden von voll bis stumm (Ausblenden)
    // Grundpegel: Auch bei Regler 100 % spielt die Musik nur mit 70 % der
    // Dateilautstärke. Die Stücke sind laut abgemischt (gemessen im Mittel
    // etwa -17 dB, Spitzen -4 dB); so bleiben Schüsse, Warntöne und
    // Menütöne darüber gut hörbar. Wert nach Gehör gewählt.
    static constexpr float kBaseLevel = 0.7f;

    int wanted = -1;                          // gewünschtes Stück (Index in kSongs), -1 = Stille
    bool enabled = true;                      // Optionen: Musik an/aus
    float volume = 0.5f;                      // Optionen: Lautstärke 0..1
    float fade[kTracks] = {};                 // aktuelle Blende je Stück, 0..1

    // Blenden um dt Sekunden weiterführen.
    // Lineare Rampe: pro Sekunde ändert sich die Blende um 1 / kFadeIn
    // (lauter) bzw. 1 / kFadeOut (leiser). Weil der Schritt dt-mal so groß
    // ist, dauert eine Blende immer gleich lang – egal ob das Spiel mit 30
    // oder 144 Bildern pro Sekunde läuft ("bildratenunabhängig").
    // Ausblenden ist etwas schneller als Einblenden: Beim Wechsel ist das
    // alte Stück nach 1 s weg, während das neue noch eine halbe Sekunde
    // anschwillt – so überlagern sich die beiden nur kurz und leise.
    // Ein negatives dt (sollte nie vorkommen) wird wie 0 behandelt.
    void Step(float dt) {
        if (dt < 0.0f) dt = 0.0f;
        for (int i = 0; i < kTracks; ++i) {
            const float target = (enabled && wanted == i) ? 1.0f : 0.0f;
            if (fade[i] < target) {
                fade[i] += dt / kFadeIn;
                if (fade[i] > target) fade[i] = target;   // nicht übers Ziel hinaus
            } else if (fade[i] > target) {
                fade[i] -= dt / kFadeOut;
                if (fade[i] < target) fade[i] = target;
            }
        }
    }

    // Lautstärke, mit der Stück i gerade abgespielt werden soll (0..0,7):
    //   Blende * Regler * Grundpegel
    // Alle drei Faktoren liegen zwischen 0 und 1, also auch das Ergebnis.
    // 0 heißt: unhörbar – Audio.cpp pausiert das Stück dann.
    float Gain(int i) const {
        if (i < 0 || i >= kTracks) return 0.0f;
        float v = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
        return fade[i] * v * kBaseLevel;
    }
};
