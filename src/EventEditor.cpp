// ---------------------------------------------------------------------------
// EventEditor.cpp – Event-Editor im Mehrspieler (Beschreibung in EventEditor.hpp).
//
// Aufbau des Bildschirms:
//   oben       Titel, eigenes Konto, heutiger Tag des Servers
//   links      Events des Servers (Klick wählt eins), "Neues Event"
//   rechts     Name, von/bis, Kartenliste (Mondkarte immer wählbar, jede
//              Einsatzkarte: wie sonst / wählbar / gesperrt), darunter was
//              an diesen Tagen im Command Center wählbar ist, Statuszeile
//              und die Knöpfe Speichern, Löschen, Zurück
//
// Bedienung mit Maus und Tastatur (Tab wechselt das Feld, Enter oder Strg+S
// speichert, ESC geht zurück zur Rollenwahl). Gespeichert wird nur auf dem
// Server; dieser Bildschirm hält nur das Formular.
// ---------------------------------------------------------------------------
#include "EventEditor.hpp"
#include "Audio.hpp"
#include "Util.hpp"
#include "raylib.h"
#include <algorithm>

using namespace ui;

namespace {

constexpr double kBusyTimeout = 8.0;      // Sekunden ohne Antwort, dann Knöpfe wieder frei
constexpr double kConfirmDelete = 3.0;    // so lange gilt "Wirklich löschen?"

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

// Text kürzen ("Sehr langer Na..."), damit er in maxW Pixel passt (wie in Multiplayer.cpp)
std::string Fit(const std::string& t, float size, float maxW) {
    if (Width(t, size) <= maxW) return t;
    std::string s = t;
    while (!s.empty() && Width(s + "...", size) > maxW) {
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // UTF-8-Folgebytes
        if (!s.empty()) s.pop_back();
    }
    return s + "...";
}

// "2026-11-07" -> "07.11.2026"
std::string DayText(const std::string& d) {
    if (!ValidEventDay(d)) return d;
    return d.substr(8, 2) + "." + d.substr(5, 2) + "." + d.substr(0, 4);
}

std::string RangeText(const MapEvent& e) {
    return e.from == e.to ? DayText(e.from) : DayText(e.from) + " - " + DayText(e.to);
}

// Eingabefilter: Eventname wie Chat (Zeichen der Schrift), Tage nur Ziffern und "-"
bool NameChar(int cp) { return net::ChatChar(cp); }
bool DayChar(int cp) { return (cp >= '0' && cp <= '9') || cp == '-'; }

} // namespace

EventEditorScreen::EventEditorScreen() {
    nameField_.placeholder = "z. B. Halloween-Wochenende";
    nameField_.maxChars = MAX_EVENT_NAME;
    nameField_.accept = NameChar;
    fromField_.placeholder = "JJJJ-MM-TT";
    fromField_.maxChars = 10;
    fromField_.accept = DayChar;
    toField_.placeholder = "nur ein Tag";
    toField_.maxChars = 10;
    toField_.accept = DayChar;
}

void EventEditorScreen::Open(const std::string& account) {
    account_ = account;
    list_ = net::EventListMsg{};
    fresh_ = true;
    selected_ = 0;
    picks_.clear();
    listScroll_ = 0;
    mapScroll_ = 0;
    busy_ = true;   // bis die erste Liste da ist
    busyAt_ = GetTime();
    confirmDeleteAt_ = -10.0;
    frames_ = 0;
    status_.clear();
}

