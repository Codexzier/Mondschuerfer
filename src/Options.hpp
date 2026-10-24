#pragma once
// ---------------------------------------------------------------------------
// Options.hpp – der Bildschirm "Optionen" (Umsetzung: Options.cpp).
//
// Wofür ist die Datei da?
//   Die Optionen zeigen ein Fenster mittig über dem Bild mit drei
//   Karteireitern (Seiten) und darunter dem Knopf "Zurück":
//   Seite "Ton":
//     - Musik an/aus (Umschalter), Musiklautstärke und Lautstärke der
//       Missions-Clips (Schieberegler in 5-%-Schritten), Untertitel der
//       Funksprüche an/aus (Umschalter).
//   Seite "Grafik":
//     - der Stärke des Kameraeffekts Tilt-Shift (Schieberegler in
//       10-%-Schritten, 0 = aus; siehe TiltShift.hpp),
//     - der Kantenglättung (Regler: Aus, 2x, 4x, 8x, 16x) und der Stärke
//       von FXAA (Glätten per Shader, 10-%-Schritte, 0 = aus); beides
//       lässt sich gleichzeitig einschalten,
//     - der Dauer der Fahrspuren der Rover (Regler: Aus, 15 s, 30 s, 1 min,
//       2 min, 5 min; siehe Render.cpp).
//   Seite "Steuerung":
//     - einer Übersicht der Steuerung als Tabelle: Tastatur/Maus, Xbox- und
//       PlayStation-Controller (nur zum Nachlesen, nicht änderbar).
//   Änderungen wirken sofort (die Musik wird gleich lauter/leiser) und
//   werden in der Einstellungsdatei gespeichert (ClientConfig), damit sie
//   beim nächsten Start noch gelten.
//
// Wer benutzt sie?
//   - src/main.cpp: aus dem Hauptmenü und aus der Pause des Einzelspielers
//     (eigener Bildschirm Screen::Options, danach zurück dorthin).
//   - src/Multiplayer.cpp: aus der Pause des Mehrspielers (das Spiel läuft
//     dort auf dem Server weiter; die Optionen liegen wie die Pause darüber).
//   Jeder Aufrufer besitzt ein eigenes OptionsScreen-Objekt. Das ist
//   unproblematisch, weil die Werte selbst nicht hier liegen, sondern in
//   g_audio (wirkt sofort) und g_clientConfig (wird gespeichert).
//
// Abhängigkeiten: Ui.hpp (Panel, Knopf, Schieberegler, Umschalter, Texte),
// Audio.hpp (g_audio: Musikwerte, Bedientöne), ClipPlayer.hpp (g_clip:
// Clip-Lautstärke), ClientConfig.hpp
// (g_clientConfig: Speichern), raylib (Tastatur, Maus, Zeichnen).
//
// Ablauf:
//   Open()  einmal beim Öffnen,
//   Draw()  danach jedes Frame (zwischen BeginDrawing und EndDrawing, nach
//           der 3D-Welt), bis es true liefert; dann schließt der Aufrufer
//           die Optionen und kehrt zu seinem Menü zurück.
//
// Bedienung:
//   Maus:     Karteireiter, Umschalter bzw. Zeile anklicken, Regler ziehen,
//             "Zurück".
//   Tastatur: Tab (Umschalt+Tab rückwärts) wechselt die Seite, Pfeil
//             hoch/runter (oder W/S) wählt die Zeile (auch die Karteireiter),
//             Pfeil links/rechts (oder A/D) verstellt den Regler der Zeile bzw.
//             wechselt auf den Karteireitern die Seite, Enter/Leertaste
//             schaltet die Musik bzw. die Untertitel um oder drückt
//             "Zurück", ESC = zurück.
//   Controller: LB/RB wechselt die Seite, linker Stick bewegt den Zeiger,
//             A klickt (Regler: A halten und ziehen), Steuerkreuz wie die
//             Pfeiltasten, B = zurück (PlayStation: L1/R1, Kreuz, Kreis). Siehe "Controller in Menüs" in Ui.hpp.
// ---------------------------------------------------------------------------

