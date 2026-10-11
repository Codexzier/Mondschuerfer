#pragma once
#include "raylib.h"
#include <string>
#include <vector>

// Einfache UI-Bausteine (Texte, Panels, Balken, Knöpfe) im Kenney-Farbstil.
namespace ui {

constexpr Color ACCENT{255, 160, 51, 255};
constexpr Color ACCENT_DIM{200, 120, 40, 255};
constexpr Color PANEL{26, 31, 41, 238};
constexpr Color PANEL_LIGHT{44, 51, 64, 235};
constexpr Color TEXT{236, 239, 245, 255};
constexpr Color MUTED{150, 158, 176, 255};
constexpr Color GOOD{110, 240, 170, 255};
constexpr Color BAD{255, 105, 90, 255};
constexpr Color SHIELD{95, 175, 255, 255};
constexpr Color SCAN{70, 230, 170, 255};

void BeginFrame();                   // einmal pro Frame aufrufen
float Scale();                       // 1.0 bei 900 Pixel Fensterhöhe
float Px(float v);                   // v * Scale()

float Width(const std::string& t, float size);
void Text(const std::string& t, float x, float y, float size, Color c, bool shadow = true);
void TextCentered(const std::string& t, float cx, float y, float size, Color c, bool shadow = true);
void TextRight(const std::string& t, float rx, float y, float size, Color c, bool shadow = true);

void Panel(Rectangle r, Color fill = PANEL, Color border = ACCENT_DIM);
void Bar(Rectangle r, float frac, Color fill, Color back = Color{15, 18, 24, 220});

// Knopf: liefert true beim Anklicken. "focused" = per Tastatur ausgewählt.
bool Button(Rectangle r, const std::string& label, float size, bool focused, bool enabled = true,
            bool* hovered = nullptr);

// Senkrechte Knopfliste mit Tastatur- und Maussteuerung. Liefert gewählten Index oder -1.
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

std::string Num(long long v);          // 100.000
std::string Time(float seconds);       // 12:34

bool AnyKeyOrClick();

} // namespace ui
