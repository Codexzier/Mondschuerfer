// ---------------------------------------------------------------------------
// Chat.cpp – Chatfenster unten links und Sprechblase über tippenden Spielern.
// Bedienung und Aufbau stehen in Chat.hpp.
//
// Ablauf pro Frame (aus MultiplayerScreen):
//   Update()  Enter öffnet, ESC schließt (vor der Flugsteuerung)
//   Draw()    Fenster, Verlauf, Eingabezeile; Enter in der Eingabezeile
//             liefert den Text zum Senden. Das Textfeld (ui::TextField)
//             nimmt dabei die getippten Zeichen, die Rücktaste und Strg+V.
// ---------------------------------------------------------------------------
#include "Chat.hpp"
#include "Protocol.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace ui;

namespace {

constexpr size_t MAX_ENTRIES = 50;   // so viele Nachrichten bleiben im Verlauf
constexpr int VISIBLE_LINES = 5;     // so viele Zeilen zeigt das Fenster

// Maße bei Referenzgröße (werden mit Scale() multipliziert)
constexpr float kWidth = 470.0f;
constexpr float kPad = 10.0f;        // Innenrand
constexpr float kTextSize = 18.0f;
constexpr float kLineH = 23.0f;
constexpr float kGap = 6.0f;         // zwischen Verlauf und Eingabezeile
constexpr float kInputH = 30.0f;

// Eine sichtbare Zeile. name ("Name:") nur in der ersten Zeile einer Nachricht.
struct Line {
    std::string name;
    std::string text;
    bool mine = false;
};

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

// Länge des UTF-8-Zeichens ab s[i] in Bytes (Folgebytes haben die Form 10xxxxxx)
size_t CharBytes(const std::string& s, size_t i) {
    size_t n = 1;
    while (i + n < s.size() && (static_cast<unsigned char>(s[i + n]) & 0xC0) == 0x80) ++n;
    return n;
}

// Nachricht in Zeilen zerlegen, die höchstens maxW Pixel breit sind: an
// Leerzeichen; ein Wort, das allein nicht passt, Zeichen für Zeichen. Die
// erste Zeile beginnt mit dem Namen (prefix).
void WrapEntry(const std::string& prefix, const std::string& text, bool mine, float maxW, float size,
               std::vector<Line>& out) {
    std::string line = prefix;
    bool first = true;
    auto push = [&]() {
        Line l;
        if (first) {
            l.name = prefix;
            l.text = line.substr(prefix.size());
        } else {
            l.text = line;
        }
        l.mine = mine;
        out.push_back(l);
        first = false;
        line.clear();
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(' ', pos);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(pos, end - pos);
        pos = end + 1;
        if (word.empty()) continue;   // mehrere Leerzeichen hintereinander
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (Width(tryLine, size) <= maxW) {
            line = tryLine;
            continue;
        }
        if (!line.empty()) push();
        if (Width(word, size) <= maxW) {
            line = word;
            continue;
        }
        for (size_t i = 0; i < word.size();) {
            const std::string ch = word.substr(i, CharBytes(word, i));
            i += ch.size();
            if (!line.empty() && Width(line + ch, size) > maxW) push();
            line += ch;
        }
    }
    if (first || !line.empty()) push();
}

} // namespace

ChatBox::ChatBox() {
    input_.maxChars = net::MAX_CHAT;
    input_.accept = net::ChatChar;
    input_.placeholder = "Nachricht (Enter senden, ESC abbrechen)";
}

void ChatBox::Add(const std::string& name, const std::string& text, bool mine) {
    entries_.push_back({name, text, mine});
    if (entries_.size() > MAX_ENTRIES) entries_.pop_front();
}

void ChatBox::Clear() {
    entries_.clear();
    input_.text.clear();
    input_.focused = false;
    scroll_ = 0;
    lastLines_ = 0;
}

void ChatBox::Close() {
    input_.focused = false;
    closedFrame_ = FrameIndex();
}

void ChatBox::Update(bool allowed) {
    if (!allowed) {
        if (Active()) Close();
        return;
    }
    if (Active()) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            input_.text.clear();
            Close();
        }
    } else if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !TextInputActive()) {
        input_.focused = true;
        input_.lastEdit = GetTime();   // Cursor sofort sichtbar
        openedFrame_ = FrameIndex();
    }
}

bool ChatBox::Busy() const {
    const unsigned f = FrameIndex();
    return Active() || openedFrame_ == f || closedFrame_ == f;
}

bool ChatBox::WantsWheel() const { return Active() && CheckCollisionPointRec(PointerPos(), rect_); }

float ChatBox::Height() { return (kPad + VISIBLE_LINES * kLineH + kGap + kInputH + kPad) * Scale(); }

