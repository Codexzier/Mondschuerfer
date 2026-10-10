#include "Ui.hpp"
#include "Audio.hpp"
#include <cmath>
#include <cstdio>

namespace ui {

namespace {
double g_lastMouseMove = -10.0;
}

void BeginFrame() {
    Vector2 d = GetMouseDelta();
    if (d.x != 0.0f || d.y != 0.0f || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_lastMouseMove = GetTime();
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
