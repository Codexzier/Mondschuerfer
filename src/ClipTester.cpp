// ---------------------------------------------------------------------------
// ClipTester.cpp – Seite "Videoclip Tester" der Optionen, nur im Debug-Build
// (Schnittstelle und Zweck: ClipTester.hpp).
//
// Ablauf beim Abspielen (Play):
//   1. Prüfen (Probe): die ersten Bytes der Datei zeigen das Dateiformat
//      (MPEG Program Stream, MP4, Matroska ...). Nur ein MPEG-1 Program
//      Stream kann pl_mpeg lesen; dann liefert pl_mpeg Bildgröße, Bildrate und
//      Länge, und ein Durchlauf durch die ersten Pakete (plm_demux) zeigt, ob
//      das Bild MPEG-2 ist (Erweiterungs-Startcode 0x000001B5) und wie der
//      Ton kodiert ist (Kopf des ersten Tonrahmens).
//   2. Daraus je Grundbedingung eine Zeile (Checks).
//   3. Abspielen mit ClipPlayer::PlayFile – immer, auch bei Fehlern.
// Die Sollwerte stammen aus den Clips in assets/clips/ (ffprobe, 2026-11-14:
// alle MPEG-1-Video 480x480, 30 fps, MP2 32000 Hz mono, 3,6 bis 8,0 s).
// ---------------------------------------------------------------------------
#include "ClipTester.hpp"
#ifdef MONDSCHUERFER_CLIP_TESTER
#include "Assets.hpp"
#include "ClientConfig.hpp"
#include "Config.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>   // vor pl_mpeg.h: es braucht size_t und FILE
#include <cstring>
#include "pl_mpeg.h"

using namespace ui;

namespace {

// Grundbedingungen der Spiel-Clips
constexpr int kWantW = 480, kWantH = 480;
constexpr double kWantFps = 30.0;
constexpr int kWantRate = 32000;
constexpr int kWantChannels = 1;

// Maße (in Pixeln bei Fensterhöhe 900, werden mit Scale() multipliziert)
constexpr float kListW = 340.0f;     // Breite der Liste links
constexpr float kGap = 30.0f;        // Abstand Liste - Video
constexpr float kListRowH = 28.0f;   // Zeilenhöhe der Liste
constexpr float kVideo = 320.0f;     // Kantenlänge des Videobilds
constexpr float kCheckH = 23.0f;     // Zeilenhöhe des Prüfergebnisses

// Was die Prüfung über eine Datei herausfindet
struct Probe {
    bool found = false;                 // Datei ließ sich öffnen
    std::string container = "unbekannt";
    bool mpeg1ps = false;               // MPEG-1 Program Stream (nur den liest pl_mpeg)
    bool video = false;                 // Bildstrom mit Kopf gefunden
    bool mpeg2Video = false;
    int w = 0, h = 0;
    double fps = 0.0, duration = 0.0;
    bool audio = false;                 // Kopf eines MPEG-Tonrahmens gefunden
    int version = 0, layer = 0;         // Bits aus dem Rahmenkopf (siehe AudioHeader)
    int rate = 0, channels = 0;
};

bool StartsWith(const unsigned char* b, size_t n, size_t at, const char* sig, size_t len) {
    return n >= at + len && std::memcmp(b + at, sig, len) == 0;
}

// Dateiformat an den ersten Bytes erkennen
void DetectContainer(const unsigned char* b, size_t n, Probe& p) {
    if (n >= 5 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xBA) {
        // Pack-Header: MPEG-1 beginnt mit den Bits 0010, MPEG-2 mit 01
        if ((b[4] & 0xF0) == 0x20) {
            p.container = "MPEG-1 Program Stream";
            p.mpeg1ps = true;
        } else if ((b[4] & 0xC0) == 0x40) {
            p.container = "MPEG-2 Program Stream";
        } else {
            p.container = "MPEG Program Stream (unbekannt)";
        }
    } else if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xB3) {
        p.container = "MPEG-Video ohne Container";
    } else if (StartsWith(b, n, 4, "ftyp", 4)) {
        p.container = StartsWith(b, n, 8, "qt  ", 4) ? "QuickTime (MOV)" : "MP4";
    } else if (StartsWith(b, n, 0, "\x1A\x45\xDF\xA3", 4)) {
        p.container = "Matroska/WebM";
    } else if (StartsWith(b, n, 0, "RIFF", 4) && StartsWith(b, n, 8, "AVI ", 4)) {
        p.container = "AVI";
    } else if (StartsWith(b, n, 0, "OggS", 4)) {
        p.container = "Ogg";
    } else if (StartsWith(b, n, 0, "FLV", 3)) {
        p.container = "FLV";
    } else if (n > 188 && b[0] == 0x47 && b[188] == 0x47) {
        p.container = "MPEG-TS";
    }
}