bool ChatBox::Draw(std::string& send) {
    const float s = Scale();
    const float ts = kTextSize * s;
    const float h = Height();
    rect_ = {16 * s, static_cast<float>(GetScreenHeight()) - 16 * s - h, kWidth * s, h};

    // Schwarz, halb durchsichtig; bei offener Eingabe nur zu 20 % durchsichtig.
    DrawRectangleRec(rect_, Color{0, 0, 0, static_cast<unsigned char>(Active() ? 204 : 128)});
    BlockPointer(rect_);   // ein Klick aufs Fenster setzt kein Flugziel

    // --- Eingabezeile ----------------------------------------------------------
    // Die Enter-Taste, die die Eingabe geöffnet hat, darf sie nicht gleich
    // wieder abschicken (openedFrame_). Ein Klick neben das Feld nimmt ihm den
    // Fokus (TextField::Draw); dann ist die Eingabe zu, der Text bleibt.
    const Rectangle ir{rect_.x + kPad * s, rect_.y + h - kPad * s - kInputH * s, rect_.width - 2 * kPad * s, kInputH * s};
    bool sent = false;
    if (Active()) {
        const bool enter = input_.Draw(ir, ts);
        if (enter && openedFrame_ != FrameIndex()) {
            const std::string text = Trim(input_.text);
            if (net::ValidChatText(text)) {   // leer: nichts senden, nur schließen
                send = text;
                sent = true;
            }
            input_.text.clear();
            Close();
        } else if (!input_.focused) {
            closedFrame_ = FrameIndex();
        }
    } else {
        Text("[Enter] Chat", ir.x + 4 * s, ir.y + (ir.height - 16 * s) * 0.5f, 16 * s, Color{255, 255, 255, 140}, false);
    }

    // --- Verlauf ----------------------------------------------------------------
    // Zeilen jedes Frame neu umbrechen (höchstens 50 kurze Nachrichten). Rechts
    // bleibt Platz für die Bildlaufleiste.
    std::vector<Line> lines;
    const float textW = rect_.width - 2 * kPad * s - 10 * s;
    for (const Entry& e : entries_) WrapEntry(e.name + ":", e.text, e.mine, textW, ts, lines);
    const int n = static_cast<int>(lines.size());

    // Blättern: scroll_ zählt Zeilen vom neuesten Ende. Kommt beim Blättern
    // eine Nachricht dazu, bleibt die Ansicht stehen. Das Mausrad liefert
    // Rasten (Touchpads auch Bruchteile davon, die sammeln wir in wheel_).
    if (scroll_ > 0 && n > lastLines_) scroll_ += n - lastLines_;
    lastLines_ = n;
    if (WantsWheel()) {
        wheel_ += GetMouseWheelMove();
        for (; wheel_ >= 1.0f; wheel_ -= 1.0f) ++scroll_;
        for (; wheel_ <= -1.0f; wheel_ += 1.0f) --scroll_;
    } else {
        wheel_ = 0.0f;
    }
    if (!Active()) scroll_ = 0;
    scroll_ = std::clamp(scroll_, 0, std::max(0, n - VISIBLE_LINES));

    // Die Zeilen stehen unten bündig über der Eingabezeile.
    const int firstLine = std::max(0, n - VISIBLE_LINES - scroll_);
    const int count = std::min(VISIBLE_LINES, n - firstLine);
    const float bottom = ir.y - kGap * s;
    for (int i = 0; i < count; ++i) {
        const Line& l = lines[static_cast<size_t>(firstLine + i)];
        const float y = bottom - static_cast<float>(count - i) * kLineH * s + (kLineH - kTextSize) * 0.5f * s;
        float x = rect_.x + kPad * s;
        if (!l.name.empty()) {
            Text(l.name, x, y, ts, l.mine ? ACCENT : GOOD, false);
            x += Width(l.name, ts);
        }
        Text(l.text, x, y, ts, WHITE, false);
    }

    // Bildlaufleiste: nur beim Blättern (Eingabe offen) und wenn es mehr gibt
    if (Active() && n > VISIBLE_LINES) {
        const float th = VISIBLE_LINES * kLineH * s;
        const Rectangle track{rect_.x + rect_.width - kPad * s - 3 * s, bottom - th, 3 * s, th};
        DrawRectangleRec(track, Color{255, 255, 255, 40});
        DrawRectangleRec({track.x, track.y + th * static_cast<float>(firstLine) / static_cast<float>(n), track.width,
                          th * static_cast<float>(VISIBLE_LINES) / static_cast<float>(n)},
                         Color{255, 255, 255, 170});
    }
    return sent;
}

// Weiße Blase mit kleiner Spitze nach unten; die drei Punkte werden der
// Reihe nach dunkel ("tippt ..."), einmal pro Sekunde.
void DrawChatEmote(Vector2 at, float time) {
    const float s = Scale();
    const float w = 40 * s, h = 24 * s, tip = 6 * s;
    const Rectangle r{at.x - w * 0.5f, at.y - tip - h, w, h};
    const Color fill{255, 255, 255, 235};
    DrawRectangleRounded(r, 0.5f, 8, fill);
    // raylib zeichnet Dreiecke nur mit Ecken gegen den Uhrzeigersinn
    DrawTriangle({at.x - tip, r.y + h - 1}, {at.x, at.y}, {at.x + tip, r.y + h - 1}, fill);
    for (int i = 0; i < 3; ++i) {
        const float pulse = 0.5f + 0.5f * sinf(time * 6.3f - static_cast<float>(i) * 1.1f);
        DrawCircleV({r.x + w * (0.27f + 0.23f * static_cast<float>(i)), r.y + h * 0.5f}, 3.2f * s,
                    Color{30, 34, 44, static_cast<unsigned char>(90 + 165 * pulse)});
    }
}
