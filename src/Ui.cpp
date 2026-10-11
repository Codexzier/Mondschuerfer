// ---------------------------------------------------------------------------
// Ui.cpp: einfache 2D-Bausteine für Menüs und HUD (Texte, Panels, Balken,
// Knöpfe, Knopflisten, Textfelder). Alles im "immediate mode": Jede Funktion
// zeichnet sofort und wertet dabei Maus/Tastatur des aktuellen Frames aus.
// Benutzt von main.cpp, Hud.cpp und Multiplayer.cpp (nur im Spiel-Thread,
// zwischen BeginDrawing und EndDrawing). Größen skalieren mit Scale().
// ---------------------------------------------------------------------------
#include "Ui.hpp"
#include "Audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {
double g_lastMouseMove = -10.0;
// Textfeld-Fokus: TextField::Draw setzt "thisFrame", BeginFrame schiebt den
// Wert nach "lastFrame". Abgefragt wird der Wert des VORIGEN Frames, weil
// main.cpp Tasten wie M schon am Anfang des Frames auswertet – bevor die
// Textfelder dieses Frames gezeichnet wurden.
bool g_textFocusThisFrame = false;
bool g_textFocusLastFrame = false;
}

void BeginFrame() {
    Vector2 d = GetMouseDelta();
    if (d.x != 0.0f || d.y != 0.0f || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_lastMouseMove = GetTime();
    g_textFocusLastFrame = g_textFocusThisFrame;
    g_textFocusThisFrame = false;
}

static bool MouseActive() { return GetTime() - g_lastMouseMove < 2.0; }

float Scale() {
    float s = static_cast<float>(GetScreenHeight()) / 900.0f;
    float sw = static_cast<float>(GetScreenWidth()) / 1500.0f;
    if (sw < s) s = sw;
    return s < 0.55f ? 0.55f : s;
}

float Px(float v) { return v * Scale(); }

static float Spacing(float size) { return size / 10.0f; }

float Width(const std::string& t, float size) {
    return MeasureTextEx(GetFontDefault(), t.c_str(), size, Spacing(size)).x;
}

void Text(const std::string& t, float x, float y, float size, Color c, bool shadow) {
    Font f = GetFontDefault();
    if (shadow) {
        float o = size / 12.0f;
        DrawTextEx(f, t.c_str(), {roundf(x + o), roundf(y + o)}, size, Spacing(size), Color{0, 0, 0, static_cast<unsigned char>(c.a * 0.6f)});
    }
    DrawTextEx(f, t.c_str(), {roundf(x), roundf(y)}, size, Spacing(size), c);
}

void TextCentered(const std::string& t, float cx, float y, float size, Color c, bool shadow) {
    Text(t, cx - Width(t, size) * 0.5f, y, size, c, shadow);
}

void TextRight(const std::string& t, float rx, float y, float size, Color c, bool shadow) {
    Text(t, rx - Width(t, size), y, size, c, shadow);
}

void Panel(Rectangle r, Color fill, Color border) {
    DrawRectangleRec(r, fill);
    float t = Px(2.0f);
    DrawRectangleLinesEx(r, t < 1 ? 1 : t, border);
    // Akzent-Ecken
    float c = Px(12.0f);
    float w = Px(4.0f);
    DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(c), static_cast<int>(w), ACCENT);
    DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(w), static_cast<int>(c), ACCENT);
    DrawRectangle(static_cast<int>(r.x + r.width - c), static_cast<int>(r.y + r.height - w), static_cast<int>(c), static_cast<int>(w), ACCENT);
    DrawRectangle(static_cast<int>(r.x + r.width - w), static_cast<int>(r.y + r.height - c), static_cast<int>(w), static_cast<int>(c), ACCENT);
}

void Bar(Rectangle r, float frac, Color fill, Color back) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    DrawRectangleRec(r, back);
    DrawRectangleRec({r.x, r.y, r.width * frac, r.height}, fill);
    // Segmentierung
    int seg = 10;
    for (int i = 1; i < seg; ++i) {
        float x = r.x + r.width * i / seg;
        DrawLineEx({x, r.y}, {x, r.y + r.height}, 1.0f, Color{0, 0, 0, 90});
    }
    DrawRectangleLinesEx(r, 1.0f, Color{255, 255, 255, 40});
}

