#pragma once
// ---------------------------------------------------------------------------
// ClipTester.hpp – Seite "Videoclip Tester" der Optionen (Umsetzung:
// ClipTester.cpp). Gibt es nur im Debug-Build (CMake: CMAKE_BUILD_TYPE
// Debug, z. B. Start aus CLion): Dort setzt CMakeLists.txt das Makro
// MONDSCHUERFER_CLIP_TESTER. Im Release-Build (GitHub-Action release.yml)
// fehlen Seite und Code ganz.
//
// Wofür ist die Datei da?
//   Avatar-Clips prüfen, ohne ein Spiel zu starten (auch aus dem
//   Hauptmenü): links alle .mpg-Dateien aus assets/clips/ (Klick spielt den
//   Clip rechts daneben ab), darunter ein Pfadfeld und der Knopf "Teste Video
//   Datei abspielen" für eine beliebige Datei (Pfad eintippen oder einfügen
//   oder die Datei ins Spielfenster ziehen). Abgespielt wird mit demselben
//   ClipPlayer wie im Spiel – immer, auch wenn die Prüfung Fehler findet.
//   Unter dem Bild steht das Prüfergebnis: je Grundbedingung der Spiel-Clips
//   eine Zeile, grün "is valid" oder rot "is not valid" mit Grund
//   (Dateiendung .mpg, MPEG-1 Program Stream, MPEG-1-Video, 480x480, 30 fps,
//   höchstens cfg::CUE_TIME Sekunden, MP2-Ton mit 32000 Hz, Mono).
//
// Wer benutzt die Datei? src/Options.cpp (eigener Karteireiter).
// Abhängigkeiten: ClipPlayer.hpp (Wiedergabe), third_party/pl_mpeg (Prüfung),
// Ui.hpp, Assets.hpp (Ordner der Clips), ClientConfig.hpp (Clip-Lautstärke),
// raylib (Ordnerliste, Drag & Drop, Zeichnen).
// ---------------------------------------------------------------------------
#ifdef MONDSCHUERFER_CLIP_TESTER
#include "ClipPlayer.hpp"
#include "Ui.hpp"
#include "raylib.h"
#include <string>
#include <vector>

class ClipTester {
public:
    // Beim Wechsel auf die Seite: Clip-Liste neu einlesen.
    void Open();
    // Beim Verlassen der Seite oder Schließen der Optionen: Clip beenden.
    void Stop();
    // Zeichnet die Seite in area und verarbeitet Maus, Pfadfeld und
    // hineingezogene Dateien. focused = Knopf per Tastatur gewählt. Liefert
    // true, wenn der Zeiger auf einem Bedienelement liegt (Controller-A
    // klickt dann dort, siehe Options.cpp).
    bool Draw(Rectangle area, bool focused);
    // Knopf "Teste Video Datei abspielen": Datei aus dem Pfadfeld abspielen.
    void PlayTestFile();
    // Pfadfeld hat den Fokus (Tasten sind dann Text, keine Befehle).
    bool Typing() const { return path_.focused; }

private:
    struct Check {
        std::string text;   // z. B. "Frame per second: 30 is valid"
        bool ok;
    };
    void Play(const std::string& path);   // prüfen und abspielen

    std::string dir_;                  // Ordner der Clips (mit "/" am Ende)
    std::vector<std::string> files_;   // Dateinamen der Clips, sortiert
    int first_ = 0;                    // oberste sichtbare Zeile der Liste (Mausrad)
    std::string playing_;              // Pfad der zuletzt gestarteten Datei
    std::string error_;                // Grund, wenn kein Bild abspielbar ist
    std::vector<Check> checks_;        // Prüfergebnis zu playing_
    ui::TextField path_{"", "Pfad zur Videodatei", 512};
    ClipPlayer player_;
};
#endif
