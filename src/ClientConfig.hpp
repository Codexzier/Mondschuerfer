#pragma once
// ---------------------------------------------------------------------------
// ClientConfig: kleine Einstellungsdatei des Spiels (Mehrspieler und Musik).
//
// Wofür? Damit man die Serveradresse und den eigenen Spielernamen nicht bei
// jedem Start neu eintippen muss, und damit die Musik-Einstellungen aus den
// Optionen (an/aus, Lautstärke) beim nächsten Start noch gelten.
//
// Wer benutzt sie?
//   Es gibt genau ein Objekt: g_clientConfig (global, definiert in
//   ClientConfig.cpp). main() lädt es einmal beim Start, noch vor dem
//   Hauptmenü, und gibt die Musikwerte an g_audio weiter.
//   - src/Multiplayer.cpp füllt damit die Eingabefelder vor und speichert,
//     sobald man auf "Verbinden" drückt bzw. sich erfolgreich angemeldet hat.
//   - src/Options.cpp ändert die Musikwerte und speichert sie.
//   - src/main.cpp speichert beim Beenden noch einmal, falls eine Änderung
//     noch nicht in der Datei steht (SaveIfChanged).
//   Warum ein gemeinsames Objekt statt einer Kopie pro Bildschirm? Save()
//   schreibt immer die GANZE Datei. Hätte der Mehrspieler eine eigene Kopie
//   (gelesen beim Öffnen des Mehrspieler-Menüs), würde er beim Speichern der
//   Serveradresse die alten Musikwerte zurückschreiben – und eine Änderung,
//   die man danach in den Optionen der Mehrspieler-Pause gemacht hat, wäre weg.
//
// Wo liegt die Datei? Neben der ausführbaren Datei (wie der Spielstand in
// Save.cpp): GetApplicationDirectory() + "mondschuerfer_client.txt".
//
// Format: einfache Textdatei, eine Einstellung pro Zeile, "Schlüssel Wert":
//     server 192.168.1.20:7777
//     name Anna
//     music 1
//     music_volume 0.50
//     clip_volume 1.00
//     subtitles 1
//     tilt_shift 0.50
//     msaa 4
//     fxaa 0.00
//     track_time 60
// Unbekannte Zeilen werden ignoriert (so bleiben ältere Spielversionen mit
// neueren Dateien verträglich). Fehlende Zeilen behalten ihren Standardwert
// (z. B. eine Datei aus der Zeit vor den Musik-Einstellungen).
//
// WICHTIG: Das Passwort wird NIE gespeichert. Es existiert nur im Speicher,
// solange das Anmeldefenster offen ist, und wird auch nicht zum Server
// geschickt (siehe shared/Crypto.hpp: nur ein daraus berechneter Schlüssel).
//
// Abhängigkeiten: raylib (nur GetApplicationDirectory), Standardbibliothek.
// Läuft im Spiel-Thread (das Spiel hat nur einen).
// ---------------------------------------------------------------------------
#include <string>

struct ClientConfig {
    std::string server = "localhost:7777";   // zuletzt benutzte Serveradresse ("Host" oder "Host:Port")
    std::string name;                         // zuletzt erfolgreich angemeldeter Spielername
    bool musicEnabled = true;                 // Optionen: Musik an/aus           (Zeile "music 1|0")
    float musicVolume = 0.5f;                 // Optionen: Musiklautstärke 0..1   (Zeile "music_volume 0.50")
    float clipVolume = 1.0f;                  // Optionen: Lautstärke der Missions-Clips 0..1 (Zeile "clip_volume 1.00")
    bool subtitles = true;                    // Optionen: Untertitel der Funksprüche an/aus (Zeile "subtitles 1|0")
    float tiltShift = 0.5f;                   // Optionen: Tilt-Shift-Stärke 0..1, 0 = aus (Zeile "tilt_shift 0.50")
    int msaa = 4;                             // Optionen: Kantenglättung 0/2/4/8/16-fach, 0 = aus (Zeile "msaa 4")
    float fxaa = 0.0f;                        // Optionen: FXAA-Stärke 0..1, 0 = aus (Zeile "fxaa 0.00")
    int trackTime = 60;                       // Optionen: Fahrspuren der Rover sichtbar in s: 0 (aus), 15, 30, 60, 120, 300 (Zeile "track_time 60")

    // Steht NICHT in der Datei: true, wenn ein Wert geändert, aber noch nicht
    // gespeichert wurde. Setzt, wer ändert, ohne gleich zu speichern (die
    // Optionen beim Ziehen des Lautstärkereglers); Save() löscht es wieder.
    bool unsaved = false;

    // Vollständiger Pfad der Datei (neben der .exe)
    static std::string Path();

    // Liest die Datei. Fehlt sie oder ist sie unlesbar, bleiben die
    // Standardwerte stehen und es wird false geliefert (kein Fehler fürs Spiel).
    bool Load();

    // Schreibt die Datei neu. false, wenn sie nicht geschrieben werden konnte
    // (z. B. Spielordner schreibgeschützt) – das Spiel läuft trotzdem weiter.
    // Bei Erfolg wird unsaved zurückgesetzt.
    bool Save();

    // Speichert nur, wenn unsaved gesetzt ist (Optionen schließen, Spielende).
    void SaveIfChanged() {
        if (unsaved) Save();
    }
};

// Die Einstellungen des Spiels (definiert in ClientConfig.cpp, geladen in main()).
extern ClientConfig g_clientConfig;
