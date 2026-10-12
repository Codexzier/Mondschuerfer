// ---------------------------------------------------------------------------
// Ui.cpp: einfache 2D-Bausteine für Menüs und HUD (Texte, Panels, Balken,
// Knöpfe, Knopflisten, Schieberegler, Umschalter, Textfelder). Alles im
// "immediate mode": Jede Funktion zeichnet sofort und wertet dabei
// Maus/Tastatur des aktuellen Frames aus. Benutzt von main.cpp, Hud.cpp,
// Multiplayer.cpp und Options.cpp (nur im Spiel-Thread, zwischen
// BeginDrawing und EndDrawing). Größen skalieren mit Scale().
//
// Abhängigkeiten:
//   - raylib: Zeichnen (DrawRectangle..., DrawTextEx), eingebaute Schrift
//     (GetFontDefault), Eingaben (IsKeyPressed, IsMouseButtonPressed,
//     IsGamepadButtonPressed, GetGamepadAxisMovement, SetMousePosition,
//     GetCharPressed, Zwischenablage) und Zeit (GetTime, GetFrameTime).
//   - Audio.hpp: g_audio spielt den Ton Sfx::UiMove, wenn sich die Auswahl
//     in einer Knopfliste oder der Fokus der Textfelder ändert.
//   - ControlsMath.hpp: Totzone des Sticks, PlayStation-Namen erkennen.
//
// Zustand dieser Datei (alles im anonymen Namensraum unten): Zeitpunkt der
// letzten Zeigerbewegung, Textfeld-Fokus, Flug-/Menümodus, die Rechtecke
// der UI-Flächen, der benutzte Controller und der Controller-Zeiger. Den
// Rest (Auswahl, Feldinhalte) hält der Aufrufer.
//
// Viele Werte gibt es doppelt, als "dieses Frame" und "voriges Frame":
// Wer sie setzt (Textfelder, Panels, die Flugsteuerung), tut das beim
// Zeichnen bzw. Rechnen IN einem Frame; wer sie abfragt, braucht sie oft
// schon VORHER (am Anfang des nächsten Frames). BeginFrame schiebt sie
// deshalb jedes Frame eins weiter.
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
#include "ControlsMath.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui {

namespace {
// Zeitpunkt (GetTime, Sekunden seit Programmstart) der letzten
// Zeigerbewegung (Maus oder Controller-Stick) oder des letzten Klicks.
// -10 = "lange her", damit der Zeiger beim Start nicht als aktiv gilt.
double g_lastMouseMove = -10.0;
// Textfeld-Fokus: TextField::Draw setzt "thisFrame", BeginFrame schiebt den
// Wert nach "lastFrame". Abgefragt wird der Wert des VORIGEN Frames, weil
// main.cpp Tasten wie M schon am Anfang des Frames auswertet – bevor die
// Textfelder dieses Frames gezeichnet wurden.
bool g_textFocusThisFrame = false;
bool g_textFocusLastFrame = false;

unsigned g_frame = 0;                 // FrameIndex()
// Flug: SetFlightMode setzt "thisFrame"; MenuMode fragt "lastFrame" ab.
bool g_flightThisFrame = false;
bool g_flightLastFrame = false;
// Rechtecke der UI-Flächen (Panel, BlockPointer), getrennt nach Frames.
std::vector<Rectangle> g_blockThisFrame;
std::vector<Rectangle> g_blockLastFrame;

int g_pad = -1;                       // benutzter Controller (Gamepad())
bool g_usingPad = false;              // zuletzt benutztes Gerät war ein Controller
bool g_padIsPlayStation = false;
bool g_testPad = false;               // SetTestGamepad: Controller nur vorgetäuscht
std::string g_testPadName;

// Controller-Zeiger (siehe Ui.hpp, "Zeiger-Abfragen")
bool g_padPointer = false;            // der Controller bedient den Zeiger
Vector2 g_padPos{0.0f, 0.0f};         // seine Position in Pixeln (mit Nachkommastellen)
Vector2 g_padDelta{0.0f, 0.0f};       // seine Bewegung in diesem Frame

// Höchstgeschwindigkeit des Controller-Zeigers bei voll ausgelenktem Stick,
// in Pixeln pro Sekunde bei Referenzgröße (wird mit Scale() multipliziert):
// Die 1500 Pixel breite Referenzfläche quert er in gut einer Sekunde.
constexpr float kPadPointerSpeed = 1300.0f;

// Wurde an diesem Controller in diesem Frame irgendein Knopf gedrückt?
// raylib zählt die Knöpfe von GAMEPAD_BUTTON_LEFT_FACE_UP (1) bis
// GAMEPAD_BUTTON_RIGHT_THUMB (17); 0 ist "unbekannt".
bool AnyPadButtonPressed(int pad) {
    if (pad < 0) return false;
    for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB; ++b)
        if (IsGamepadButtonPressed(pad, b)) return true;
    return false;
}