// Kopf des ersten MPEG-Tonrahmens in d: 11 Bit Sync, Version (2 Bit:
// 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5), Layer (2 Bit: 3 = I, 2 = II,
// 1 = III), Schutz, Bitrate (4), Abtastrate (2), Füllbit, privat,
// Kanalmodus (2 Bit: 3 = Mono).
bool AudioHeader(const unsigned char* d, size_t n, Probe& p) {
    static const int kRates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) continue;
        const int version = (d[i + 1] >> 3) & 3;
        const int layer = (d[i + 1] >> 1) & 3;
        const int bitrate = (d[i + 2] >> 4) & 15;
        const int rate = (d[i + 2] >> 2) & 3;
        if (version == 1 || layer == 0 || bitrate == 15 || rate == 3) continue;   // ungültige Werte: kein Kopf
        p.audio = true;
        p.version = version;
        p.layer = layer;
        p.rate = kRates[version][rate];
        p.channels = ((d[i + 3] >> 6) & 3) == 3 ? 1 : 2;
        return true;
    }
    return false;
}

Probe Examine(const std::string& path) {
    Probe p;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return p;
    p.found = true;
    unsigned char head[512];
    const size_t n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    DetectContainer(head, n, p);
    if (!p.mpeg1ps) return p;   // anderes liest pl_mpeg nicht (siehe ClipPlayer.hpp)

    // Bildgröße, Bildrate, Länge: so, wie der ClipPlayer sie sieht
    if (plm_t* m = plm_create_with_filename(path.c_str())) {
        p.w = plm_get_width(m);
        p.h = plm_get_height(m);
        p.video = p.w > 0 && p.h > 0;
        p.fps = plm_get_framerate(m);
        p.duration = plm_get_duration(m);
        plm_destroy(m);
    }
    // Erste Pakete: Bild MPEG-2? Ton-Rahmenkopf (erster Tonstrom, wie im Spiel)
    if (plm_buffer_t* buf = plm_buffer_create_with_filename(path.c_str())) {
        plm_demux_t* dmx = plm_demux_create(buf, 1);
        bool seenVideo = false;
        for (int i = 0; i < 500 && !(seenVideo && p.audio); ++i) {
            plm_packet_t* pk = plm_demux_decode(dmx);
            if (!pk) break;
            if (pk->type == PLM_DEMUX_PACKET_VIDEO_1 && !seenVideo) {
                seenVideo = true;
                static const unsigned char kExt[4] = {0, 0, 1, 0xB5};
                for (size_t k = 0; k + 4 <= pk->length; ++k)
                    if (std::memcmp(pk->data + k, kExt, 4) == 0) {
                        p.mpeg2Video = true;
                        break;
                    }
            } else if (pk->type == PLM_DEMUX_PACKET_AUDIO_1 && !p.audio) {
                AudioHeader(pk->data, pk->length, p);
            }
        }
        plm_demux_destroy(dmx);
    }
    return p;
}

// Zahl mit Komma statt Punkt (deutsche Schreibweise)
std::string Decimal(double v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    std::string s = buf;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// Eine Zeile des Prüfergebnisses
void Add(std::vector<std::string>& out, std::vector<bool>& ok, const std::string& label, const std::string& value,
         bool valid, const std::string& want) {
    out.push_back(label + ": " + value + (valid ? " is valid" : " is not valid (" + want + ")"));
    ok.push_back(valid);
}

} // namespace