bool Button(Rectangle r, const std::string& label, float size, bool focused, bool enabled, bool* hovered) {
    Vector2 m = GetMousePosition();
    bool hover = CheckCollisionPointRec(m, r);
    if (hovered) *hovered = hover;
    Color fill = PANEL_LIGHT;
    Color border = Color{90, 100, 120, 255};
    Color tc = TEXT;
    if (!enabled) {
        fill = Color{30, 34, 42, 200};
        tc = Color{100, 106, 120, 255};
    } else if (focused || (hover && MouseActive())) {
        fill = Color{70, 52, 30, 240};
        border = ACCENT;
        tc = Color{255, 220, 170, 255};
    }
    DrawRectangleRec(r, fill);
    DrawRectangleLinesEx(r, Px(2.0f) < 1 ? 1 : Px(2.0f), border);
    if (enabled && (focused || (hover && MouseActive()))) {
        DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(Px(6)), static_cast<int>(r.height), ACCENT);
    }
    TextCentered(label, r.x + r.width * 0.5f, r.y + (r.height - size) * 0.5f, size, tc);
    return enabled && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

int ButtonList(const std::vector<std::string>& items, const std::vector<bool>& enabled, int& selected,
               float cx, float top, float width, float height, float gap, float textSize) {
    const int n = static_cast<int>(items.size());
    auto isEnabled = [&](int i) { return i < static_cast<int>(enabled.size()) ? enabled[i] : true; };
    if (selected < 0 || selected >= n || !isEnabled(selected)) {
        for (int i = 0; i < n; ++i) if (isEnabled(i)) { selected = i; break; }
    }
    const int before = selected;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) {
        for (int k = 1; k <= n; ++k) {
            int i = (selected + k) % n;
            if (isEnabled(i)) { selected = i; break; }
        }
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) {
        for (int k = 1; k <= n; ++k) {
            int i = (selected - k + n * 2) % n;
            if (isEnabled(i)) { selected = i; break; }
        }
    }
    int chosen = -1;
    for (int i = 0; i < n; ++i) {
        Rectangle r{cx - width * 0.5f, top + i * (height + gap), width, height};
        bool hover = false;
        if (Button(r, items[i], textSize, i == selected, isEnabled(i), &hover)) chosen = i;
        if (hover && isEnabled(i) && (GetMouseDelta().x != 0 || GetMouseDelta().y != 0)) selected = i;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE)) {
        if (isEnabled(selected)) chosen = selected;
    }
    if (selected != before) g_audio.Play(Sfx::UiMove);
    return chosen;
}

// ===========================================================================
// Textfeld
// ===========================================================================
int Utf8Length(const std::string& s) {
    // In UTF-8 beginnt jedes Zeichen mit einem Byte, das NICHT die Form
    // 10xxxxxx hat; Folgebytes haben genau diese Form. Also zählen wir nur
    // die Anfangsbytes.
    int n = 0;
    for (char ch : s)
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;
    return n;
}

namespace {

// Letztes Zeichen (mitsamt seinen UTF-8-Folgebytes) entfernen
void PopUtf8(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}

// Ein Zeichen anhängen, wenn es erlaubt ist und noch Platz ist. true = angehängt.
bool AppendCodepoint(TextField& f, int cp) {
    if (cp < 32 || cp == 127) return false;                  // Steuerzeichen (Tab, Enter ...) nie
    if (f.accept && !f.accept(cp)) return false;
    if (Utf8Length(f.text) >= f.maxChars) return false;
    int len = 0;
    const char* bytes = CodepointToUTF8(cp, &len);           // raylib: Codepoint -> UTF-8 (statischer Puffer)
    f.text.append(bytes, static_cast<size_t>(len));
    return true;
}

} // namespace

