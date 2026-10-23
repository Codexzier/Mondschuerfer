// ---------------------------------------------------------------------------
// Options.cpp – Bildschirm "Optionen" mit drei Karteireitern: "Ton" (Musik
// an/aus, Musik- und Clip-Lautstärke), "Grafik" (Tilt-Shift, Kantenglättung, FXAA) und "Steuerung"
// (Übersicht der Tasten) (Schnittstelle und Bedienung: Options.hpp).
//
// Wer benutzt die Datei? main.cpp (Hauptmenü, Einzelspieler-Pause) und
// Multiplayer.cpp (Mehrspieler-Pause), jeweils über ein OptionsScreen-Objekt.
//
// Abhängigkeiten:
//   - Ui.hpp: Panel, Button, Slider, Toggle, Texte, Farben, Scale()
//   - Audio.hpp: g_audio (Musik an/aus und Lautstärke wirken sofort; Töne
//     beim Bedienen)
//   - ClipPlayer.hpp: g_clip (Lautstärke der Missions-Clips, wirkt sofort)
//   - ClientConfig.hpp: g_clientConfig (Werte in der Einstellungsdatei)
//   - Controls.hpp: Tastennamen für die Hilfezeile unten (Tastatur oder
//     Controller)
//   - raylib: Tastatur, Controller-Knöpfe, Zeichnen
//
// Ablauf von Draw() (jedes Frame, "immediate mode" wie in Ui.hpp):
//   1. Bild abdunkeln, Fenster (Panel) mittig zeichnen. Alle Maße sind für
//      eine Fensterhöhe von 900 Pixeln gedacht und werden mit s = Scale()
//      multipliziert; so passt das Fenster von 960 x 540 bis Vollbild.
//   2. Tastatur: Seite wechseln, Zeile wählen, Regler ändern, umschalten,
//      zurück.
//   3. Karteireiter zeichnen; Klick wechselt die Seite.
//   4. Inhalt der gewählten Seite:
//      "Ton":       Umschalter und zwei Schieberegler (Maus wird dabei ausgewertet)
//      "Grafik":    Schieberegler für Tilt-Shift, Kantenglättung (MSAA) und
//                   FXAA (alle umgesetzt in TiltShift.cpp)
//      "Steuerung": feste Tabelle (kControls unten)
//   5. Knopf "Zurück".
// Das Fenster ist immer so hoch wie die längste Seite, damit es beim
// Wechseln der Seite nicht springt.
// Wo sich ein Wert ändert, laufen alle Wege (Maus, Tastatur, Testoption)
// über SetMusicOn, SetVolume bzw. SetClipVolume: Dort wird der Wert an g_audio
// bzw. g_clip gegeben,
// in g_clientConfig eingetragen und gespeichert.
//
// Alles läuft im Spiel-Thread.
// ---------------------------------------------------------------------------
#include "Options.hpp"
#include "Audio.hpp"
#include "ClientConfig.hpp"
#include "ClipPlayer.hpp"
#include "Controls.hpp"
#include "TiltShift.hpp"
#include "Ui.hpp"
#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <string>

using namespace ui;

