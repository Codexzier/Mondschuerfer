// ---------------------------------------------------------------------------
// Ui.cpp: einfache 2D-Bausteine für Menüs und HUD (Texte, Panels, Balken,
// Knöpfe, Knopflisten, Textfelder). Alles im "immediate mode": Jede Funktion
// zeichnet sofort und wertet dabei Maus/Tastatur des aktuellen Frames aus.
// Benutzt von main.cpp, Hud.cpp und Multiplayer.cpp (nur im Spiel-Thread,
// zwischen BeginDrawing und EndDrawing). Größen skalieren mit Scale().
//
// Abhängigkeiten:
//   - raylib: Zeichnen (DrawRectangle..., DrawTextEx), eingebaute Schrift
//     (GetFontDefault), Eingaben (IsKeyPressed, IsMouseButtonPressed,
//     GetCharPressed, Zwischenablage) und Zeit (GetTime, GetFrameTime).
//   - Audio.hpp: g_audio spielt den Ton Sfx::UiMove, wenn sich die Auswahl
//     in einer Knopfliste oder der Fokus der Textfelder ändert.
//
// Zustand dieser Datei (alles im anonymen Namensraum unten): nur der
// Zeitpunkt der letzten Mausbewegung und der Textfeld-Fokus. Den Rest
// (Auswahl, Feldinhalte) hält der Aufrufer.
//
// Raylib-Begriffe:
//   - IsKeyPressed / IsMouseButtonPressed: true nur in dem EINEN Frame, in
//     dem die Taste heruntergedrückt wurde. IsKeyDown: in jedem Frame, solange
//     sie gehalten wird.
//   - raylib sammelt Tasten und getippte Zeichen in Warteschlangen, die bei
//     jedem Frame (in EndDrawing -> PollInputEvents) neu gefüllt werden.
//     GetCharPressed/GetKeyPressed holen daraus je einen Eintrag und
//     ENTFERNEN ihn.
// ---------------------------------------------------------------------------
#include "Ui.hpp"
#include "Audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {
// Zeitpunkt (GetTime, Sekunden seit Programmstart) der letzten Mausbewegung
// oder des letzten Klicks. -10 = "lange her", damit die Maus beim Start
// nicht als aktiv gilt.
double g_lastMouseMove = -10.0;
// Textfeld-Fokus: TextField::Draw setzt "thisFrame", BeginFrame schiebt den
// Wert nach "lastFrame". Abgefragt wird der Wert des VORIGEN Frames, weil
// main.cpp Tasten wie M schon am Anfang des Frames auswertet – bevor die
// Textfelder dieses Frames gezeichnet wurden.
bool g_textFocusThisFrame = false;
bool g_textFocusLastFrame = false;
}