void ClipTester::Open() {
    dir_ = g_assets.AssetDir() + "clips/";
    files_.clear();
    first_ = 0;
    if (DirectoryExists(dir_.c_str())) {
        FilePathList list = LoadDirectoryFilesEx(dir_.c_str(), ".mpg", false);
        for (unsigned int i = 0; i < list.count; ++i) files_.push_back(GetFileName(list.paths[i]));
        UnloadDirectoryFiles(list);
    }
    std::sort(files_.begin(), files_.end());
    if (IsFileDropped()) UnloadDroppedFiles(LoadDroppedFiles());   // früher (auf anderen Seiten) Hineingezogenes verwerfen
}

void ClipTester::Stop() {
    player_.Shutdown();
    playing_.clear();
    error_.clear();
    checks_.clear();
    path_.focused = false;
}

void ClipTester::PlayTestFile() {
    // Leerzeichen und Anführungszeichen außen weg ("Als Pfad kopieren" unter Windows)
    std::string p = path_.text;
    while (!p.empty() && std::isspace(static_cast<unsigned char>(p.back()))) p.pop_back();
    while (!p.empty() && std::isspace(static_cast<unsigned char>(p.front()))) p.erase(0, 1);
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') p = p.substr(1, p.size() - 2);
    if (p.empty()) {
        Stop();
        error_ = "Kein Pfad eingegeben";
        path_.focused = true;
        return;
    }
    Play(p);
}

void ClipTester::Play(const std::string& path) {
    Stop();
    playing_ = path;
    const Probe p = Examine(path);
    if (!p.found) {
        error_ = "Datei nicht gefunden";
        checks_.push_back({std::string("Datei: ") + GetFileName(path.c_str()) + " nicht gefunden", false});
        return;
    }

    std::vector<std::string> lines;
    std::vector<bool> ok;
    std::string ext = GetFileExtension(path.c_str()) ? GetFileExtension(path.c_str()) : "";
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    Add(lines, ok, "Dateiendung", ext.empty() ? "keine" : ext, ext == ".mpg", "erwartet .mpg");
    Add(lines, ok, "Dateiformat", p.container, p.mpeg1ps, "erwartet MPEG-1 Program Stream");
    if (!p.mpeg1ps) {
        for (const char* label : {"Videocodec", "Videoformat", "Frame per second", "Länge", "Tonformat", "Abtastrate",
                                  "Tonkanäle"})
            Add(lines, ok, label, "unbekannt", false, "Datei nicht lesbar");
    } else {
        if (!p.video) Add(lines, ok, "Videocodec", "kein Bild", false, "erwartet MPEG-1");
        else Add(lines, ok, "Videocodec", p.mpeg2Video ? "MPEG-2" : "MPEG-1", !p.mpeg2Video, "erwartet MPEG-1");
        Add(lines, ok, "Videoformat", p.video ? std::to_string(p.w) + "x" + std::to_string(p.h) : "unbekannt",
            p.w == kWantW && p.h == kWantH, "erwartet " + std::to_string(kWantW) + "x" + std::to_string(kWantH));
        const bool wholeFps = std::fabs(p.fps - std::round(p.fps)) < 0.005;
        Add(lines, ok, "Frame per second",
            p.fps > 0.0 ? (wholeFps ? std::to_string(static_cast<int>(std::lround(p.fps))) : Decimal(p.fps, 2)) : "unbekannt",
            std::fabs(p.fps - kWantFps) < 0.01, "erwartet " + std::to_string(static_cast<int>(kWantFps)));
        Add(lines, ok, "Länge", p.duration > 0.0 ? Decimal(p.duration, 1) + " s" : "unbekannt",
            p.duration > 0.0 && p.duration <= cfg::CUE_TIME,
            "höchstens " + std::to_string(static_cast<int>(cfg::CUE_TIME)) + " s");
        if (!p.audio) {
            Add(lines, ok, "Tonformat", "kein Ton", false, "erwartet MP2");
            Add(lines, ok, "Abtastrate", "unbekannt", false, "erwartet " + std::to_string(kWantRate) + " Hz");
            Add(lines, ok, "Tonkanäle", "unbekannt", false, "erwartet 1 (Mono)");
        } else {
            static const char* const kVersion[4] = {"MPEG-2.5", "?", "MPEG-2", "MPEG-1"};
            static const char* const kLayer[4] = {"?", "Layer III", "Layer II", "Layer I"};
            const bool mp2 = p.version == 3 && p.layer == 2;
            const std::string name = mp2 ? std::string("MP2 (MPEG-1 Layer II)")
                                         : std::string(kVersion[p.version]) + " " + kLayer[p.layer];
            Add(lines, ok, "Tonformat", name, mp2, "erwartet MPEG-1 Layer II");
            Add(lines, ok, "Abtastrate", std::to_string(p.rate) + " Hz", p.rate == kWantRate,
                "erwartet " + std::to_string(kWantRate) + " Hz");
            Add(lines, ok, "Tonkanäle", p.channels == 1 ? "1 (Mono)" : "2 (Stereo)", p.channels == kWantChannels,
                "erwartet 1 (Mono)");
        }
    }
    for (size_t i = 0; i < lines.size(); ++i) checks_.push_back({lines[i], ok[i]});

    // Immer versuchen abzuspielen, auch wenn die Prüfung Fehler zeigt
    player_.SetVolume(g_clientConfig.clipVolume);
    if (!player_.PlayFile(path))
        error_ = p.mpeg1ps ? std::string("Kein lesbares MPEG-1-Bild") : p.container + " kann das Spiel nicht lesen";
}