namespace {

// ===========================================================================
// Tabelle: Steuerung (nur Anzeige)
// ===========================================================================
// Spalten: Aktion | Tastatur / Maus | Xbox-Controller | PlayStation-Controller.
// "-" = auf diesem Gerät nicht belegt.
// Die eingebaute Schrift von raylib kennt nur die Zeichen bis 255 (Latin-1):
// keine Pfeile und keine Symbole wie die PlayStation-Zeichen. Deshalb stehen
// hier Wörter ("Pfeiltasten", "Kreuz", "Kreis", "Dreieck").
// Gleiche Lage, gleiche Aufgabe: Der untere Knopf rechts heißt bei Xbox "A"
// und bei PlayStation "Kreuz" – beide schießen. Ebenso B/Kreis (rechts),
// Y/Dreieck (oben), RT/R2 und LT/L2 (Trigger). Die Menü-Taste heißt bei Xbox
// "Menü" (drei Striche), bei PlayStation "Options" (rechts neben dem Touchpad).
// Ausgewertet werden die Tasten in Controls.cpp (Flug) und Ui.cpp (Menüs);
// diese Tabelle muss dazu passen.
struct ControlRow {
    const char* action;
    const char* keyboard;
    const char* xbox;
    const char* playstation;
};

const ControlRow kControls[] = {
    {"Drehen / Flugrichtung", "A / D, Pfeiltasten", "Linker Stick", "Linker Stick"},
    {"Schub / Bremsen", "W / S", "RT / LT", "R2 / L2"},
    {"Hinfliegen", "Linksklick auf den Boden", "-", "-"},
    {"Dem Mauszeiger folgen", "Linke Maustaste halten", "-", "-"},
    {"Schießen", "Leertaste", "A", "Kreuz"},
    {"Scanner an/aus", "E", "B", "Kreis"},
    {"Andocken / Abbauen", "F", "Y", "Dreieck"},
    {"Pause / Menü", "ESC oder P", "Menü-Taste", "Options"},
    {"Zoom", "Mausrad", "Rechter Stick", "Rechter Stick"},
    {"Flugziel löschen", "Rechte Maustaste", "-", "-"},
    {"Im Menü: zeigen, klicken", "Maus, Enter", "Linker Stick, A", "Linker Stick, Kreuz"},
    {"Im Menü: auswählen", "Pfeiltasten", "Steuerkreuz", "Steuerkreuz"},
    {"Im Menü: zurück", "ESC", "B", "Kreis"},
    {"Optionen: Seite wechseln", "Tab", "LB / RB", "L1 / R1"},
};
constexpr int kControlCount = static_cast<int>(sizeof(kControls) / sizeof(kControls[0]));

// Linke Kante jeder Spalte als Anteil der Tabellenbreite (die letzte Spalte
// reicht bis zum Rand). Abgestimmt auf die längsten Einträge: Weil Schrift
// und Tabelle beide mit Scale() wachsen, passt es dann bei jeder Fenstergröße.
constexpr float kColumn[4] = {0.0f, 0.29f, 0.60f, 0.79f};

// ===========================================================================
// Maße (in Pixeln bei Fensterhöhe 900, werden mit Scale() multipliziert)
// ===========================================================================
constexpr float kPanelW = 1080.0f;   // Fensterbreite
constexpr float kPad = 40.0f;        // Innenabstand links/rechts
constexpr float kRowH = 52.0f;       // Höhe der wählbaren Zeilen (Ton, Grafik)
constexpr float kRowGap = 8.0f;      // Abstand unter jeder wählbaren Zeile
constexpr float kTabW = 220.0f;      // Breite eines Karteireiters
constexpr float kTabH = 46.0f;       // Höhe der Karteireiter
constexpr float kCtrlX = 300.0f;     // Abstand der Bedienelemente vom linken Zeilenrand
constexpr float kTableRowH = 26.0f;  // Zeilenhöhe der Steuerungstabelle
constexpr float kTableText = 18.0f;  // Schriftgröße der Tabelle
constexpr float kButtonH = 50.0f;    // Höhe des Knopfs "Zurück"

// Höhe des Inhalts der einzelnen Seiten (in derselben Reihenfolge wie in Draw):
//   Ton:       drei Zeilen, Hinweiszeile 42
//   Grafik:    drei Zeilen, Hinweiszeile 42
//   Steuerung: Hinweiszeile 30, Tabellenkopf 30, Tabellenzeilen
float SoundHeight() { return 3.0f * (kRowH + kRowGap) + 42.0f; }
float GraphicsHeight() { return 3.0f * (kRowH + kRowGap) + 42.0f; }
float ControlsHeight() { return 30.0f + 30.0f + kControlCount * kTableRowH; }

// Gesamthöhe des Fensters: Rand 24, Überschrift 64, Karteireiter, Abstand 20,
// die längste Seite, Abstand 16, Knopf, Rand 24.
float PanelHeight() {
    const float content = std::max(SoundHeight(), std::max(GraphicsHeight(), ControlsHeight()));
    return 24.0f + 64.0f + kTabH + 20.0f + content + 16.0f + kButtonH + 24.0f;
}

// Hintergrund einer wählbaren Zeile. Ausgewählt (Tastatur) oder unter der
// bewegten Maus: wie ein hervorgehobener Knopf (bräunlich, oranger Rahmen,
// oranger Streifen links), sonst nur eine dezente dunkle Fläche.
void DrawRow(Rectangle r, bool highlighted) {
    const float s = Scale();
    if (highlighted) {
        DrawRectangleRec(r, Color{70, 52, 30, 200});
        DrawRectangleLinesEx(r, Px(2.0f) < 1 ? 1 : Px(2.0f), ACCENT);
        DrawRectangleRec({r.x, r.y, 6 * s, r.height}, ACCENT);
    } else {
        DrawRectangleRec(r, Color{15, 18, 24, 150});
    }
}

// Hat sich der Zeiger (Maus oder Controller-Zeiger) in diesem Frame bewegt?
// Nur dann übernimmt die Zeile unter dem Zeiger die Tastaturauswahl (wie in
// ui::ButtonList) – ein ruhender Zeiger soll die Pfeiltasten nicht blockieren.
bool MouseMoved() { return PointerMoved(); }

} // namespace