// Wurde eine Taste der Tastatur gedrückt? Bewusst NICHT mit GetKeyPressed:
// Das nimmt die Taste aus raylibs Warteschlange (siehe AnyKeyOrClick unten),
// und ein Textfeld bekäme sie dann nicht mehr. IsKeyPressed lässt alles,
// wie es ist; die rund 320 Abfragen kosten praktisch nichts.
// KEY_SPACE (32) .. KEY_KB_MENU (348) sind alle Tasten, die raylib kennt.
bool AnyKeyboardKeyPressed() {
    for (int k = KEY_SPACE; k <= KEY_KB_MENU; ++k)
        if (IsKeyPressed(k)) return true;
    return false;
}

// Wurde der Controller in diesem Frame benutzt? Ein Knopf, ein Stick weiter
// als halb ausgelenkt oder ein Trigger mehr als gut halb gedrückt (> 0,3 auf
// raylibs Skala -1..1). Die hohen Schwellen sorgen dafür, dass ein leicht
// schief stehender Stick nicht ständig auf "Controller" umschaltet.
bool PadUsedThisFrame(int pad) {
    if (pad < 0) return false;
    if (AnyPadButtonPressed(pad)) return true;
    const Vector2 l{GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y)};
    const Vector2 r{GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y)};
    if (Vector2Length(l) > 0.5f || Vector2Length(r) > 0.5f) return true;
    return GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.3f ||
           GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_TRIGGER) > 0.3f;
}
}

