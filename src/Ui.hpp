#pragma once
// ---------------------------------------------------------------------------
// Ui.hpp – einfache 2D-Bausteine für Menüs und HUD (Schnittstelle zu Ui.cpp).
//
// Wofür ist die Datei da?
//   Texte (mit Schatten, zentriert, rechtsbündig), Panels, Balken, Knöpfe,
//   senkrechte Knopflisten, einzeilige Textfelder und zwei Zahlenformate
//   (Tausenderpunkte, Uhrzeit). Dazu die Farbpalette des Spiels, angelehnt an
//   den Stil der Kenney-Grafiken (dunkle Panels, orangefarbene Akzente).
//
// Wer benutzt sie?
//   src/main.cpp (Hauptmenü, Pause), src/Hud.cpp (HUD, Stationsfenster,
//   Endbildschirme) und src/Multiplayer.cpp (Mehrspieler-Menüs, Anmeldung).
//
// Abhängigkeiten:
//   Nur raylib (Zeichnen, Maus, Tastatur, eingebaute Schrift) und für die
//   Bedientöne Audio.hpp (in Ui.cpp).
//
// Grundidee "immediate mode" (sofortiger Modus):
//   Es gibt keine Fenster- oder Knopf-Objekte, die irgendwo gespeichert sind.
//   Stattdessen ruft der Aufrufer JEDES FRAME die Funktionen auf; jede
//   zeichnet sofort und wertet gleichzeitig Maus und Tastatur dieses Frames
//   aus (Button liefert z. B. direkt true, wenn er gerade angeklickt wurde).
//   Was ausgewählt ist o. Ä., hält der Aufrufer selbst (z. B. "selected").
//   Das passt gut zu raylib, das ohnehin jedes Bild komplett neu zeichnet.
//
// Voraussetzungen: Aufruf nur im Spiel-Thread, zwischen BeginDrawing() und
// EndDrawing() und außerhalb von BeginMode3D() (also in Pixelkoordinaten,
// 0,0 = links oben). ui::BeginFrame() einmal am Anfang jedes Frames.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include <string>
#include <vector>