// ===========================================================================
// Öffnen, Schließen, Aktionen
// ===========================================================================
void OptionsScreen::Open() {
    tab_ = TAB_SOUND;
    sel_ = ROW_MUSIC;
    frames_ = 0;
    dragging_ = false;
    draggingClip_ = false;
    draggingTilt_ = false;
    draggingMsaa_ = false;
    draggingFxaa_ = false;
}

void OptionsScreen::Close() {
    dragging_ = false;
    draggingClip_ = false;
    draggingTilt_ = false;
    draggingMsaa_ = false;
    draggingFxaa_ = false;
    g_clientConfig.SaveIfChanged();   // z. B. Lautstärke per Pfeiltasten geändert
    g_audio.Play(Sfx::UiBack);
}

// An/aus wird sofort gespeichert: Es ist ein einzelner Klick, und die Datei
// ist winzig.
void OptionsScreen::SetMusicOn(bool on) {
    g_audio.SetMusicEnabled(on);
    g_clientConfig.musicEnabled = on;
    g_clientConfig.Save();
}

// Rundung auf 5-%-Schritte: v * 20 zählt die Schritte (0,5 -> 10), round
// nimmt den nächsten ganzen Schritt, / 20 rechnet zurück. So steht in der
// Datei immer ein "glatter" Wert wie 0.35 und die Anzeige springt nicht
// zwischen 34 % und 35 % hin und her.
// Beim Ziehen des Reglers (save = false) würde jedes Frame eine Datei
// geschrieben; deshalb wird dort nur vorgemerkt und erst beim Loslassen bzw.
// beim Schließen gespeichert.
void OptionsScreen::SetVolume(float v, bool save) {
    v = std::round(v * 20.0f) / 20.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_audio.SetMusicVolume(v);
    g_clientConfig.musicVolume = v;
    if (save) g_clientConfig.Save();
    else g_clientConfig.unsaved = true;
}

// Wie SetVolume, für die Missions-Clips (ClipPlayer).
void OptionsScreen::SetClipVolume(float v, bool save) {
    v = std::round(v * 20.0f) / 20.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_clip.SetVolume(v);
    g_clientConfig.clipVolume = v;
    if (save) g_clientConfig.Save();
    else g_clientConfig.unsaved = true;
}

// Wie SetVolume, aber in 10-%-Schritten; wirkt ab dem nächsten Frame, weil
// TiltShift.cpp den Wert jedes Frame aus g_clientConfig liest.
void OptionsScreen::SetTiltShift(float v, bool save) {
    v = std::round(v * 10.0f) / 10.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_clientConfig.tiltShift = v;
    if (save) g_clientConfig.Save();
    else g_clientConfig.unsaved = true;
}

// Kantenglättung: Stufe 0..4 = Aus, 2x, 4x, 8x, 16x. In der Datei steht die
// Zahl der Abtastpunkte (0, 2, 4, 8, 16); MsaaLevel rechnet zurück.
int OptionsScreen::MsaaLevel(int samples) {
    int level = 0;
    while (level < 4 && (2 << level) <= samples) ++level;   // 2 -> 1, 4 -> 2, 8 -> 3, 16 -> 4
    return level;
}

void OptionsScreen::SetMsaaLevel(int level, bool save) {
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    g_clientConfig.msaa = level == 0 ? 0 : (1 << level);
    if (save) g_clientConfig.Save();
    else g_clientConfig.unsaved = true;
}

// FXAA-Stärke wie SetTiltShift in 10-%-Schritten (0 = aus).
void OptionsScreen::SetFxaa(float v, bool save) {
    v = std::round(v * 10.0f) / 10.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_clientConfig.fxaa = v;
    if (save) g_clientConfig.Save();
    else g_clientConfig.unsaved = true;
}

// ===========================================================================
// Zeichnen und Bedienen (Ablauf siehe Dateikopf)
// ===========================================================================
// Wählbare Zeilen der Seite tab von oben nach unten (Pfeil hoch/runter).
// Liefert die Anzahl; rows muss Platz für 5 Einträge haben.
int OptionsScreen::RowsOf(int tab, int* rows) {
    int n = 0;
    rows[n++] = ROW_TABS;
    if (tab == TAB_SOUND) {
        rows[n++] = ROW_MUSIC;
        rows[n++] = ROW_VOLUME;
        rows[n++] = ROW_CLIPS;
    } else if (tab == TAB_GRAPHICS) {
        rows[n++] = ROW_TILT;
        rows[n++] = ROW_MSAA;
        rows[n++] = ROW_FXAA;
    }
    rows[n++] = ROW_BACK;
    return n;
}