// Wird von main.cpp (App::Frame) als Erstes in jedem Frame aufgerufen.
void BeginFrame() {
    Vector2 d = GetMouseDelta();
    if (d.x != 0.0f || d.y != 0.0f || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_lastMouseMove = GetTime();
    g_textFocusLastFrame = g_textFocusThisFrame;
    g_textFocusThisFrame = false;
}

// "Benutzt der Spieler gerade die Maus?" – ja, wenn sie sich in den letzten
// 2 s bewegt hat. Grund: Wer mit der Tastatur durch ein Menü geht, während der
// Mauszeiger zufällig still auf einem Knopf steht, soll nicht zwei
// hervorgehobene Knöpfe sehen. Ruht die Maus, zählt nur die Tastaturauswahl.
static bool MouseActive() { return GetTime() - g_lastMouseMove < 2.0; }

// Referenzgröße 1500 x 900: Gemessen wird an der Seite, die im Verhältnis
// knapper ist – so passt die Oberfläche auch in sehr breite oder sehr hohe
// Fenster. Untergrenze 0,55 (ca. 825 x 495 Pixel), damit die Pixelschrift
// lesbar bleibt; noch kleinere Fenster schneiden dann eben etwas ab.
float Scale() {
    float s = static_cast<float>(GetScreenHeight()) / 900.0f;
    float sw = static_cast<float>(GetScreenWidth()) / 1500.0f;
    if (sw < s) s = sw;
    return s < 0.55f ? 0.55f : s;
}

float Px(float v) { return v * Scale(); }

// Zeichenabstand: raylibs eingebaute Schrift ist 10 Pixel hoch und hat
// 1 Pixel Abstand zwischen den Buchstaben. Beim Vergrößern wächst der Abstand
// mit (size / 10) – genau so macht es raylibs eigenes DrawText().
static float Spacing(float size) { return size / 10.0f; }

// GetFontDefault(): die in raylib eingebaute Pixelschrift (wird mit dem
// Fenster geladen, keine Datei nötig). Sie enthält die Zeichen 32..255, also
// auch ä, ö, ü, ß; DrawTextEx/MeasureTextEx lesen die Texte als UTF-8.
float Width(const std::string& t, float size) {
    return MeasureTextEx(GetFontDefault(), t.c_str(), size, Spacing(size)).x;
}

void Text(const std::string& t, float x, float y, float size, Color c, bool shadow) {
    Font f = GetFontDefault();
    // Schatten: dieselbe Schrift in Schwarz, um size/12 nach rechts unten
    // versetzt (bei 24 px also 2 px). Seine Deckkraft folgt der des Textes
    // (60 % davon), damit ausblendende Texte auch den Schatten ausblenden.
    if (shadow) {
        float o = size / 12.0f;
        DrawTextEx(f, t.c_str(), {roundf(x + o), roundf(y + o)}, size, Spacing(size), Color{0, 0, 0, static_cast<unsigned char>(c.a * 0.6f)});
    }
    // Positionen auf ganze Pixel runden: Eine Pixelschrift auf halben
    // Pixeln wird ungleichmäßig/verschwommen gezeichnet.
    DrawTextEx(f, t.c_str(), {roundf(x), roundf(y)}, size, Spacing(size), c);
}

void TextCentered(const std::string& t, float cx, float y, float size, Color c, bool shadow) {
    Text(t, cx - Width(t, size) * 0.5f, y, size, c, shadow);
}

void TextRight(const std::string& t, float rx, float y, float size, Color c, bool shadow) {
    Text(t, rx - Width(t, size), y, size, c, shadow);
}

// Panel: Fläche, 2 px Rahmen (mindestens 1 px bei kleinem Fenster) und zwei
// L-förmige Akzent-Ecken (12 px lang, 4 px dick) links oben und rechts unten.
// Die Ecken bestehen je aus einem waagerechten und einem senkrechten Rechteck.
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

// Balken: Hintergrund, gefüllter Anteil frac, dann 9 dunkle Trennstriche
// (10 Segmente, wie eine Ladeanzeige) und ein feiner heller Rahmen.
// frac wird auf 0..1 begrenzt, damit z. B. ein Kontostand über dem Ziel
// oder eine negative Zahl den Balken nicht über den Rand hinaus malt.
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

// Knopf mit drei Zuständen: gesperrt (grau), normal, hervorgehoben (per
// Tastatur ausgewählt ODER Maus darüber und Maus aktiv; dann bräunliche
// Fläche, oranger Rahmen und ein oranger Streifen links).
// Ein Klick zählt im Frame des Herunterdrückens (IsMouseButtonPressed),
// nicht beim Loslassen – einfacher, und für ein Spiel reaktionsschnell.
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
    // Text senkrecht mittig: Schrifthöhe = size, also (Knopfhöhe - size) / 2 Rand oben
    TextCentered(label, r.x + r.width * 0.5f, r.y + (r.height - size) * 0.5f, size, tc);
    return enabled && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// Ablauf pro Frame:
//   1. selected gültig machen (ungültig oder gesperrt -> erster wählbarer Eintrag)
//   2. Pfeiltasten/W/S: zum nächsten wählbaren Eintrag, mit Umlauf am Ende
//   3. Knöpfe zeichnen; Klick wählt sofort, Mausbewegung über einem Knopf
//      verschiebt die Tastaturauswahl dorthin (so bleiben Maus und Tastatur
//      einig)
//   4. Enter/Leertaste bestätigen die aktuelle Auswahl
//   5. Hat sich die Auswahl geändert: kurzer Bedienton
int ButtonList(const std::vector<std::string>& items, const std::vector<bool>& enabled, int& selected,
               float cx, float top, float width, float height, float gap, float textSize) {
    const int n = static_cast<int>(items.size());
    auto isEnabled = [&](int i) { return i < static_cast<int>(enabled.size()) ? enabled[i] : true; };
    if (selected < 0 || selected >= n || !isEnabled(selected)) {
        for (int i = 0; i < n; ++i) if (isEnabled(i)) { selected = i; break; }
    }
    const int before = selected;
    // Nach unten: k = 1, 2, ... Einträge weiter, % n springt vom letzten zum
    // ersten. Gesperrte Einträge werden übersprungen.
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) {
        for (int k = 1; k <= n; ++k) {
            int i = (selected + k) % n;
            if (isEnabled(i)) { selected = i; break; }
        }
    }
    // Nach oben: + n * 2 hält den Wert vor dem % positiv (in C++ ist z. B.
    // -1 % 5 = -1, nicht 4).
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
        // Nur bei BEWEGTER Maus übernimmt der Knopf unter dem Zeiger die
        // Auswahl – ein ruhender Zeiger soll die Pfeiltasten nicht blockieren.
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
// Kurz zu UTF-8 (so sind alle Texte im Spiel gespeichert): Ein Zeichen
// belegt 1 bis 4 Bytes. ASCII (0..127) ist 1 Byte. Bei längeren Zeichen hat
// das erste Byte die Form 11xxxxxx und jedes Folgebyte die Form 10xxxxxx,
// z. B. 'ä' = 0xC3 0xA4. "& 0xC0" schneidet die obersten zwei Bits heraus;
// ergibt das 0x80 (= 10xxxxxx), ist es ein Folgebyte.
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

// Letztes Zeichen (mitsamt seinen UTF-8-Folgebytes) entfernen: erst alle
// Folgebytes am Ende, dann das Anfangsbyte. So bleibt nie ein halbes Zeichen
// stehen.
void PopUtf8(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}

// Ein Zeichen anhängen, wenn es erlaubt ist und noch Platz ist. true = angehängt.
// Wird für getippte UND eingefügte Zeichen benutzt, damit Filter (accept,
// z. B. "nur Zeichen, die in einer Serveradresse vorkommen") und Höchstlänge
// immer gelten.
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

// Ablauf: 1. Fokus per Mausklick, 2. Eingaben (nur mit Fokus): Zeichen,
// Rücktaste, Einfügen, Enter, 3. Zeichnen (Feld, Text oder Platzhalter,
// blinkender Cursor). Gelesen wird vor dem Zeichnen, damit ein gerade
// getipptes Zeichen schon in diesem Frame sichtbar ist.
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
        // repeatT ist der Countdown bis zum nächsten Löschen; GetFrameTime()
        // = Sekunden seit dem letzten Frame.
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
        // KEY_*_SUPER ist die Befehlstaste (macOS) bzw. Windows-Taste.
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) ||
                          IsKeyDown(KEY_RIGHT_SUPER);
        if (ctrl && IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();   // kann nullptr sein (leere Zwischenablage)
            if (clip) {
                // Zeichen für Zeichen dekodieren; beim ersten Zeilenumbruch aufhören
                // (aus einer Datei kopierte Adresse mit '\n' am Ende).
                // GetCodepointNext (raylib) liest ein UTF-8-Zeichen ab p und
                // meldet in len, wie viele Bytes es belegt hat.
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

    // pad = Innenabstand links; ty = Textoberkante, senkrecht mittig im Feld.
    // Texte im Feld ohne Schatten (shadow = false) – auf dem dunklen Feld unnötig.
    const float pad = 12 * s;
    const float ty = r.y + (r.height - size) * 0.5f;
    if (text.empty()) {
        if (!placeholder.empty()) Text(placeholder, r.x + pad, ty, size, Color{110, 117, 134, 255}, false);
    }

    // Anzeigetext: beim Passwort ein * pro Zeichen. Ist er breiter als das
    // Feld, wird vorne abgeschnitten, damit das Ende (wo getippt wird) sichtbar
    // bleibt. avail lässt rechts noch Platz für den Cursor (0,4 * size).
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
    // fmod(..., 1.0) = Nachkommateil der Sekunden seit der letzten Eingabe.
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

// Fokuswechsel mit Tab. Vorwärts: +1; rückwärts: + (n - 1), das ist modulo n
// dasselbe wie -1, bleibt aber positiv. Benutzt von Multiplayer.cpp für die
// Anmeldefelder (Name, Passwort).
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

// Wert des vorigen Frames (Begründung bei g_textFocusLastFrame oben)
bool TextInputActive() { return g_textFocusLastFrame; }

// Zahl mit deutschen Tausenderpunkten: 1234567 -> "1.234.567".
// Die Ziffern werden von hinten nach vorn kopiert; nach jeder dritten kommt
// ein Punkt davor (außer ganz vorn). Das Vorzeichen wird getrennt behandelt.
// (std::locale wäre die Alternative, hängt aber von der Systemeinstellung ab.)
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

// Spielzeit als m:ss, ab einer Stunde h:mm:ss (angefangene Sekunden werden
// abgeschnitten). %02d = zweistellig mit führender Null.
std::string Time(float seconds) {
    int s = static_cast<int>(seconds);
    char buf[32];
    if (s >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

// Achtung: GetKeyPressed nimmt die Taste aus raylibs Warteschlange. Wer in
// demselben Frame danach GetKeyPressed aufruft, bekommt sie nicht mehr
// (IsKeyPressed ist davon nicht betroffen).
bool AnyKeyOrClick() {
    return GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
}

} // namespace ui
