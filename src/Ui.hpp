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

std::string Num(long long v);          // 100.000
std::string Time(float seconds);       // 12:34

bool AnyKeyOrClick();

} // namespace ui
