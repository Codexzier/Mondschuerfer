// ---------------------------------------------------------------------------
// Options.cpp – Bildschirm "Optionen": Musik an/aus, Musiklautstärke und eine
// Übersicht der Steuerung (Schnittstelle und Bedienung: Options.hpp).
//
// Wer benutzt die Datei? main.cpp (Hauptmenü, Einzelspieler-Pause) und
// Multiplayer.cpp (Mehrspieler-Pause), jeweils über ein OptionsScreen-Objekt.
//
// Abhängigkeiten:
//   - Ui.hpp: Panel, Button, Slider, Toggle, Texte, Farben, Scale()
//   - Audio.hpp: g_audio (Musik an/aus und Lautstärke wirken sofort; Töne
//     beim Bedienen)
//   - ClientConfig.hpp: g_clientConfig (Werte in der Einstellungsdatei)
//   - raylib: Tastatur, Mausposition, Zeichnen
//
// Ablauf von Draw() (jedes Frame, "immediate mode" wie in Ui.hpp):
//   1. Bild abdunkeln, Fenster (Panel) mittig zeichnen. Alle Maße sind für
//      eine Fensterhöhe von 900 Pixeln gedacht und werden mit s = Scale()
//      multipliziert; so passt das Fenster von 960 x 540 bis Vollbild.
//   2. Tastatur: Zeile wählen, Lautstärke ändern, umschalten, zurück.
//   3. Abschnitt "Musik": Umschalter und Schieberegler zeichnen und dabei
//      die Maus auswerten.
//   4. Abschnitt "Steuerung": feste Tabelle (kControls unten).
//   5. Knopf "Zurück".
// Wo sich ein Wert ändert, laufen alle Wege (Maus, Tastatur, Testoption)
// über SetMusicOn bzw. SetVolume: Dort wird der Wert an g_audio gegeben,
// in g_clientConfig eingetragen und gespeichert.
//
// Alles läuft im Spiel-Thread.
// ---------------------------------------------------------------------------
#include "Options.hpp"
#include "Audio.hpp"
#include "ClientConfig.hpp"
#include "Ui.hpp"
#include "raylib.h"
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
// Ausgewertet werden die Tasten in main.cpp und Multiplayer.cpp; diese Tabelle
// muss dazu passen.
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
    {"Im Menü: zurück", "ESC", "B", "Kreis"},
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
constexpr float kRowH = 52.0f;       // Höhe der Zeilen "Musik" und "Lautstärke"
constexpr float kCtrlX = 300.0f;     // Abstand der Bedienelemente vom linken Zeilenrand
constexpr float kTableRowH = 26.0f;  // Zeilenhöhe der Steuerungstabelle
constexpr float kTableText = 18.0f;  // Schriftgröße der Tabelle
constexpr float kButtonH = 50.0f;    // Höhe des Knopfs "Zurück"

// Gesamthöhe des Fensters, Summe der Abschnitte von oben nach unten (in
// derselben Reihenfolge wie in Draw): Rand 24, Überschrift 64, "MUSIK" 30,
// zwei Zeilen à 52 + 8 Abstand, Hinweiszeile 42, "STEUERUNG" 30, Tabellenkopf
// 30, Tabellenzeilen, Abstand 16, Knopf, Rand 24.
float PanelHeight() {
    return 24.0f + 64.0f + 30.0f + 2.0f * (kRowH + 8.0f) + 42.0f + 30.0f + 30.0f + kControlCount * kTableRowH + 16.0f +
           kButtonH + 24.0f;
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

// Hat sich die Maus in diesem Frame bewegt? Nur dann übernimmt die Zeile
// unter dem Zeiger die Tastaturauswahl (wie in ui::ButtonList) – ein ruhender
// Zeiger soll die Pfeiltasten nicht blockieren.
bool MouseMoved() {
    const Vector2 d = GetMouseDelta();
    return d.x != 0.0f || d.y != 0.0f;
}

} // namespace

// ===========================================================================
// Öffnen, Schließen, Aktionen
// ===========================================================================
void OptionsScreen::Open() {
    sel_ = ROW_MUSIC;
    frames_ = 0;
    dragging_ = false;
}