// Seite wechseln. Ziehen endet (der Regler ist dann nicht mehr zu sehen);
// keepTabsRow: Auswahl bleibt auf den Karteireitern (Pfeiltasten dort, Klick),
// sonst springt sie auf die erste Zeile der neuen Seite (Tab, LB/RB).
void OptionsScreen::SetTab(int tab, bool keepTabsRow) {
    tab_ = (tab + TAB_COUNT) % TAB_COUNT;
    dragging_ = draggingClip_ = draggingTilt_ = draggingMsaa_ = draggingFxaa_ = false;
    g_clientConfig.SaveIfChanged();
    int rows[5];
    RowsOf(tab_, rows);
    sel_ = keepTabsRow ? ROW_TABS : rows[1];
}

bool OptionsScreen::Draw() {
    ++frames_;
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const int before = sel_;
    const int tabBefore = tab_;

    // --- 1. Abdunkeln und Fenster ----------------------------------------------
    // Wie die Pause: Das Bild dahinter (Menü-Hintergrund oder angehaltenes
    // Spiel) bleibt schwach sichtbar.
    DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 150});
    const float pw = kPanelW * s, ph = PanelHeight() * s;
    const Rectangle panel{W * 0.5f - pw * 0.5f, H * 0.5f - ph * 0.5f, pw, ph};
    Panel(panel);
    const float x = panel.x + kPad * s;          // linker Rand des Inhalts
    const float iw = pw - 2.0f * kPad * s;        // Breite des Inhalts
    float y = panel.y + 24 * s;
    TextCentered("OPTIONEN", W * 0.5f, y, 46 * s, ACCENT);
    y += 64 * s;

    // --- 2. Tastatur -------------------------------------------------------------
    // Erst ab dem zweiten Frame: Die Taste, die die Optionen geöffnet hat
    // (Enter im Menü), gilt in ihrem ganzen Frame als "gedrückt" und dürfte
    // hier nicht gleich noch etwas auslösen (wie pauseFrames_ in main.cpp).
    // IsKeyPressedRepeat: true bei der automatischen Wiederholung des
    // Betriebssystems, solange eine Taste gehalten wird – so lässt sich die
    // Lautstärke durch Festhalten von Pfeil rechts/links zügig verstellen.
    // Controller (Menümodus, Ui.hpp): Steuerkreuz wie die Pfeiltasten; A wie
    // Enter – außer der Controller-Zeiger steht auf einer Zeile oder dem
    // Knopf "Zurück", dann klickt A dort (ausgewertet unten bei "Auswerten").
    // Seite wechseln: Tab (mit Umschalt rückwärts) bzw. LB/RB auf jeder Zeile,
    // auf den Karteireitern auch Pfeil links/rechts.
    const bool keys = frames_ > 1;
    bool toggle = false, close = false;
    float volStep = 0.0f, clipStep = 0.0f, tiltStep = 0.0f, fxaaStep = 0.0f;
    int msaaStep = 0;
    const bool padA = keys && PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    if (keys) {
        if (IsKeyPressed(KEY_TAB))
            SetTab(tab_ + ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? -1 : 1), sel_ == ROW_TABS);
        if (PadPressed(GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) SetTab(tab_ + 1, sel_ == ROW_TABS);
        if (PadPressed(GAMEPAD_BUTTON_LEFT_TRIGGER_1)) SetTab(tab_ - 1, sel_ == ROW_TABS);
        // Pfeil hoch/runter: nur durch die Zeilen der aktuellen Seite
        int rows[5];
        const int n = RowsOf(tab_, rows);
        int at = 0;
        while (at < n - 1 && rows[at] != sel_) ++at;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN)) sel_ = rows[(at + 1) % n];
        // + n - 1 statt - 1: bleibt positiv (in C++ ist -1 % 3 = -1)
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_UP)) sel_ = rows[(at + n - 1) % n];
        // Pfeil rechts/links: verstellt den Regler der gewählten Zeile bzw.
        // wechselt auf den Karteireitern die Seite
        int dir = 0;
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT) || IsKeyPressed(KEY_D) || IsKeyPressedRepeat(KEY_D) ||
            PadPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            dir = 1;
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT) || IsKeyPressed(KEY_A) || IsKeyPressedRepeat(KEY_A) ||
            PadPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            dir = -1;
        if (sel_ == ROW_TABS && dir != 0) SetTab(tab_ + dir, true);
        if (sel_ == ROW_VOLUME) volStep = 0.05f * dir;
        if (sel_ == ROW_CLIPS) clipStep = 0.05f * dir;
        if (sel_ == ROW_TILT) tiltStep = 0.1f * dir;
        if (sel_ == ROW_MSAA) msaaStep = dir;
        if (sel_ == ROW_FXAA) fxaaStep = 0.1f * dir;
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE)) {
            if (sel_ == ROW_MUSIC) toggle = true;
            else if (sel_ == ROW_BACK) close = true;
        }
        if (BackPressed()) close = true;
    }

    // --- 3. Karteireiter ---------------------------------------------------------------
    // Gewählte Seite: orange Fläche mit dunkler Schrift, die anderen dunkel.
    // Stehen die Pfeiltasten auf den Karteireitern, hat der gewählte einen
    // hellen Rahmen; unter der Maus wird ein Reiter aufgehellt.
    const Vector2 mouse = PointerPos();
    const char* tabNames[TAB_COUNT] = {"Ton", "Grafik", "Steuerung"};
    bool overTabs = false;
    for (int t = 0; t < TAB_COUNT; ++t) {
        const Rectangle r{x + t * (kTabW + 10.0f) * s, y, kTabW * s, kTabH * s};
        const bool hover = CheckCollisionPointRec(mouse, r);
        if (hover) {
            overTabs = true;
            if (MouseMoved()) sel_ = ROW_TABS;
            if (PointerPressed() && t != tab_) SetTab(t, true);
        }
        const bool current = t == tab_;
        Color fill = current ? ACCENT : (hover ? PANEL_LIGHT : Color{15, 18, 24, 200});
        DrawRectangleRec(r, fill);
        if (current && sel_ == ROW_TABS) DrawRectangleLinesEx(r, std::max(2.0f, 3.0f * s), TEXT);
        else if (!current) DrawRectangleLinesEx(r, 1.0f, ACCENT_DIM);
        TextCentered(tabNames[t], r.x + r.width * 0.5f, r.y + (r.height - 24 * s) * 0.5f, 24 * s,
                     current ? Color{26, 31, 41, 255} : TEXT, !current);
    }
    // Linie unter den Reitern über die ganze Breite
    DrawRectangleRec({x, y + kTabH * s, iw, std::max(2.0f, 3.0f * s)}, ACCENT);
    y += (kTabH + 20.0f) * s;
    const float contentY = y;

    // --- 4. Inhalt der gewählten Seite -------------------------------------------------
    const float label = 24 * s;                   // Schriftgröße der Zeilenbeschriftung
    const float labelY = (kRowH * s - label) * 0.5f;   // senkrecht mittig in der Zeile
    bool overRow = false;                         // Zeiger auf einer Zeile (für Controller-A)

    if (tab_ == TAB_SOUND) {
        // Zeile "Musik": Klick auf den Schalter ODER irgendwo in die Zeile schaltet um.
        const Rectangle rowMusic{x, y, iw, kRowH * s};
        const bool overMusic = CheckCollisionPointRec(mouse, rowMusic);
        if (overMusic && MouseMoved()) sel_ = ROW_MUSIC;
        DrawRow(rowMusic, sel_ == ROW_MUSIC);
        Text("Musik", x + 22 * s, y + labelY, label, TEXT);
        const Rectangle sw{x + kCtrlX * s, y + 9 * s, 120 * s, kRowH * s - 18 * s};
        const bool switchClicked = Toggle(sw, g_audio.MusicEnabled(), 20 * s, sel_ == ROW_MUSIC);
        if (switchClicked || (overMusic && PointerPressed())) {
            toggle = true;
            sel_ = ROW_MUSIC;
        }
        Text("Menü: \"Main Menu Theme\"  ·  Spiel: \"Lunar Silence 01\"", sw.x + sw.width + 30 * s, y + (kRowH * s - 17 * s) * 0.5f,
             17 * s, MUTED);
        y += (kRowH + kRowGap) * s;

        // Zeile "Lautstärke": Schieberegler plus Prozentanzeige.
        const Rectangle rowVol{x, y, iw, kRowH * s};
        const bool overVol = CheckCollisionPointRec(mouse, rowVol);
        if (overVol && MouseMoved()) sel_ = ROW_VOLUME;
        DrawRow(rowVol, sel_ == ROW_VOLUME || dragging_);
        Text("Musiklautstärke", x + 22 * s, y + labelY, label, TEXT);
        const Rectangle slider{x + kCtrlX * s, y + 6 * s, 440 * s, kRowH * s - 12 * s};
        float vol = g_audio.MusicVolume();
        const bool wasDragging = dragging_;
        if (Slider(slider, vol, dragging_, sel_ == ROW_VOLUME)) {
            SetVolume(vol, false);
            g_audio.Play(Sfx::UiMove, 0.6f);   // leises Ticken pro 5-%-Schritt
        }
        if (dragging_) sel_ = ROW_VOLUME;
        if (wasDragging && !dragging_) g_clientConfig.SaveIfChanged();   // Regler losgelassen
        // Prozent: lround rundet auf die nächste ganze Zahl (0,35 -> 35, auch wenn
        // intern 0,3500001 steht).
        const std::string pct = std::to_string(std::lround(g_audio.MusicVolume() * 100.0f)) + " %";
        Text(pct, slider.x + slider.width + 30 * s, y + labelY, label, g_audio.MusicEnabled() ? TEXT : MUTED);
        if (!g_audio.MusicEnabled())
            Text("(Musik ist aus)", slider.x + slider.width + 120 * s, y + (kRowH * s - 17 * s) * 0.5f, 17 * s, MUTED);
        y += (kRowH + kRowGap) * s;

        // Zeile "Clip-Lautstärke": wie die Musiklautstärke, für die
        // Funksprüche der Missionen (ClipPlayer).
        const Rectangle rowClip{x, y, iw, kRowH * s};
        const bool overClip = CheckCollisionPointRec(mouse, rowClip);
        if (overClip && MouseMoved()) sel_ = ROW_CLIPS;
        DrawRow(rowClip, sel_ == ROW_CLIPS || draggingClip_);
        Text("Clip-Lautstärke", x + 22 * s, y + labelY, label, TEXT);
        const Rectangle clipSlider{x + kCtrlX * s, y + 6 * s, 440 * s, kRowH * s - 12 * s};
        float clipVol = g_clientConfig.clipVolume;
        const bool wasDraggingClip = draggingClip_;
        if (Slider(clipSlider, clipVol, draggingClip_, sel_ == ROW_CLIPS)) {
            SetClipVolume(clipVol, false);
            g_audio.Play(Sfx::UiMove, 0.6f);
        }
        if (draggingClip_) sel_ = ROW_CLIPS;
        if (wasDraggingClip && !draggingClip_) g_clientConfig.SaveIfChanged();
        Text(std::to_string(std::lround(g_clientConfig.clipVolume * 100.0f)) + " %", clipSlider.x + clipSlider.width + 30 * s,
             y + labelY, label, TEXT);
        y += (kRowH + kRowGap) * s;

        // Hinweis zur Taste M; ist der Ton gerade stumm, auffälliger in Orange –
        // sonst wundert man sich, warum der Regler nichts bewirkt.
        if (g_audio.Muted())
            Text("Der Ton ist gerade stumm geschaltet. Taste M schaltet ihn wieder ein.", x, y + 4 * s, 17 * s, ACCENT);
        else
            Text("Taste M schaltet jederzeit den ganzen Ton stumm (Effekte, Musik und Clips).", x, y + 4 * s, 17 * s, MUTED);
        overRow = overMusic || overVol || overClip;
    } else if (tab_ == TAB_GRAPHICS) {
        // Zeile "Tilt-Shift": Schieberegler plus Prozentanzeige (0 = "Aus").
        const Rectangle rowTilt{x, y, iw, kRowH * s};
        const bool overTilt = CheckCollisionPointRec(mouse, rowTilt);
        if (overTilt && MouseMoved()) sel_ = ROW_TILT;
        DrawRow(rowTilt, sel_ == ROW_TILT || draggingTilt_);
        Text("Tilt-Shift", x + 22 * s, y + labelY, label, TEXT);
        const Rectangle tiltSlider{x + kCtrlX * s, y + 6 * s, 440 * s, kRowH * s - 12 * s};
        float tilt = g_clientConfig.tiltShift;
        const bool wasDraggingTilt = draggingTilt_;
        if (Slider(tiltSlider, tilt, draggingTilt_, sel_ == ROW_TILT, 0.1f)) {
            SetTiltShift(tilt, false);
            g_audio.Play(Sfx::UiMove, 0.6f);
        }
        if (draggingTilt_) sel_ = ROW_TILT;
        if (wasDraggingTilt && !draggingTilt_) g_clientConfig.SaveIfChanged();
        const std::string tiltText =
            g_clientConfig.tiltShift > 0.0f ? std::to_string(std::lround(g_clientConfig.tiltShift * 100.0f)) + " %" : std::string("Aus");
        Text(tiltText, tiltSlider.x + tiltSlider.width + 30 * s, y + labelY, label, g_clientConfig.tiltShift > 0.0f ? TEXT : MUTED);
        y += (kRowH + kRowGap) * s;

        // Zeile "Kantenglättung": Regler mit fünf Stufen Aus, 2x, 4x, 8x, 16x
        // (Wert 0, 0.25, 0.5, 0.75, 1). Kann die Grafikkarte die Stufe nicht,
        // steht daneben, wie viel sie kann (TiltShift begrenzt dann darauf).
        const Rectangle rowMsaa{x, y, iw, kRowH * s};
        const bool overMsaa = CheckCollisionPointRec(mouse, rowMsaa);
        if (overMsaa && MouseMoved()) sel_ = ROW_MSAA;
        DrawRow(rowMsaa, sel_ == ROW_MSAA || draggingMsaa_);
        Text("Kantenglättung", x + 22 * s, y + labelY, label, TEXT);
        const Rectangle msaaSlider{x + kCtrlX * s, y + 6 * s, 440 * s, kRowH * s - 12 * s};
        float msaaValue = MsaaLevel(g_clientConfig.msaa) * 0.25f;
        const bool wasDraggingMsaa = draggingMsaa_;
        if (Slider(msaaSlider, msaaValue, draggingMsaa_, sel_ == ROW_MSAA, 0.25f)) {
            SetMsaaLevel(static_cast<int>(std::lround(msaaValue * 4.0f)), false);
            g_audio.Play(Sfx::UiMove, 0.6f);
        }
        if (draggingMsaa_) sel_ = ROW_MSAA;
        if (wasDraggingMsaa && !draggingMsaa_) g_clientConfig.SaveIfChanged();
        const std::string msaaText = g_clientConfig.msaa > 0 ? std::to_string(g_clientConfig.msaa) + "x" : std::string("Aus");
        Text(msaaText, msaaSlider.x + msaaSlider.width + 30 * s, y + labelY, label, g_clientConfig.msaa > 0 ? TEXT : MUTED);
        const int maxSamples = tiltshift::MaxSamples();
        if (g_clientConfig.msaa > maxSamples) {
            const std::string note = maxSamples > 0 ? "(GPU max. " + std::to_string(maxSamples) + "x)" : std::string("(nicht möglich)");
            Text(note, msaaSlider.x + msaaSlider.width + 100 * s, y + (kRowH * s - 17 * s) * 0.5f, 17 * s, MUTED);
        }
        y += (kRowH + kRowGap) * s;

        // Zeile "FXAA": Glätten per Shader, Stärke in 10-%-Schritten (0 = "Aus").
        const Rectangle rowFxaa{x, y, iw, kRowH * s};
        const bool overFxaa = CheckCollisionPointRec(mouse, rowFxaa);
        if (overFxaa && MouseMoved()) sel_ = ROW_FXAA;
        DrawRow(rowFxaa, sel_ == ROW_FXAA || draggingFxaa_);
        Text("FXAA", x + 22 * s, y + labelY, label, TEXT);
        const Rectangle fxaaSlider{x + kCtrlX * s, y + 6 * s, 440 * s, kRowH * s - 12 * s};
        float fxaa = g_clientConfig.fxaa;
        const bool wasDraggingFxaa = draggingFxaa_;
        if (Slider(fxaaSlider, fxaa, draggingFxaa_, sel_ == ROW_FXAA, 0.1f)) {
            SetFxaa(fxaa, false);
            g_audio.Play(Sfx::UiMove, 0.6f);
        }
        if (draggingFxaa_) sel_ = ROW_FXAA;
        if (wasDraggingFxaa && !draggingFxaa_) g_clientConfig.SaveIfChanged();
        const std::string fxaaText =
            g_clientConfig.fxaa > 0.0f ? std::to_string(std::lround(g_clientConfig.fxaa * 100.0f)) + " %" : std::string("Aus");
        Text(fxaaText, fxaaSlider.x + fxaaSlider.width + 30 * s, y + labelY, label, g_clientConfig.fxaa > 0.0f ? TEXT : MUTED);
        y += (kRowH + kRowGap) * s;

        Text("Wirkt auf die 3D-Welt; HUD und Menüs bleiben scharf. Kantenglättung und FXAA gehen auch zusammen.", x, y + 4 * s,
             17 * s, MUTED);
        overRow = overTilt || overMsaa || overFxaa;
    } else {
        // Steuerung: nur Anzeige
        Text("Tastatur, Maus und Controller gehen jederzeit, auch gemischt.", x, y + 2 * s, 17 * s, MUTED);
        y += 30 * s;
        const float ts = kTableText * s;
        const float rowH = kTableRowH * s;
        const float textY = (rowH - ts) * 0.5f;       // Text senkrecht mittig in der Tabellenzeile
        const char* head[4] = {"Aktion", "Tastatur / Maus", "Xbox", "PlayStation"};
        for (int c = 0; c < 4; ++c) Text(head[c], x + 10 * s + kColumn[c] * iw, y + textY, ts, ACCENT);
        DrawRectangleRec({x, y + rowH + 1 * s, iw, std::max(1.0f, 1.5f * s)}, ACCENT_DIM);   // Linie unter dem Kopf
        y += 30 * s;
        for (int i = 0; i < kControlCount; ++i) {
            const float ry = y + i * rowH;
            // Jede zweite Zeile leicht aufgehellt ("Zebrastreifen"): Das Auge
            // findet beim Lesen quer über die Spalten leichter die richtige Zeile.
            if (i % 2 == 0) DrawRectangleRec({x, ry, iw, rowH}, Color{255, 255, 255, 10});
            const char* cells[4] = {kControls[i].action, kControls[i].keyboard, kControls[i].xbox, kControls[i].playstation};
            for (int c = 0; c < 4; ++c) {
                const bool none = cells[c][0] == '-' && cells[c][1] == '\0';
                const Color col = c == 0 ? TEXT : (none ? Color{100, 106, 120, 255} : Color{255, 200, 140, 230});
                Text(cells[c], x + 10 * s + kColumn[c] * iw, ry + textY, ts, col);
            }
        }
    }
    // Knopf immer an derselben Stelle, unter der längsten Seite
    y = contentY + std::max(SoundHeight(), std::max(GraphicsHeight(), ControlsHeight())) * s + 16 * s;

    // --- 5. Zurück ------------------------------------------------------------------
    // Links daneben eine kurze Tastenhilfe für die Optionen selbst.
    const Rectangle back{x + iw - 260 * s, y, 260 * s, kButtonH * s};
    bool backHover = false;
    if (Button(back, "Zurück", 24 * s, sel_ == ROW_BACK, true, &backHover)) close = true;
    if (backHover && MouseMoved()) sel_ = ROW_BACK;
    // Hilfezeile passend zum zuletzt benutzten Gerät (Controls.hpp)
    const controls::Device dev = controls::ActiveDevice();
    const std::string help = dev == controls::Device::KeyboardMouse
                                 ? std::string("Tab: Seite    Pfeiltasten: wählen, Regler    Enter: umschalten    ESC: zurück")
                                 : std::string(dev == controls::Device::PlayStation ? "L1/R1" : "LB/RB") +
                                       ": Seite    Steuerkreuz: wählen, Regler    " + controls::KeyName(controls::Act::Confirm) +
                                       ": umschalten    " + controls::KeyName(controls::Act::Back) + ": zurück";
    Text(help, x, y + (kButtonH * s - 16 * s) * 0.5f, 16 * s, MUTED);

    // --- Auswerten ------------------------------------------------------------------
    // Controller-A ohne Zeiger auf einem Bedienelement: wie Enter
    if (padA && !(PadPointerActive() && (overRow || overTabs || backHover))) {
        if (sel_ == ROW_MUSIC) toggle = true;
        else if (sel_ == ROW_BACK) close = true;
    }
    if (volStep != 0.0f) {
        SetVolume(g_audio.MusicVolume() + volStep, false);
        g_audio.Play(Sfx::UiMove, 0.6f);
    }
    if (clipStep != 0.0f) {
        SetClipVolume(g_clientConfig.clipVolume + clipStep, false);
        g_audio.Play(Sfx::UiMove, 0.6f);
    }
    if (tiltStep != 0.0f) {
        SetTiltShift(g_clientConfig.tiltShift + tiltStep, false);
        g_audio.Play(Sfx::UiMove, 0.6f);
    }
    if (msaaStep != 0) {
        SetMsaaLevel(MsaaLevel(g_clientConfig.msaa) + msaaStep, false);
        g_audio.Play(Sfx::UiMove, 0.6f);
    }
    if (fxaaStep != 0.0f) {
        SetFxaa(g_clientConfig.fxaa + fxaaStep, false);
        g_audio.Play(Sfx::UiMove, 0.6f);
    }
    if (toggle) {
        SetMusicOn(!g_audio.MusicEnabled());
        g_audio.Play(Sfx::UiConfirm);
    }
    if ((sel_ != before || tab_ != tabBefore) && !toggle) g_audio.Play(Sfx::UiMove);
    if (close) {
        Close();
        return true;
    }
    return false;
}