// Wird von main.cpp (App::Frame) als Erstes in jedem Frame aufgerufen.
// Ablauf:
//   1. Werte des vorigen Frames weiterschieben (Flug, Fokus, UI-Flächen).
//   2. Controller suchen: der erste angeschlossene von 0 bis 3.
//   3. Zuletzt benutztes Gerät bestimmen (für die Hilfetexte).
//   4. Controller-Zeiger: im Flug abgeben, im Menümodus mit dem Stick bewegen.
void BeginFrame() {
    ++g_frame;
    g_textFocusLastFrame = g_textFocusThisFrame;
    g_textFocusThisFrame = false;
    g_flightLastFrame = g_flightThisFrame;
    g_flightThisFrame = false;
    g_blockLastFrame.swap(g_blockThisFrame);   // tauscht nur die Inhalte, ohne zu kopieren
    g_blockThisFrame.clear();

    // --- 2. Controller ------------------------------------------------------
    g_pad = -1;
    for (int i = 0; i < 4; ++i) {
        if (IsGamepadAvailable(i)) {
            g_pad = i;
            break;
        }
    }
    if (g_testPad) g_padIsPlayStation = controls::LooksLikePlayStation(g_testPadName.c_str());
    else if (g_pad >= 0) g_padIsPlayStation = controls::LooksLikePlayStation(GetGamepadName(g_pad));

    // --- 3. Zuletzt benutztes Gerät ---------------------------------------------
    // Echte Mausbewegung: Die Maus hat sich bewegt UND steht nicht dort, wo
    // der Controller-Zeiger sie zuletzt hingesetzt hat. (Je nach System
    // meldet raylib nach SetMousePosition im nächsten Frame eine
    // Bewegung – das ist dann keine echte.)
    const Vector2 md = GetMouseDelta();
    const bool warpEcho = g_padPointer && Vector2Distance(GetMousePosition(), g_padPos) < 2.0f;
    const bool mouseMoved = (md.x != 0.0f || md.y != 0.0f) && !warpEcho;
    const bool mouseClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
                              IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
    if (mouseMoved || mouseClicked) g_lastMouseMove = GetTime();
    if (mouseMoved || mouseClicked || GetMouseWheelMove() != 0.0f || AnyKeyboardKeyPressed()) g_usingPad = false;
    if (PadUsedThisFrame(g_pad)) g_usingPad = true;
    if (g_testPad) g_usingPad = true;

    // --- 4. Controller-Zeiger ------------------------------------------------------
    g_padDelta = {0.0f, 0.0f};
    if (g_flightLastFrame || mouseMoved || g_pad < 0) {
        g_padPointer = false;   // geparkt (siehe Ui.hpp)
    } else if (IsWindowFocused()) {
        // Stick mit kreisförmiger Totzone 0,2 (ControlsMath.hpp). Weiche
        // Kennlinie: Tempo = Höchsttempo * Ausschlag². Halb ausgelenkt geht
        // der Zeiger also nur ein Viertel so schnell – damit lässt er sich
        // auch auf kleine Knöpfe genau setzen, voll ausgelenkt ist er flott.
        const Vector2 st = controls::StickDeadzone(
            {GetGamepadAxisMovement(g_pad, GAMEPAD_AXIS_LEFT_X), GetGamepadAxisMovement(g_pad, GAMEPAD_AXIS_LEFT_Y)}, 0.2f);
        const float m = Vector2Length(st);
        if (m > 0.0f) {
            const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
            if (!g_padPointer) {
                // Übernehmen: dort beginnen, wo die Maus steht (außerhalb des
                // Fensters: in der Mitte).
                const Vector2 mp = GetMousePosition();
                const bool inside = mp.x >= 0 && mp.y >= 0 && mp.x < W && mp.y < H;
                g_padPos = inside ? mp : Vector2{W * 0.5f, H * 0.5f};
                g_padPointer = true;
            }
            // GetFrameTime = Dauer des letzten Frames; begrenzt, damit ein
            // Ruckler den Zeiger nicht quer über den Bildschirm springen lässt.
            const float dt = std::min(GetFrameTime(), 0.05f);
            const float speed = kPadPointerSpeed * Scale() * m * m;
            const Vector2 before = g_padPos;
            g_padPos.x = Clamp(g_padPos.x + st.x / m * speed * dt, 0.0f, W - 1.0f);
            g_padPos.y = Clamp(g_padPos.y + st.y / m * speed * dt, 0.0f, H - 1.0f);
            g_padDelta = Vector2Subtract(g_padPos, before);
            // Den Mauszeiger des Systems mitnehmen (wo das System es erlaubt)
            SetMousePosition(static_cast<int>(roundf(g_padPos.x)), static_cast<int>(roundf(g_padPos.y)));
            g_lastMouseMove = GetTime();
        }
    }
}

unsigned FrameIndex() { return g_frame; }

void SetFlightMode() { g_flightThisFrame = true; }
bool MenuMode() { return !g_flightLastFrame; }

int Gamepad() { return g_pad; }
bool UsingGamepad() { return g_usingPad; }
bool GamepadIsPlayStation() { return g_padIsPlayStation; }

void SetTestGamepad(const char* name) {
    g_testPad = true;
    g_testPadName = name ? name : "";
    g_usingPad = true;
    g_padIsPlayStation = controls::LooksLikePlayStation(g_testPadName.c_str());
}

bool PadPressed(int button) { return MenuMode() && g_pad >= 0 && IsGamepadButtonPressed(g_pad, button); }

// Zeiger-Abfragen (Begründung in Ui.hpp). A zählt nur, wenn der Controller
// gerade den Zeiger bedient und nicht geflogen wird.
Vector2 PointerPos() { return g_padPointer ? g_padPos : GetMousePosition(); }
Vector2 PointerDelta() { return g_padPointer ? g_padDelta : GetMouseDelta(); }
bool PointerMoved() {
    const Vector2 d = PointerDelta();
    return d.x != 0.0f || d.y != 0.0f;
}
bool PadPointerActive() { return g_padPointer; }