void OptionsScreen::Close() {
    dragging_ = false;
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

// ===========================================================================
// Zeichnen und Bedienen (Ablauf siehe Dateikopf)
// ===========================================================================
bool OptionsScreen::Draw() {
    ++frames_;
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const int before = sel_;

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
    const bool keys = frames_ > 1;
    bool toggle = false, close = false;
    float volStep = 0.0f;
    if (keys) {
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) sel_ = (sel_ + 1) % ROW_COUNT;
        // + ROW_COUNT - 1 statt - 1: bleibt positiv (in C++ ist -1 % 3 = -1)
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) sel_ = (sel_ + ROW_COUNT - 1) % ROW_COUNT;
        if (sel_ == ROW_VOLUME) {
            if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT) || IsKeyPressed(KEY_D) || IsKeyPressedRepeat(KEY_D))
                volStep = 0.05f;
            if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT) || IsKeyPressed(KEY_A) || IsKeyPressedRepeat(KEY_A))
                volStep = -0.05f;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE)) {
            if (sel_ == ROW_MUSIC) toggle = true;
            else if (sel_ == ROW_BACK) close = true;
        }
        if (BackPressed()) close = true;
    }

    // --- 3. Abschnitt Musik -----------------------------------------------------------
    Text("MUSIK", x, y, 20 * s, MUTED);
    y += 30 * s;
    const Vector2 mouse = GetMousePosition();
    const float label = 24 * s;                   // Schriftgröße der Zeilenbeschriftung
    const float labelY = (kRowH * s - label) * 0.5f;   // senkrecht mittig in der Zeile

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
    y += (kRowH + 8) * s;

    // Zeile "Lautstärke": Schieberegler plus Prozentanzeige.
    const Rectangle rowVol{x, y, iw, kRowH * s};
    if (CheckCollisionPointRec(mouse, rowVol) && MouseMoved()) sel_ = ROW_VOLUME;
    DrawRow(rowVol, sel_ == ROW_VOLUME || dragging_);
    Text("Lautstärke", x + 22 * s, y + labelY, label, TEXT);
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
    y += (kRowH + 8) * s;

    // Hinweis zur Taste M; ist der Ton gerade stumm, auffälliger in Orange –
    // sonst wundert man sich, warum der Regler nichts bewirkt.
    if (g_audio.Muted())
        Text("Der Ton ist gerade stumm geschaltet. Taste M schaltet ihn wieder ein.", x, y + 4 * s, 17 * s, ACCENT);
    else
        Text("Taste M schaltet jederzeit den ganzen Ton stumm (Effekte und Musik).", x, y + 4 * s, 17 * s, MUTED);
    y += 42 * s;

    // --- 4. Abschnitt Steuerung --------------------------------------------------------
    Text("STEUERUNG", x, y, 20 * s, MUTED);
    TextRight("Tastatur, Maus und Controller gehen jederzeit, auch gemischt.", x + iw, y + 2 * s, 17 * s, MUTED);
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
    y += kControlCount * rowH + 16 * s;

    // --- 5. Zurück ------------------------------------------------------------------
    // Links daneben eine kurze Tastenhilfe für die Optionen selbst.
    const Rectangle back{x + iw - 260 * s, y, 260 * s, kButtonH * s};
    bool backHover = false;
    if (Button(back, "Zurück", 24 * s, sel_ == ROW_BACK, true, &backHover)) close = true;
    if (backHover && MouseMoved()) sel_ = ROW_BACK;
    Text("Pfeiltasten: wählen, Lautstärke    Enter: umschalten    ESC: zurück", x, y + (kButtonH * s - 16 * s) * 0.5f, 16 * s,
         MUTED);

    // --- Auswerten ------------------------------------------------------------------
    if (volStep != 0.0f) {
        SetVolume(g_audio.MusicVolume() + volStep, false);
        g_audio.Play(Sfx::UiMove, 0.6f);
    }
    if (toggle) {
        SetMusicOn(!g_audio.MusicEnabled());
        g_audio.Play(Sfx::UiConfirm);
    }
    if (sel_ != before && !toggle) g_audio.Play(Sfx::UiMove);
    if (close) {
        Close();
        return true;
    }
    return false;
}