// Aufbau: links Überschrift, Liste (Mausrad blättert), Pfadfeld, Knopf,
// Hinweis; rechts Dateiname, Videobild (oder Fehlermeldung), Prüfergebnis
// und bei Fehlern der ffmpeg-Befehl, der eine passende Datei erzeugt.
bool ClipTester::Draw(Rectangle area, bool focused) {
    const float s = Scale();
    const Vector2 mouse = PointerPos();
    bool over = false;

    // Datei ins Fenster gezogen: Pfad übernehmen und gleich abspielen
    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        if (dropped.count > 0) {
            path_.text = dropped.paths[0];
            PlayTestFile();
        }
        UnloadDroppedFiles(dropped);
    }
    if (path_.focused && IsKeyPressed(KEY_ESCAPE)) path_.focused = false;   // erst ESC verlässt das Feld
    player_.UpdateFile(GetFrameTime());

    // --- links: Liste der Clips --------------------------------------------------
    const float lw = kListW * s;
    Text("Avatar-Clips (assets/clips)", area.x, area.y, 20 * s, MUTED);
    TextRight(std::to_string(files_.size()), area.x + lw, area.y, 20 * s, MUTED);
    const float bottom = (12.0f + 44.0f + 10.0f + 50.0f + 10.0f + 18.0f) * s;   // Abstand, Feld, Knopf, Hinweis
    const Rectangle list{area.x, area.y + 30 * s, lw, area.height - 30 * s - bottom};
    DrawRectangleRec(list, Color{15, 18, 24, 150});
    DrawRectangleLinesEx(list, 1.0f, ACCENT_DIM);
    const float rh = kListRowH * s;
    const int visible = std::max(1, static_cast<int>(list.height / rh));
    const int count = static_cast<int>(files_.size());
    if (CheckCollisionPointRec(mouse, list)) {
        over = true;
        first_ -= static_cast<int>(GetMouseWheelMove());
    }
    first_ = std::clamp(first_, 0, std::max(0, count - visible));
    if (files_.empty()) Text("Keine .mpg-Dateien gefunden", list.x + 12 * s, list.y + 10 * s, 18 * s, MUTED, false);
    for (int i = first_; i < count && i < first_ + visible; ++i) {
        const Rectangle r{list.x, list.y + static_cast<float>(i - first_) * rh, list.width, rh};
        const bool hover = CheckCollisionPointRec(mouse, r);
        const bool current = playing_ == dir_ + files_[i];
        if (current) DrawRectangleRec(r, Color{70, 52, 30, 200});
        else if (hover) DrawRectangleRec(r, PANEL_LIGHT);
        if (current) DrawRectangleRec({r.x, r.y, 4 * s, r.height}, ACCENT);
        Text(files_[i], r.x + 12 * s, r.y + (rh - 17 * s) * 0.5f, 17 * s, current ? ACCENT : TEXT, false);
        if (hover && PointerPressed()) Play(dir_ + files_[i]);
    }
    if (count > visible) {   // Bildlaufleiste
        const float bh = list.height * static_cast<float>(visible) / static_cast<float>(count);
        const float by = list.y + (list.height - bh) * static_cast<float>(first_) / static_cast<float>(count - visible);
        DrawRectangleRec({list.x + list.width - 5 * s, by, 4 * s, bh}, ACCENT_DIM);
    }

    float y = list.y + list.height + 12 * s;
    const Rectangle field{area.x, y, lw, 44 * s};
    if (CheckCollisionPointRec(mouse, field)) over = true;
    if (path_.Draw(field, 18 * s)) PlayTestFile();
    y += 54 * s;
    bool hover = false;
    if (Button({area.x, y, lw, 50 * s}, "Teste Video Datei abspielen", 20 * s, focused, true, &hover)) PlayTestFile();
    over = over || hover;
    y += 60 * s;
    Text("Pfad eintippen/einfügen oder Datei ins Fenster ziehen", area.x, y, 14 * s, MUTED);

    // --- rechts: Video und Prüfergebnis ---------------------------------------------
    const float rx = area.x + lw + kGap * s;
    const float rw = area.x + area.width - rx;
    // Zu lange Zeilen werden kleiner geschrieben, damit sie in die Spalte passen
    auto fit = [&](const std::string& t, float size) {
        while (size > 11 * s && Width(t, size) > rw) size -= 1.0f;
        return size;
    };
    const std::string name = playing_.empty() ? std::string("Kein Clip gewählt") : std::string(GetFileName(playing_.c_str()));
    Text(name, rx, area.y, fit(name, 20 * s), playing_.empty() ? MUTED : TEXT);
    const float vs = kVideo * s;
    const Rectangle vr{rx, area.y + 30 * s, vs, vs};
    DrawRectangleRec(vr, Color{8, 10, 14, 255});
    if (const Texture2D* t = player_.FileFrame()) {
        DrawTexturePro(*t, Rectangle{0, 0, static_cast<float>(t->width), static_cast<float>(t->height)}, vr, Vector2{0, 0},
                       0.0f, WHITE);
    } else if (!error_.empty()) {
        TextCentered("Nicht abspielbar", vr.x + vs * 0.5f, vr.y + vs * 0.5f - 30 * s, 24 * s, BAD);
        float es = 17 * s;
        while (es > 11 * s && Width(error_, es) > vs - 20 * s) es -= 1.0f;
        TextCentered(error_, vr.x + vs * 0.5f, vr.y + vs * 0.5f + 6 * s, es, TEXT);
    } else if (playing_.empty()) {
        TextCentered("Clip links anklicken", vr.x + vs * 0.5f, vr.y + vs * 0.5f - 9 * s, 18 * s, MUTED);
    }
    DrawRectangleLinesEx(vr, 1.0f, ACCENT_DIM);

    y = vr.y + vs + 12 * s;
    bool allOk = true;
    for (const Check& c : checks_) {
        Text(c.text, rx, y, fit(c.text, 17 * s), c.ok ? GOOD : BAD);
        y += kCheckH * s;
        allOk = allOk && c.ok;
    }
    if (!allOk && !checks_.empty()) {
        y += 6 * s;
        const char* cmd[2] = {"Passend umwandeln: ffmpeg -i eingabe -c:v mpeg1video -r 30 -s 480x480 -q:v 3",
                              "    -c:a mp2 -ar 32000 -ac 1 -b:a 128k -t 10 -f mpeg ausgabe.mpg"};
        for (const char* line : cmd) {
            Text(line, rx, y, fit(line, 15 * s), MUTED);
            y += 19 * s;
        }
    }
    return over;
}
#endif