void EventEditorScreen::SetList(const net::EventListMsg& list) {
    list_ = list;
    busy_ = false;
    auto find = [&](int id) -> const MapEvent* {
        for (const MapEvent& e : list_.events)
            if (e.id == id) return &e;
        return nullptr;
    };
    if (fresh_) {
        fresh_ = false;
        NewEvent();
    }
    if (list.savedId != 0) {
        // Gerade gespeichert: dieses Event bleibt gewählt (ein neues hat jetzt seine Nummer)
        selected_ = list.savedId;
        if (const MapEvent* e = find(list.savedId)) Select(*e);
    } else if (selected_ != 0 && !find(selected_)) {
        NewEvent();   // gelöscht (hier oder von jemand anderem)
    }
    if (!list.text.empty()) SetStatus(list.text, list.ok ? GOOD : BAD);
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, static_cast<int>(list_.events.size()) - 1));
    mapScroll_ = std::clamp(mapScroll_, 0, std::max(0, static_cast<int>(list_.maps.size()) - 1));
}

void EventEditorScreen::SetStatus(const std::string& text, Color c) {
    status_ = text;
    statusColor_ = c;
}

void EventEditorScreen::NewEvent() {
    selected_ = 0;
    nameField_.text.clear();
    fromField_.text = list_.today;
    toField_.text.clear();
    picks_.clear();
    mapScroll_ = 0;
    confirmDeleteAt_ = -10.0;
    nameField_.focused = true;
    fromField_.focused = toField_.focused = false;
}

void EventEditorScreen::Select(const MapEvent& e) {
    selected_ = e.id;
    nameField_.text = e.name;
    fromField_.text = e.from;
    toField_.text = e.to == e.from ? std::string() : e.to;
    picks_.clear();
    for (const std::string& f : e.maps) picks_[f] = 1;
    for (const std::string& f : e.locked) picks_[f] = 2;
    mapScroll_ = 0;
    confirmDeleteAt_ = -10.0;
    nameField_.focused = fromField_.focused = toField_.focused = false;
}

// Dieselben Regeln wie auf dem Server (GameServer.cpp, EventProblem), damit
// offensichtliche Fehler gar nicht erst hingeschickt werden.
bool EventEditorScreen::BuildEvent(MapEvent& out) {
    out = MapEvent{};
    out.id = selected_;
    out.name = Trim(nameField_.text);
    out.from = Trim(fromField_.text);
    out.to = Trim(toField_.text);
    if (out.to.empty()) out.to = out.from;   // Datum statt Zeitraum
    // Karten in der Reihenfolge der Liste; Karten, die es nicht mehr gibt, fallen weg
    for (const net::EventMapEntry& m : list_.maps) {
        auto it = picks_.find(m.file);
        if (it == picks_.end()) continue;
        (it->second == 1 ? out.maps : out.locked).push_back(m.file);
    }
    std::string problem;
    if (out.name.empty()) problem = "Bitte einen Namen eingeben.";
    else if (!ValidEventDay(out.from)) problem = "Bitte den ersten Tag als JJJJ-MM-TT eingeben (z. B. " + list_.today + ").";
    else if (!ValidEventDay(out.to)) problem = "Bitte den letzten Tag als JJJJ-MM-TT eingeben oder leer lassen (nur ein Tag).";
    else if (out.to < out.from) problem = "Der letzte Tag liegt vor dem ersten.";
    else if (out.maps.empty() && out.locked.empty()) problem = "Bitte mindestens eine Karte wählbar machen oder sperren.";
    else if (out.maps.size() > static_cast<size_t>(MAX_EVENT_MAPS) || out.locked.size() > static_cast<size_t>(MAX_EVENT_MAPS))
        problem = Fmt("Höchstens %d Karten wählbar und %d gesperrt.", MAX_EVENT_MAPS, MAX_EVENT_MAPS);
    if (problem.empty()) return true;
    SetStatus(problem, BAD);
    return false;
}