class OptionsScreen {
public:
    // Beim Öffnen: Auswahl auf die erste Zeile, Frame-Zähler auf 0.
    void Open();

    // Zeichnet die Optionen und verarbeitet Maus und Tastatur dieses Frames.
    // Liefert true, wenn sie geschlossen werden sollen ("Zurück" oder ESC);
    // ungespeicherte Änderungen sind dann schon gespeichert.
    bool Draw();

    // Die beiden Aktionen der Optionen. Draw() benutzt sie bei Klick oder
    // Taste; öffentlich, damit die Testoption --music-test (main.cpp) genau
    // denselben Weg gehen kann wie ein Spieler.
    //   SetMusicOn: Musik an/aus, wird sofort gespeichert.
    //   SetVolume:  Lautstärke 0..1, gerundet auf 5-%-Schritte; save = false
    //               merkt die Änderung nur vor (ClientConfig::unsaved) –
    //               gespeichert wird dann beim Loslassen des Reglers oder
    //               beim Schließen der Optionen.
    void SetMusicOn(bool on);
    void SetVolume(float v, bool save);
    // Lautstärke der Missions-Clips 0..1, sonst wie SetVolume.
    void SetClipVolume(float v, bool save);

private:
    // Karteireiter (Seiten) der Optionen
    enum Tab { TAB_SOUND, TAB_GRAPHICS, TAB_CONTROLS, TAB_COUNT };
    // Wählbare Zeilen (Tastatursteuerung); welche davon zu sehen sind, hängt
    // von der Seite ab (RowsOf). ROW_TABS = die Karteireiter selbst.
    enum Row { ROW_TABS, ROW_MUSIC, ROW_VOLUME, ROW_CLIPS, ROW_SUBTITLES, ROW_TILT, ROW_MSAA, ROW_FXAA, ROW_TRACKS, ROW_BACK };

    // Wählbare Zeilen der Seite tab von oben nach unten; liefert die Anzahl
    // (rows braucht Platz für 6).
    static int RowsOf(int tab, int* rows);
    // Seite wechseln (siehe Options.cpp).
    void SetTab(int tab, bool keepTabsRow);

    // Schließen: Ziehen beenden, Ungespeichertes speichern, Ton "zurück".
    void Close();

    // Untertitel der Funksprüche an/aus, wird wie SetMusicOn sofort gespeichert.
    void SetSubtitlesOn(bool on);

    // Tilt-Shift-Stärke 0..1 (10-%-Schritte, 0 = aus); save wie bei SetVolume.
    void SetTiltShift(float v, bool save);

    // Kantenglättung: Stufe 0..4 = Aus, 2x, 4x, 8x, 16x (MsaaLevel: aus der
    // Zahl der Abtastpunkte die Stufe). FXAA-Stärke 0..1 in 10-%-Schritten.
    static int MsaaLevel(int samples);
    void SetMsaaLevel(int level, bool save);
    void SetFxaa(float v, bool save);

    // Fahrspuren: Stufe 0..5 = Aus, 15 s, 30 s, 1 min, 2 min, 5 min
    // (TrackLevel: aus den Sekunden die Stufe); save wie bei SetVolume.
    static int TrackLevel(int seconds);
    void SetTrackLevel(int level, bool save);

    int tab_ = TAB_SOUND;    // gewählte Seite
    int sel_ = ROW_MUSIC;    // per Tastatur ausgewählte Zeile
    int frames_ = 0;         // Frames seit Open() (Tasten erst ab dem 2. Frame, siehe Draw)
    bool dragging_ = false;  // Lautstärkeregler wird gerade mit der Maus gezogen (siehe ui::Slider)
    bool draggingClip_ = false;   // dasselbe für den Clip-Lautstärkeregler
    bool draggingTilt_ = false;   // dasselbe für den Tilt-Shift-Regler
    bool draggingMsaa_ = false;   // ... den Kantenglättungs-Regler
    bool draggingFxaa_ = false;   // ... den FXAA-Regler
    bool draggingTracks_ = false; // ... den Fahrspuren-Regler
};