bool TextField::Draw(Rectangle r, float size, bool enabled) {
    // --- Fokus per Maus ------------------------------------------------------
    if (enabled && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) focused = CheckCollisionPointRec(GetMousePosition(), r);

    // Gesperrt (enabled = false): Fokus bleibt gemerkt, aber es wird nichts
    // eingegeben – nach dem Entsperren kann man gleich weitertippen.
    const bool active = focused && enabled;
    bool submit = false;
    if (active) {
        g_textFocusThisFrame = true;

        // --- Getippte Zeichen ------------------------------------------------
        // GetCharPressed liefert jedes Zeichen, das das Betriebssystem seit dem
        // letzten Frame gemeldet hat (mit Tastaturlayout, Umschalt, AltGr ...),
        // eins pro Aufruf, 0 = Warteschlange leer.
        for (int cp = GetCharPressed(); cp > 0; cp = GetCharPressed())
            if (AppendCodepoint(*this, cp)) lastEdit = GetTime();

        // --- Rücktaste mit Wiederholung ---------------------------------------
        // Erster Druck löscht sofort; wer die Taste hält, löscht nach 0,45 s
        // Wartezeit alle 0,04 s ein weiteres Zeichen (wie in Textprogrammen).
        if (IsKeyPressed(KEY_BACKSPACE)) {
            PopUtf8(text);
            repeatT = 0.45f;
            lastEdit = GetTime();
        } else if (IsKeyDown(KEY_BACKSPACE)) {
            repeatT -= GetFrameTime();
            if (repeatT <= 0.0f) {
                PopUtf8(text);
                repeatT = 0.04f;
                lastEdit = GetTime();
            }
        }

        // --- Einfügen: Strg+V (macOS: Cmd+V) ------------------------------------
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) ||
                          IsKeyDown(KEY_RIGHT_SUPER);
        if (ctrl && IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();   // kann nullptr sein (leere Zwischenablage)
            if (clip) {
                // Zeichen für Zeichen dekodieren; beim ersten Zeilenumbruch aufhören
                // (aus einer Datei kopierte Adresse mit '\n' am Ende).
                for (const char* p = clip; *p;) {
                    int len = 0;
                    const int cp = GetCodepointNext(p, &len);
                    if (len <= 0) break;
                    p += len;
                    if (cp == '\n' || cp == '\r') break;
                    AppendCodepoint(*this, cp);
                }
                lastEdit = GetTime();
            }
        }

        submit = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    }

    // --- Zeichnen ---------------------------------------------------------------
    const float s = Scale();
    Color fill = enabled ? Color{15, 18, 24, 235} : Color{24, 27, 34, 200};
    Color border = active ? ACCENT : Color{90, 100, 120, 255};
    DrawRectangleRec(r, fill);
    DrawRectangleLinesEx(r, Px(2.0f) < 1 ? 1 : Px(2.0f), border);

    const float pad = 12 * s;
    const float ty = r.y + (r.height - size) * 0.5f;
    if (text.empty()) {
        if (!placeholder.empty()) Text(placeholder, r.x + pad, ty, size, Color{110, 117, 134, 255}, false);
    }

    // Anzeigetext: beim Passwort ein * pro Zeichen. Ist er breiter als das
    // Feld, wird vorne abgeschnitten, damit das Ende (wo getippt wird) sichtbar
    // bleibt.
    std::string shown = password ? std::string(static_cast<size_t>(Utf8Length(text)), '*') : text;
    const float avail = r.width - 2 * pad - size * 0.4f;
    while (!shown.empty() && Width(shown, size) > avail) {
        // erstes Zeichen samt UTF-8-Folgebytes entfernen
        size_t cut = 1;
        while (cut < shown.size() && (static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80) ++cut;
        shown.erase(0, cut);
    }
    if (!shown.empty()) Text(shown, r.x + pad, ty, size, enabled ? TEXT : MUTED, false);

    // Blinkender Cursor (Strich hinter dem Text): 0,5 s an, 0,5 s aus. Nach
    // einer Eingabe beginnt der Takt neu, damit der Cursor beim Tippen sichtbar ist.
    if (active) {
        const double phase = fmod(GetTime() - lastEdit, 1.0);
        if (phase < 0.5) {
            const float cx = r.x + pad + (shown.empty() ? 0.0f : Width(shown, size) + size * 0.08f);
            const float cw = std::max(1.0f, size / 12.0f);
            DrawRectangleRec({cx, ty - size * 0.05f, cw, size * 1.05f}, ACCENT);
        }
    }
    return submit;
}

void CycleFocus(const std::vector<TextField*>& fields) {
    if (fields.empty() || !IsKeyPressed(KEY_TAB)) return;
    const int n = static_cast<int>(fields.size());
    int cur = -1;
    for (int i = 0; i < n; ++i) if (fields[i]->focused) cur = i;
    const bool back = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int next = cur < 0 ? 0 : (cur + (back ? n - 1 : 1)) % n;
    for (int i = 0; i < n; ++i) fields[i]->focused = (i == next);
    fields[next]->lastEdit = GetTime();   // Cursor sofort sichtbar
    g_audio.Play(Sfx::UiMove);
}

bool TextInputActive() { return g_textFocusLastFrame; }

std::string Num(long long v) {
    bool neg = v < 0;
    if (neg) v = -v;
    std::string s = std::to_string(v);
    std::string out;
    int c = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i) {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i > 0) out.insert(out.begin(), '.');
    }
    return neg ? "-" + out : out;
}

std::string Time(float seconds) {
    int s = static_cast<int>(seconds);
    char buf[32];
    if (s >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

bool AnyKeyOrClick() {
    return GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
}

} // namespace ui