// Einfache UI-Bausteine (Texte, Panels, Balken, Knöpfe) im Kenney-Farbstil.
namespace ui {

// Farbpalette. raylib-Farben sind {Rot, Grün, Blau, Alpha}, je 0..255;
// Alpha = Deckkraft (255 = undurchsichtig). Die Panels sind knapp unter 255,
// damit die 3D-Welt dahinter ein wenig durchscheint.
constexpr Color ACCENT{255, 160, 51, 255};      // Orange: Hervorhebungen, Kontostand, Handelsstation
constexpr Color ACCENT_DIM{200, 120, 40, 255};  // gedämpftes Orange: Panel-Rahmen
constexpr Color PANEL{26, 31, 41, 238};         // Hintergrund der Panels
constexpr Color PANEL_LIGHT{44, 51, 64, 235};   // Hintergrund der Knöpfe
constexpr Color TEXT{236, 239, 245, 255};       // normaler Text
constexpr Color MUTED{150, 158, 176, 255};      // Beschriftungen, Nebensächliches
constexpr Color GOOD{110, 240, 170, 255};       // Grün: Erfolg, Kristalle, andere Spieler
constexpr Color BAD{255, 105, 90, 255};         // Rot: Gefahr, Gegner
constexpr Color SHIELD{95, 175, 255, 255};      // Blau: Schildbalken
constexpr Color SCAN{70, 230, 170, 255};        // Türkis: Scannerbalken

void BeginFrame();                   // einmal pro Frame aufrufen (main.cpp, App::Frame)
// Skalierungsfaktor für alle Pixelmaße: Die Oberfläche ist für 1500 x 900
// Pixel entworfen. Ergebnis = min(Höhe / 900, Breite / 1500), aber nie
// kleiner als 0,55 (sonst wird die Schrift unlesbar).
float Scale();
float Px(float v);                   // v * Scale()

// Texte mit raylibs eingebauter Pixelschrift. size = Schrifthöhe in Pixeln.
// Width misst die Breite in Pixeln (zum Zentrieren/Rechtsbündig-Setzen).
// shadow = dunkler, leicht versetzter Schatten für Lesbarkeit über der Welt.
float Width(const std::string& t, float size);
void Text(const std::string& t, float x, float y, float size, Color c, bool shadow = true);
void TextCentered(const std::string& t, float cx, float y, float size, Color c, bool shadow = true);
void TextRight(const std::string& t, float rx, float y, float size, Color c, bool shadow = true);

// Panel = gefülltes Rechteck mit Rahmen und orangefarbenen Eck-Akzenten.
// Bar = Fortschrittsbalken; frac ist der Füllstand 0..1 (wird begrenzt).
void Panel(Rectangle r, Color fill = PANEL, Color border = ACCENT_DIM);
void Bar(Rectangle r, float frac, Color fill, Color back = Color{15, 18, 24, 220});

// Knopf: liefert true beim Anklicken. "focused" = per Tastatur ausgewählt.
// enabled = false: grau, nicht anklickbar (liefert nie true).
// hovered (optional): erfährt, ob die Maus über dem Knopf ist – auch bei
// gesperrtem Knopf, z. B. um dann den Ton "Abgelehnt" zu spielen (Hud.cpp).
// Tastatur (Enter usw.) wertet Button NICHT aus; das macht der Aufrufer.
bool Button(Rectangle r, const std::string& label, float size, bool focused, bool enabled = true,
            bool* hovered = nullptr);

// Senkrechte Knopfliste mit Tastatur- und Maussteuerung. Liefert gewählten Index oder -1.
// selected: per Tastatur markierter Eintrag; gehört dem Aufrufer und muss
// über die Frames hinweg erhalten bleiben (z. B. menuSel_ in main.cpp).
// enabled: pro Eintrag, ob wählbar (fehlende Einträge gelten als wählbar).
// cx = Mitte, top = Oberkante des ersten Knopfs, height/gap = Knopfhöhe und
// Abstand, alles in Pixeln. Steuerung: Pfeiltasten oder W/S, Enter/Leertaste
// oder Mausklick.
int ButtonList(const std::vector<std::string>& items, const std::vector<bool>& enabled, int& selected,
               float cx, float top, float width, float height, float gap, float textSize);

// ---------------------------------------------------------------------------
// Einzeiliges Texteingabefeld (Mehrspieler: Serveradresse, Name, Passwort).
//
// Wie die anderen Bausteine hier ist es "immediate mode": Es gibt kein
// Ereignissystem, sondern der Aufrufer ruft jedes Frame Draw() auf. Draw()
// zeichnet das Feld UND verarbeitet dabei Maus und Tastatur:
//   - Klick ins Feld gibt ihm den Fokus, Klick daneben nimmt ihn weg.
//     Mit Tab wechselt der Aufrufer zwischen mehreren Feldern (CycleFocus).
//   - Getippte Zeichen kommen aus raylibs Zeichen-Warteschlange
//     (GetCharPressed). Das sind Unicode-"Codepoints" (z. B. 'ä' = 228); sie
//     werden als UTF-8 in text gespeichert, so wie alle Texte im Spiel.
//   - Rücktaste löscht ein Zeichen, gedrückt halten wiederholt das Löschen.
//   - Strg+V fügt Text aus der Zwischenablage ein.
// Nur das Feld mit Fokus liest die Tastatur. Solange eines den Fokus hat,
// liefert TextInputActive() true – main.cpp benutzt das, damit z. B. die
// Taste M beim Tippen eines Namens nicht den Ton ausschaltet.
// Die TextField-Objekte selbst leben beim Aufrufer (Multiplayer.cpp), weil
// Inhalt und Fokus über viele Frames erhalten bleiben müssen.
// ---------------------------------------------------------------------------
struct TextField {
    std::string text;                          // Inhalt (UTF-8)
    std::string placeholder;                   // grauer Hinweis, solange das Feld leer ist
    int maxChars = 64;                         // Höchstlänge in Zeichen (nicht in Bytes)
    bool password = false;                     // Inhalt als **** anzeigen
    bool focused = false;                      // bekommt die Tastatureingaben
    bool (*accept)(int codepoint) = nullptr;   // optionaler Filter: welche Zeichen sind erlaubt?

    // Zeichnet das Feld im Rechteck r und verarbeitet die Eingaben.
    // enabled = false: nur Anzeige (z. B. während die Anmeldung läuft).
    // Liefert true, wenn das Feld den Fokus hat und Enter gedrückt wurde.
    bool Draw(Rectangle r, float textSize, bool enabled = true);

    double lastEdit = -10.0;                   // intern: Cursor nach dem Tippen sofort sichtbar
    float repeatT = 0.0f;                      // intern: Wiederholung der Rücktaste
};

// Tab / Umschalt+Tab: Fokus zum nächsten / vorigen Feld der Liste.
// Hat keines den Fokus, bekommt ihn das erste.
void CycleFocus(const std::vector<TextField*>& fields);

// Hatte im letzten Frame ein Textfeld den Fokus? (Dann sind Tasten Text, keine Befehle.)
bool TextInputActive();

// Anzahl Zeichen (Codepoints) eines UTF-8-Texts
int Utf8Length(const std::string& s);

std::string Num(long long v);          // 100.000 (deutsche Tausenderpunkte)
std::string Time(float seconds);       // 12:34, ab einer Stunde 1:02:03

// Wurde in diesem Frame irgendeine Taste oder Maustaste gedrückt? (z. B.
// "beliebige Taste: zurück zum Menü" in der Simulation)
bool AnyKeyOrClick();

} // namespace ui