EventEditorAction EventEditorScreen::Draw() {
    ++frames_;
    const bool live = frames_ > 1;
    const double now = GetTime();
    if (busy_ && now - busyAt_ > kBusyTimeout) {
        busy_ = false;
        SetStatus("Keine Antwort vom Server. Bitte erneut versuchen.", BAD);
    }
    EventEditorAction act;
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const Vector2 mouse = PointerPos();
    const float wheel = GetMouseWheelMove();

    Text("EVENTS EINRICHTEN", 24 * s, 18 * s, 44 * s, ACCENT);
    Text(Fit("Angemeldet als " + account_ + " (Event Manager)  ·  Heute auf dem Server: " + DayText(list_.today) +
                 "  ·  Die Mondkarte ist immer wählbar.",
             18 * s, W - 48 * s),
         24 * s, 68 * s, 18 * s, MUTED);
    const float top = 100 * s, bottom = H - 64 * s;

    // --- links: Events ---------------------------------------------------------
    const Rectangle lp{24 * s, top, 430 * s, bottom - top};
    Panel(lp);
    float x = lp.x + 18 * s, y = lp.y + 16 * s;
    Text("EVENTS", x, y, 22 * s, ACCENT);
    const bool newEvent = Button({lp.x + lp.width - 18 * s - 180 * s, y - 6 * s, 180 * s, 38 * s}, "Neues Event", 18 * s, false, !busy_);
    if (newEvent && live) {
        g_audio.Play(Sfx::UiConfirm);
        NewEvent();
        SetStatus("", MUTED);
    }
    y += 46 * s;
    const float rowH = 70 * s;
    const float listBottom = lp.y + lp.height - 16 * s - (list_.hidden > 0 ? 26 * s : 0.0f);
    const int listRows = std::max(1, static_cast<int>((listBottom - y) / rowH));
    const int events = static_cast<int>(list_.events.size());
    if (wheel != 0.0f && CheckCollisionPointRec(mouse, lp)) listScroll_ -= static_cast<int>(wheel > 0 ? 1 : -1);
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, events - listRows));
    if (events == 0) Text("Noch keine Events.", x, y + 6 * s, 18 * s, MUTED);
    for (int i = listScroll_; i < events && i < listScroll_ + listRows; ++i) {
        const MapEvent& e = list_.events[static_cast<size_t>(i)];
        const Rectangle rr{lp.x + 8 * s, y, lp.width - 16 * s, rowH - 6 * s};
        const bool hover = CheckCollisionPointRec(mouse, rr);
        const bool sel = e.id == selected_;
        if (sel || hover) DrawRectangleRec(rr, PANEL_LIGHT);
        if (sel) DrawRectangleLinesEx(rr, 1.0f, ACCENT);
        if (live && hover && PointerPressed() && !busy_ && !sel) {
            g_audio.Play(Sfx::UiMove);
            Select(e);
            SetStatus("", MUTED);
        }
        const bool past = e.to < list_.today;
        const bool today = EventOnDay(e, list_.today);
        const char* state = today ? "läuft heute" : past ? "vorbei" : "geplant";
        const Color sc = today ? GOOD : past ? MUTED : SHIELD;
        const float stateW = Width(state, 16 * s);
        TextRight(state, rr.x + rr.width - 10 * s, y + 8 * s, 16 * s, sc);
        Text(Fit(e.name, 20 * s, rr.width - 30 * s - stateW), rr.x + 10 * s, y + 6 * s, 20 * s, sel ? ACCENT : past ? MUTED : TEXT);
        std::string info = RangeText(e);
        if (!e.maps.empty()) info += Fmt("  ·  %d wählbar", static_cast<int>(e.maps.size()));
        if (!e.locked.empty()) info += Fmt("  ·  %d gesperrt", static_cast<int>(e.locked.size()));
        Text(Fit(info, 16 * s, rr.width - 20 * s), rr.x + 10 * s, y + 36 * s, 16 * s, MUTED);
        y += rowH;
    }
    if (list_.hidden > 0)
        Text(Fmt("%d weitere nur auf der Verwaltungsseite", static_cast<int>(list_.hidden)), x, listBottom + 4 * s, 16 * s, MUTED);

    // --- rechts: gewähltes Event -------------------------------------------------
    const Rectangle rp{lp.x + lp.width + 20 * s, top, W - 24 * s - (lp.x + lp.width + 20 * s), bottom - top};
    Panel(rp);
    x = rp.x + 22 * s;
    y = rp.y + 16 * s;
    const float iw = rp.width - 44 * s, rx = x + iw;
    const MapEvent* cur = nullptr;
    for (const MapEvent& e : list_.events)
        if (e.id == selected_) cur = &e;
    Text(selected_ != 0 ? "EVENT BEARBEITEN" : "NEUES EVENT", x, y, 22 * s, ACCENT);
    if (cur) TextRight(Fmt("Nr. %d  ·  eingerichtet von %s", cur->id, cur->by.c_str()), rx, y + 4 * s, 16 * s, MUTED);
    y += 40 * s;

    // Name, von, bis
    const float dateW = 190 * s, gap = 16 * s, fh = 48 * s;
    const float nameW = iw - 2 * (dateW + gap);
    Text("Name", x, y, 18 * s, MUTED);
    Text("Von (JJJJ-MM-TT)", x + nameW + gap, y, 18 * s, MUTED);
    Text("Bis (leer = nur ein Tag)", x + nameW + gap + dateW + gap, y, 18 * s, MUTED);
    y += 24 * s;
    CycleFocus({&nameField_, &fromField_, &toField_});   // Tab wechselt das Feld
    bool submit = nameField_.Draw({x, y, nameW, fh}, 22 * s, !busy_);
    submit = fromField_.Draw({x + nameW + gap, y, dateW, fh}, 22 * s, !busy_) || submit;
    submit = toField_.Draw({x + nameW + gap + dateW + gap, y, dateW, fh}, 22 * s, !busy_) || submit;
    y += fh + 18 * s;

    // Kartenliste: Mondkarte fest, dann alle Einsatzkarten des Servers
    Text("KARTEN AN DIESEN TAGEN", x, y, 18 * s, MUTED);
    TextRight("Klick schaltet: wie sonst > wählbar > gesperrt", rx, y + 2 * s, 16 * s, MUTED);
    y += 30 * s;
    const float bh = 50 * s;
    const float buttonsY = rp.y + rp.height - 16 * s - bh;
    const float mapH = 44 * s;
    const float mapBottom = buttonsY - 12 * s - 28 * s - 28 * s;   // darunter Zusammenfassung und Statuszeile
    const float stateW = 170 * s, metaX = x + iw * 0.45f;
    auto rowBack = [&](float ry, bool odd) {
        if (odd) DrawRectangleRec({rp.x + 8 * s, ry - 4 * s, rp.width - 16 * s, mapH - 4 * s}, Color{15, 18, 24, 120});
    };
    rowBack(y, false);
    Text("Mondkarte", x, y + 8 * s, 20 * s, TEXT);
    Text("Standardkarte, gemeinsam mit den Piloten", metaX, y + 10 * s, 16 * s, MUTED);
    TextCentered("immer wählbar", rx - stateW * 0.5f, y + 9 * s, 18 * s, GOOD);
    y += mapH;
    const Rectangle mapArea{rp.x, y, rp.width, mapBottom - y};
    const int mapRows = std::max(1, static_cast<int>((mapBottom - y) / mapH));
    const int maps = static_cast<int>(list_.maps.size());
    if (wheel != 0.0f && CheckCollisionPointRec(mouse, mapArea)) mapScroll_ -= static_cast<int>(wheel > 0 ? 1 : -1);
    mapScroll_ = std::clamp(mapScroll_, 0, std::max(0, maps - mapRows));
    if (maps == 0)
        Text(Fit("Auf dem Server gibt es noch keine Einsatzkarten (Karteneditor: Kartenart Einsatzkarte, \"An Server senden\").",
                 16 * s, iw),
             x, y + 10 * s, 16 * s, MUTED);
    static const char* kStateNames[3] = {"wie sonst", "wählbar", "gesperrt"};
    for (int i = mapScroll_; i < maps && i < mapScroll_ + mapRows; ++i) {
        const net::EventMapEntry& m = list_.maps[static_cast<size_t>(i)];
        rowBack(y, (i - mapScroll_) % 2 == 0);
        auto it = picks_.find(m.file);
        const int st = it == picks_.end() ? 0 : it->second;
        Text(Fit(m.name, 20 * s, metaX - x - 12 * s), x, y + 8 * s, 20 * s, TEXT);
        Text(Fit("von " + m.author + (m.released ? "  ·  freigegeben" : "  ·  nicht freigegeben"), 16 * s, rx - stateW - 12 * s - metaX),
             metaX, y + 10 * s, 16 * s, m.released ? GOOD : MUTED);
        const Rectangle br{rx - stateW, y, stateW, mapH - 8 * s};
        if (Button(br, "", 18 * s, false, !busy_) && live) {
            g_audio.Play(Sfx::UiMove);
            if (st == 2) picks_.erase(m.file);
            else picks_[m.file] = st + 1;
        }
        TextCentered(kStateNames[st], br.x + br.width * 0.5f, br.y + (br.height - 18 * s) * 0.5f, 18 * s,
                     st == 1 ? GOOD : st == 2 ? BAD : TEXT);
        y += mapH;
    }
    if (maps > mapRows)
        TextRight(Fmt("%d-%d von %d (Mausrad)", mapScroll_ + 1, std::min(maps, mapScroll_ + mapRows), maps), rx, mapBottom + 2 * s,
                  16 * s, MUTED);

    // Was an diesen Tagen im Command Center wählbar ist (ohne andere Events)
    int usable = 0;
    for (const net::EventMapEntry& m : list_.maps) {
        auto it = picks_.find(m.file);
        const int st = it == picks_.end() ? 0 : it->second;
        if (st == 1 || (st == 0 && m.released)) ++usable;
    }
    y = mapBottom + 2 * s;
    Text(Fmt("Im Command Center wählbar: Mondkarte + %d Einsatzkarte(n) (ohne andere Events)", usable), x, y, 16 * s, TEXT);
    y += 28 * s;
    if (busy_) Text("Warte auf den Server ...", x, y, 18 * s, MUTED);
    else if (!status_.empty()) Text(Fit(status_, 18 * s, iw), x, y, 18 * s, statusColor_);

    // Knöpfe
    const bool confirming = now - confirmDeleteAt_ < kConfirmDelete;
    const bool save = Button({x, buttonsY, 220 * s, bh}, busy_ ? "Speichere ..." : "Speichern", 22 * s, true, !busy_);
    const bool del = Button({x + 236 * s, buttonsY, 240 * s, bh}, confirming ? "Wirklich löschen?" : "Löschen", 22 * s, confirming,
                            !busy_ && selected_ != 0);
    const bool back = Button({rx - 190 * s, buttonsY, 190 * s, bh}, "Zurück", 22 * s, false);
    const bool ctrlS = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_S);
    if (!live) return act;
    if (!busy_ && (save || submit || ctrlS)) {
        if (BuildEvent(act.event)) {
            g_audio.Play(Sfx::UiConfirm);
            act.kind = EventEditorAction::Kind::Save;
            busy_ = true;
            busyAt_ = now;
        } else {
            g_audio.Play(Sfx::Denied);
        }
    } else if (!busy_ && del) {
        if (!confirming) {
            g_audio.Play(Sfx::UiMove);
            confirmDeleteAt_ = now;
        } else {
            g_audio.Play(Sfx::UiConfirm);
            act.kind = EventEditorAction::Kind::Delete;
            act.event.id = selected_;
            busy_ = true;
            busyAt_ = now;
            confirmDeleteAt_ = -10.0;
        }
    } else if (back || BackPressed()) {
        g_audio.Play(Sfx::UiBack);
        act.kind = EventEditorAction::Kind::Back;
    }
    return act;
}