static bool PadClickActive() { return g_padPointer && MenuMode() && g_pad >= 0; }
bool PointerPressed() {
    return IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || (PadClickActive() && IsGamepadButtonPressed(g_pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
}
bool PointerDown() {
    return IsMouseButtonDown(MOUSE_BUTTON_LEFT) || (PadClickActive() && IsGamepadButtonDown(g_pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
}
bool PointerReleased() {
    return IsMouseButtonReleased(MOUSE_BUTTON_LEFT) || (PadClickActive() && IsGamepadButtonReleased(g_pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
}
// B = RIGHT_FACE_RIGHT (rechter Knopf), Menü-Taste = MIDDLE_RIGHT (rechte
// der mittleren Tasten; Xbox "Menü" mit drei Strichen, PlayStation "Options").
bool BackPressed() {
    return IsKeyPressed(KEY_ESCAPE) || PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) || PadPressed(GAMEPAD_BUTTON_MIDDLE_RIGHT);
}

// Controller-Zeiger: oranger Ring mit dunklem Rand und Punkt in der Mitte.
// DrawRing(Mitte, Innenradius, Außenradius, Startwinkel, Endwinkel,
// Segmente, Farbe) zeichnet einen flachen Kreisring (0..360° = ganz).
void DrawPointer() {
    if (!g_padPointer || !MenuMode()) return;
    const float s = Scale();
    DrawRing(g_padPos, 11.0f * s, 16.0f * s, 0.0f, 360.0f, 32, Color{10, 12, 20, 200});
    DrawRing(g_padPos, 12.0f * s, 15.0f * s, 0.0f, 360.0f, 32, ACCENT);
    DrawCircleV(g_padPos, 3.0f * s, ACCENT);
}

bool PointerOverPanel(Vector2 p) {
    for (const Rectangle& r : g_blockLastFrame)
        if (CheckCollisionPointRec(p, r)) return true;
    return false;
}

void BlockPointer(Rectangle r) { g_blockThisFrame.push_back(r); }

// "Benutzt der Spieler gerade die Maus (oder den Controller-Zeiger)?" – ja,
// wenn der Zeiger sich in den letzten 2 s bewegt hat. Grund: Wer mit der
// Tastatur durch ein Menü geht, während der Mauszeiger zufällig still auf
// einem Knopf steht, soll nicht zwei hervorgehobene Knöpfe sehen. Ruht der
// Zeiger, zählt nur die Tastaturauswahl.
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
    g_blockThisFrame.push_back(r);   // für PointerOverPanel (Flugklicks blockieren)
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
    Vector2 m = PointerPos();
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
    return enabled && hover && PointerPressed();
}

// Schieberegler (Beschreibung in Ui.hpp).
// Aufbau, von hinten nach vorn gezeichnet:
//   - Schiene: ein flaches Rechteck in der Mitte der Fläche (22 % der Höhe),
//     links vom Griff orange gefüllt (der eingestellte Anteil),
//   - Griff: ein Kreis an der Stelle des Werts.
// Damit der Griff bei 0 und bei 1 nicht über den Rand hinausragt, läuft die
// Schiene nur von x0 = links + Griffradius bis x1 = rechts - Griffradius.
// Umrechnung Zeiger -> Wert: (Zeiger-x - x0) / (x1 - x0), also 0 am linken
// und 1 am rechten Ende; danach aufs Raster runden: round(v / step) * step
// (bei step 0,05: 0,437 -> round(8,74) = 9 -> 0,45).
bool Slider(Rectangle r, float& value, bool& dragging, bool focused, float step) {
    const Vector2 m = PointerPos();
    const bool hover = CheckCollisionPointRec(m, r);
    const float knobR = r.height * 0.32f;
    const float x0 = r.x + knobR, x1 = r.x + r.width - knobR;

    // Ziehen beginnt mit einem Klick IN die Fläche und endet, sobald der
    // Zeiger losgelassen wird – egal, wo er dann ist.
    if (hover && PointerPressed()) dragging = true;
    if (dragging && !PointerDown()) dragging = false;

    const float before = value;
    if (dragging && x1 > x0) {
        float v = (m.x - x0) / (x1 - x0);
        if (step > 0.0f) v = roundf(v / step) * step;
        value = v;
    }
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;

    // --- Zeichnen ---------------------------------------------------------------
    const bool hot = focused || dragging || (hover && MouseActive());
    const float th = std::max(4.0f, r.height * 0.22f);
    const Rectangle track{x0, r.y + (r.height - th) * 0.5f, x1 - x0, th};
    DrawRectangleRec(track, Color{15, 18, 24, 235});
    DrawRectangleRec({track.x, track.y, track.width * value, track.height}, hot ? ACCENT : ACCENT_DIM);
    DrawRectangleLinesEx(track, 1.0f, Color{255, 255, 255, 40});
    const Vector2 knob{x0 + (x1 - x0) * value, r.y + r.height * 0.5f};
    DrawCircleV(knob, knobR, hot ? ACCENT : TEXT);
    DrawCircleV(knob, knobR * 0.45f, Color{26, 31, 41, 255});   // dunkler Punkt in der Mitte
    return value != before;
}

// Umschalter (Beschreibung in Ui.hpp): eine "Pille" (Rechteck mit
// halbrunden Enden) mit einem runden Knopf darin. AN: grüne Pille, Knopf
// rechts, Text "AN" links daneben. AUS: graue Pille, Knopf links, Text "AUS"
// rechts. So erkennt man den Zustand an Farbe, Lage UND Text.
// DrawRectangleRounded(r, roundness, segments, color): roundness 1 = die
// kürzere Seite wird ganz zum Halbkreis; segments = aus wie vielen
// Teilstücken eine Rundung gezeichnet wird (mehr = glatter).
bool Toggle(Rectangle r, bool on, float size, bool focused) {
    const bool hover = CheckCollisionPointRec(PointerPos(), r);
    const bool hot = focused || (hover && MouseActive());
    const Color fill = on ? Color{40, 130, 90, 255} : Color{50, 56, 70, 255};
    DrawRectangleRounded(r, 1.0f, 16, fill);
    DrawRectangleRoundedLinesEx(r, 1.0f, 16, Px(2.0f) < 1 ? 1 : Px(2.0f), hot ? ACCENT : Color{90, 100, 120, 255});
    const float knobR = r.height * 0.36f;
    const float pad = r.height * 0.5f;   // Mittelpunkt des Knopfs: eine halbe Höhe vom Rand
    const Vector2 knob{on ? r.x + r.width - pad : r.x + pad, r.y + r.height * 0.5f};
    DrawCircleV(knob, knobR, hot ? Color{255, 220, 170, 255} : TEXT);
    // Text in der freien Hälfte, senkrecht mittig
    const float ty = r.y + (r.height - size) * 0.5f;
    if (on) TextCentered("AN", r.x + (r.width - pad) * 0.5f, ty, size, TEXT, false);
    else TextCentered("AUS", r.x + pad + (r.width - pad) * 0.5f, ty, size, Color{200, 205, 215, 255}, false);
    return hover && PointerPressed();
}

// Ablauf pro Frame:
//   1. selected gültig machen (ungültig oder gesperrt -> erster wählbarer Eintrag)
//   2. Pfeiltasten/W/S oder Controller-Steuerkreuz: zum nächsten wählbaren
//      Eintrag, mit Umlauf am Ende
//   3. Knöpfe zeichnen; Klick wählt sofort, Zeigerbewegung über einem Knopf
//      verschiebt die Tastaturauswahl dorthin (so bleiben Zeiger und Tastatur
//      einig)
//   4. Enter/Leertaste bestätigen die aktuelle Auswahl. Controller-A auch –
//      außer der Controller-Zeiger steht gerade auf einem Knopf: Dann hat A
//      diesen Knopf schon in Schritt 3 angeklickt (PointerPressed), und ein
//      zweites Auslösen wäre ein Doppelklick.
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
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN)) {
        for (int k = 1; k <= n; ++k) {
            int i = (selected + k) % n;
            if (isEnabled(i)) { selected = i; break; }
        }
    }
    // Nach oben: + n * 2 hält den Wert vor dem % positiv (in C++ ist z. B.
    // -1 % 5 = -1, nicht 4).
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W) || PadPressed(GAMEPAD_BUTTON_LEFT_FACE_UP)) {
        for (int k = 1; k <= n; ++k) {
            int i = (selected - k + n * 2) % n;
            if (isEnabled(i)) { selected = i; break; }
        }
    }
    int chosen = -1;
    bool anyHover = false;
    for (int i = 0; i < n; ++i) {
        Rectangle r{cx - width * 0.5f, top + i * (height + gap), width, height};
        bool hover = false;
        if (Button(r, items[i], textSize, i == selected, isEnabled(i), &hover)) chosen = i;
        anyHover = anyHover || hover;
        // Nur bei BEWEGTEM Zeiger übernimmt der Knopf unter dem Zeiger die
        // Auswahl – ein ruhender Zeiger soll die Pfeiltasten nicht blockieren.
        if (hover && isEnabled(i) && PointerMoved()) selected = i;
    }
    const bool padConfirm = PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN) && !(PadPointerActive() && anyHover);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE) || padConfirm) {
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
    if (enabled && PointerPressed()) focused = CheckCollisionPointRec(PointerPos(), r);

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
    return GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
           AnyPadButtonPressed(g_pad);
}

} // namespace ui
