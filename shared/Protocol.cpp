// ---------------------------------------------------------------------------
// Protocol.cpp – Umsetzung des Netzwerkprotokolls: Nachrichten in Bytes
// verwandeln (Encode) und zurück (Decode). Die Nachrichten selbst und der
// Ablauf einer Sitzung sind in Protocol.hpp beschrieben.
//
// Wer benutzt diese Datei?
//   - server/src/GameServer.cpp: dekodiert, was Clients schicken, und kodiert
//     ServerInfo, Welcome, Snapshots, Ereignisse, Listen.
//   - src/NetClient.cpp (Spiel) und server/src/LoadTest.cpp (Lasttest): genau
//     umgekehrt.
//
// Weg einer Nachricht:
//   Encode(msg) -> std::vector<uint8_t> -> ENet-Paket -> Netz -> ENet-Paket
//   -> PeekType (erstes Byte = MsgType) -> Reader -> Decode(reader, msg)
//
// Drei Regeln, die überall gelten:
//   1. Encode und Decode einer Nachricht schreiben bzw. lesen dieselben
//      Felder in GENAU derselben Reihenfolge und Größe. Es gibt keine
//      Feldnamen im Datenstrom, nur die Reihenfolge. Wer ein Feld ergänzt,
//      ändert beide Funktionen und erhöht PROTOCOL_VERSION (Protocol.hpp):
//      Ein altes Spiel würde die neuen Bytes sonst falsch zuordnen.
//   2. Zahlen werden "Little Endian" geschrieben: das niedrigste Byte zuerst.
//      Das wird von Hand gemacht (Schieben und Maskieren) und ist deshalb auf
//      jedem Rechner gleich, egal wie dessen Prozessor Zahlen im Speicher ablegt.
//   3. Der Reader traut den Daten nicht. Jedes Lesen prüft, ob noch genug
//      Bytes da sind (Need); fehlt etwas, liefert er 0 und merkt sich den
//      Fehler (ok() == false). Längenangaben von Listen werden gegen MAX_LIST
//      geprüft, Aufzählungswerte gegen ihren Bereich. Decode liefert dann
//      false, und der Empfänger verwirft die ganze Nachricht.
//
// Quantisierung: Viele Werte werden nicht als float (4 Byte), sondern als
// kleinere ganze Zahl übertragen (z. B. Position in Zentimetern als int16).
// Das spart etwa die Hälfte der Bytes in jedem Snapshot. Die Faktoren stehen
// bei den Funktionen QPos, QAngle usw. und bei den Ereignissen unten;
// docs/Mehrspieler.md (Abschnitt 7) rechnet ein Beispiel durch.
//
// Abhängigkeiten: Protocol.hpp, Models.hpp (Modell-IDs für die Varianten),
// GameMap.hpp (Segmentliste für Karten).
// ---------------------------------------------------------------------------
#include "Protocol.hpp"
#include "Models.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace net {

// ===========================================================================
// Texte und Namen
// ===========================================================================
const char* ErrorText(ErrorCode c) {
    switch (c) {
        case ErrorCode::None: return "Kein Fehler";
        case ErrorCode::VersionMismatch: return "Server und Spiel haben unterschiedliche Versionen.";
        case ErrorCode::ServerFull: return "Der Server ist voll.";
        case ErrorCode::NameInvalid: return "Ungültiger Name (3-16 Zeichen: Buchstaben, Ziffern, _ und -).";
        case ErrorCode::NameTaken: return "Dieser Name ist schon vergeben.";
        case ErrorCode::WrongLogin: return "Name oder Passwort falsch.";
        case ErrorCode::AlreadyOnline: return "Dieser Spieler ist bereits angemeldet.";
        case ErrorCode::TooManyAttempts: return "Zu viele Versuche. Bitte kurz warten.";
        case ErrorCode::SpectatorsFull: return "Keine Zuschauerplätze mehr frei.";
        case ErrorCode::BadRequest: return "Ungültige Anfrage.";
        case ErrorCode::ServerShutdown: return "Der Server wird beendet.";
        case ErrorCode::Banned: return "Du bist auf diesem Server gesperrt.";
        case ErrorCode::Kicked: return "Der Betreiber hat dich vom Server getrennt.";
        case ErrorCode::RegistrationClosed: return "Dieser Server nimmt zurzeit keine neuen Konten an.";
    }
    return "Unbekannter Fehler";
}

bool ValidPlayerName(const std::string& name) {
    if (static_cast<int>(name.size()) < MIN_NAME || static_cast<int>(name.size()) > MAX_NAME) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool ChatChar(int cp) { return (cp >= 32 && cp <= 126) || (cp >= 160 && cp <= 255); }

// Der Text kommt von irgendeinem Client: UTF-8 von Hand dekodieren und dabei
// alles ablehnen, was die Schrift nicht kennt (Steuerzeichen, Emojis ...).
// Die erlaubten Zeichen brauchen 1 Byte (bis 127) oder 2 Byte (128..255).
bool ValidChatText(const std::string& text) {
    int count = 0;
    bool visible = false;
    for (size_t i = 0; i < text.size(); ++count) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        int cp = 0;
        if (b0 < 0x80) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            if ((b1 & 0xC0) != 0x80) return false;
            cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
            if (cp < 0x80) return false;   // unnötig lang kodiert
            i += 2;
        } else {
            return false;   // 3-/4-Byte-Zeichen oder abgeschnitten
        }
        if (!ChatChar(cp)) return false;
        if (cp != ' ') visible = true;
    }
    return visible && count <= MAX_CHAT;
}

// ===========================================================================
// Writer / Reader
// Writer hängt Bytes an buf an. Reader liest der Reihe nach (pos_) aus
// einem fremden Puffer und prüft vorher die Länge (Need).
// ===========================================================================
void Writer::U16(uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>(v >> 8));
}

void Writer::U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void Writer::I64(int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
}

void Writer::F32(float v) {
    // Die Bits der float-Zahl unverändert übernehmen (IEEE 754 auf allen Zielsystemen)
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float muss 32 Bit haben");
    std::memcpy(&bits, &v, sizeof(bits));
    U32(bits);
}

// Text: ein Längenbyte (0..255), danach die Bytes ohne abschließende 0.
// Längere Texte werden abgeschnitten; Namen und Meldungen sind viel kürzer.
void Writer::Str(const std::string& s) {
    const size_t n = std::min<size_t>(s.size(), 255);
    U8(static_cast<uint8_t>(n));
    buf.insert(buf.end(), s.begin(), s.begin() + static_cast<long>(n));
}

void Writer::Bytes(const uint8_t* data, size_t n) { buf.insert(buf.end(), data, data + n); }

// Sind noch n Bytes zu lesen? Wenn nicht: Fehler merken (ab dann liefert
// jedes weitere Lesen 0). "len_ - pos_ < n" statt "pos_ + n > len_", damit
// auch ein riesiges n keinen Überlauf der Rechnung erzeugt.
bool Reader::Need(size_t n) {
    if (!ok_ || len_ - pos_ < n) {
        ok_ = false;
        return false;
    }
    return true;
}

uint8_t Reader::U8() {
    if (!Need(1)) return 0;
    return data_[pos_++];
}

uint16_t Reader::U16() {
    if (!Need(2)) return 0;
    uint16_t v = static_cast<uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
    pos_ += 2;
    return v;
}

uint32_t Reader::U32() {
    if (!Need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_ + static_cast<size_t>(i)]) << (8 * i);
    pos_ += 4;
    return v;
}

int64_t Reader::I64() {
    if (!Need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data_[pos_ + static_cast<size_t>(i)]) << (8 * i);
    pos_ += 8;
    return static_cast<int64_t>(v);
}

float Reader::F32() {
    uint32_t bits = U32();
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    if (!std::isfinite(v)) return 0.0f;   // manipulierte Pakete könnten NaN enthalten
    return v;
}

std::string Reader::Str() {
    const uint8_t n = U8();
    if (!Need(n)) return std::string();
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
}

void Reader::Bytes(uint8_t* out, size_t n) {
    if (!Need(n)) {
        std::memset(out, 0, n);
        return;
    }
    std::memcpy(out, data_ + pos_, n);
    pos_ += n;
}

// ===========================================================================
// Quantisierung (Hin = Q..., Zurück = D...)
// ===========================================================================
namespace {
// Kommazahl -> ganze Zahl vom Typ T: erst auf [lo, hi] begrenzen, dann
// runden. Ohne das Begrenzen würde z. B. 400 in einem int8_t "überlaufen"
// und als völlig anderer Wert ankommen. NaN/unendlich werden zu 0.
template <typename T>
T ClampRound(float v, float lo, float hi) {
    if (!std::isfinite(v)) v = 0.0f;
    v = std::clamp(v, lo, hi);
    return static_cast<T>(std::lround(v));
}
} // namespace

// Position (x oder z) in Zentimetern als int16: Auflösung 1 cm, Bereich
// ±327,67 Einheiten. Die Karte reicht nur von -150 bis +150.
int16_t QPos(float v) { return ClampRound<int16_t>(v * 100.0f, -32767.0f, 32767.0f); }
float DPos(int16_t q) { return static_cast<float>(q) / 100.0f; }
// Geschwindigkeit in cm/s: Auflösung 0,01, Bereich ±327 Einheiten/s
// (Geschosse sind mit ~70/s die schnellsten Objekte).
int16_t QVel(float v) { return ClampRound<int16_t>(v * 100.0f, -32767.0f, 32767.0f); }
float DVel(int16_t q) { return static_cast<float>(q) / 100.0f; }

// Winkel: ein voller Kreis (2π) wird auf 65536 Stufen verteilt, also etwa
// 0,0055 Grad pro Stufe. Vorher in 0..2π bringen; "& 0xFFFF" macht aus
// genau 65536 (= 2π) wieder 0.
uint16_t QAngle(float rad) {
    if (!std::isfinite(rad)) rad = 0.0f;
    const float twoPi = 2.0f * PI;
    float a = std::fmod(rad, twoPi);
    if (a < 0) a += twoPi;
    long q = std::lround(a / twoPi * 65536.0f);
    return static_cast<uint16_t>(q & 0xFFFF);
}

// Zurück in -π..π (so rechnet die Spiellogik mit Winkeln).
float DAngle(uint16_t q) {
    float a = static_cast<float>(q) / 65536.0f * 2.0f * PI;
    if (a > PI) a -= 2.0f * PI;
    return a;
}

// Anteil 0..1 (Schild in Prozent, Schub-Anzeige, Treffer-Aufblitzen) in
// 256 Stufen: für Balken und Effekte mehr als genau genug.
uint8_t QUnit(float v) { return ClampRound<uint8_t>(v * 255.0f, 0.0f, 255.0f); }
float DUnit(uint8_t q) { return static_cast<float>(q) / 255.0f; }
// Höhe über dem Boden in 1/20 Einheiten (5 cm), Bereich 0..12,75. Schiffe
// fliegen auf 3,2; nur abstürzende sinken bis 0.
uint8_t QHeight(float y) { return ClampRound<uint8_t>(y * 20.0f, 0.0f, 255.0f); }
float DHeight(uint8_t q) { return static_cast<float>(q) / 20.0f; }

// ===========================================================================
// Hilfen für wiederkehrende Teile
// ===========================================================================
namespace {

constexpr size_t MAX_LIST = 4096;   // Schutz gegen unsinnige Längenangaben

// Jede Nachricht beginnt mit ihrem Typ-Byte
void Start(MsgType t, Writer& w) { w.U8(static_cast<uint8_t>(t)); }

void WriteSettings(Writer& w, const GameSettings& s) {
    w.Str(s.serverName);
    w.U8(static_cast<uint8_t>(s.difficulty));
    w.U8(static_cast<uint8_t>(s.enemyShips));
    w.U8(static_cast<uint8_t>(s.tradeStations));
    w.U8(static_cast<uint8_t>(s.shipyards));
    w.U8(static_cast<uint8_t>(s.moon));
    w.U8(static_cast<uint8_t>(s.maxPlayers));
    w.U8(static_cast<uint8_t>(s.lives));
    w.U8(static_cast<uint8_t>(s.missions));
    w.U8(static_cast<uint8_t>(std::min<size_t>(s.ships.size(), 255)));
    for (size_t i = 0; i < s.ships.size() && i < 255; ++i) {
        const ShipType& t = s.ships[i];
        w.Str(t.name);
        w.Str(t.model);
        w.U16(static_cast<uint16_t>(t.shield));
        w.U16(static_cast<uint16_t>(t.scanner));
        w.U16(static_cast<uint16_t>(t.speed));
        w.U16(static_cast<uint16_t>(t.weapon));
        w.U16(static_cast<uint16_t>(t.cargo));
        w.I32(t.cost);
        w.U8(static_cast<uint8_t>(t.guns));
        w.U8(t.forSale ? 1 : 0);
    }
}

void ReadSettings(Reader& r, GameSettings& s) {
    s.serverName = r.Str();
    s.difficulty = static_cast<Difficulty>(r.U8());
    s.enemyShips = r.U8();
    s.tradeStations = r.U8();
    s.shipyards = r.U8();
    s.moon = static_cast<MoonColor>(r.U8());
    s.maxPlayers = r.U8();
    s.lives = r.U8();
    s.missions = r.U8();
    const uint8_t ships = r.U8();
    s.ships.clear();
    for (uint8_t i = 0; i < ships && r.ok(); ++i) {
        ShipType t;
        t.name = r.Str();
        t.model = r.Str();
        t.shield = r.U16();
        t.scanner = r.U16();
        t.speed = r.U16();
        t.weapon = r.U16();
        t.cargo = r.U16();
        t.cost = r.I32();
        t.guns = r.U8();
        t.forSale = r.U8() != 0;
        s.ships.push_back(t);
    }
    Sanitize(s);   // ungültige Werte (z. B. von einem fremden Server) auf erlaubte Grenzen bringen
}

// Karte (GameMap ohne author): Name, Kartenart, Thema, Kantenlänge, Boden,
// Himmel (Version 10), Anzahl, je Teil 10 Bytes:
//   Segment-Nummer (Stelle in MapSegments(), dort nur hinten anfügen!),
//   x/z in cm (QPos), Drehung (QAngle), Größe in Tausendsteln, Höhe in dm.
void WriteMap(Writer& w, const GameMap& m) {
    w.Str(m.name);
    w.U8(static_cast<uint8_t>(m.kind));
    w.U8(static_cast<uint8_t>(m.theme));
    w.U16(static_cast<uint16_t>(std::clamp(m.size, mapgrid::MISSION_SIZE_MIN, mapgrid::MISSION_SIZE_MAX)));
    w.U8(static_cast<uint8_t>(m.ground));
    w.U8(static_cast<uint8_t>(m.sky));
    const size_t n = std::min<size_t>(m.pieces.size(), mapgrid::MAX_PIECES);
    w.U16(static_cast<uint16_t>(n));
    const std::vector<MapSegment>& segs = MapSegments();
    for (size_t i = 0; i < n; ++i) {
        const MapPiece& p = m.pieces[i];
        size_t seg = 0;
        while (seg < segs.size() && p.model != segs[seg].model) ++seg;
        w.U8(static_cast<uint8_t>(seg));   // unbekannt = segs.size(): der Empfänger verwirft die Karte
        w.I16(QPos(p.x));
        w.I16(QPos(p.z));
        w.U16(QAngle(p.yaw));
        w.U16(ClampRound<uint16_t>(p.scale * 1000.0f, 0.0f, 65535.0f));
        w.U8(ClampRound<uint8_t>(p.lift * 10.0f, 0.0f, 255.0f));
    }
}

// Gleiche Grenzen wie MapFromText: bekanntes Segment, innerhalb der Karte,
// Größe 0,2..20, Höhe bis MAX_LEVEL Ebenen, bekannte Kopfwerte. Sonst ist
// die ganze Nachricht ungültig.
bool ReadMap(Reader& r, GameMap& m) {
    m = GameMap{};
    m.name = r.Str();
    for (char& ch : m.name)   // keine Zeilenumbrüche: der Name steht in der Kartendatei auf einer Zeile
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
    const uint8_t kind = r.U8(), theme = r.U8();
    const uint16_t size = r.U16();
    const uint8_t ground = r.U8(), sky = r.U8();
    if (kind > static_cast<uint8_t>(MapKind::Mission) || theme >= static_cast<uint8_t>(MapTheme::Count) ||
        size < mapgrid::MISSION_SIZE_MIN || size > mapgrid::MISSION_SIZE_MAX ||
        ground >= static_cast<uint8_t>(MapGround::Count) || sky >= static_cast<uint8_t>(MapSky::Count))
        return false;
    m.kind = static_cast<MapKind>(kind);
    m.theme = static_cast<MapTheme>(theme);
    m.size = size;
    m.ground = static_cast<MapGround>(ground);
    m.sky = static_cast<MapSky>(sky);
    const float half = MapHalf(m);
    const uint16_t n = r.U16();
    if (n > mapgrid::MAX_PIECES) return false;
    const std::vector<MapSegment>& segs = MapSegments();
    m.pieces.reserve(n);
    for (uint16_t i = 0; i < n && r.ok(); ++i) {
        MapPiece p;
        const uint8_t seg = r.U8();
        if (seg >= segs.size()) return false;
        p.model = segs[seg].model;
        p.x = DPos(r.I16());
        p.z = DPos(r.I16());
        p.yaw = DAngle(r.U16());
        p.scale = static_cast<float>(r.U16()) / 1000.0f;
        p.lift = static_cast<float>(r.U8()) / 10.0f;
        if (fabsf(p.x) > half || fabsf(p.z) > half || p.scale < 0.2f || p.scale > 20.0f ||
            p.lift > mapgrid::MAX_LEVEL * mapgrid::LEVEL + 0.01f)
            return false;
        m.pieces.push_back(p);
    }
    return r.ok();
}

void WriteColor(Writer& w, Color c) {
    w.U8(c.r);
    w.U8(c.g);
    w.U8(c.b);
}

Color ReadColor(Reader& r) {
    Color c;
    c.r = r.U8();
    c.g = r.U8();
    c.b = r.U8();
    c.a = 255;
    return c;
}

uint16_t ClampU16(int64_t v) { return static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 65535)); }

} // namespace

// ===========================================================================
// Client -> Server
// ===========================================================================
std::vector<uint8_t> EncodeSimple(MsgType t) {
    Writer w;
    w.U8(static_cast<uint8_t>(t));
    return w.buf;
}

std::vector<uint8_t> Encode(const RegisterMsg& m) {
    Writer w;
    Start(MsgType::Register, w);
    w.Str(m.name);
    w.Bytes(m.key.data(), m.key.size());
    return w.buf;
}

bool Decode(Reader& r, RegisterMsg& m) {
    m.name = r.Str();
    r.Bytes(m.key.data(), m.key.size());
    return r.ok();
}

std::vector<uint8_t> Encode(const LoginStartMsg& m) {
    Writer w;
    Start(MsgType::LoginStart, w);
    w.Str(m.name);
    return w.buf;
}

bool Decode(Reader& r, LoginStartMsg& m) {
    m.name = r.Str();
    return r.ok();
}

std::vector<uint8_t> Encode(const LoginProofMsg& m) {
    Writer w;
    Start(MsgType::LoginProof, w);
    w.Bytes(m.proof.data(), m.proof.size());
    return w.buf;
}

bool Decode(Reader& r, LoginProofMsg& m) {
    r.Bytes(m.proof.data(), m.proof.size());
    return r.ok();
}

std::vector<uint8_t> Encode(const InputMsg& m) {
    Writer w;
    Start(MsgType::Input, w);
    const size_t n = std::min<size_t>(m.records.size(), MAX_INPUTS_PER_MSG);
    const size_t first = m.records.size() - n;   // die neuesten n Einträge
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = first; i < m.records.size(); ++i) {
        const InputRecord& in = m.records[i];
        w.U32(in.seq);
        // Schub und Drehung -1..1 in Prozent (int8 -100..100). Der Client
        // rechnet seine Vorhersage mit genau diesen gerundeten Werten
        // (NetClient::StepOwnShip), damit Server und Client gleich rechnen.
        w.I8(ClampRound<int8_t>(in.thrust * 100.0f, -100.0f, 100.0f));
        w.I8(ClampRound<int8_t>(in.turn * 100.0f, -100.0f, 100.0f));
        w.U8(in.fire ? 1 : 0);
        w.U8(in.scanPresses);
        w.U8(in.interactPresses);
    }
    return w.buf;
}

bool Decode(Reader& r, InputMsg& m) {
    const uint8_t n = r.U8();
    if (n > MAX_INPUTS_PER_MSG) return false;
    m.records.clear();
    for (uint8_t i = 0; i < n; ++i) {
        InputRecord in;
        in.seq = r.U32();
        in.thrust = std::clamp(static_cast<float>(r.I8()) / 100.0f, -1.0f, 1.0f);
        in.turn = std::clamp(static_cast<float>(r.I8()) / 100.0f, -1.0f, 1.0f);
        in.fire = (r.U8() & 1) != 0;
        in.scanPresses = r.U8();
        in.interactPresses = r.U8();
        m.records.push_back(in);
    }
    return r.ok();
}

std::vector<uint8_t> Encode(const DockActionMsg& m) {
    Writer w;
    Start(MsgType::DockAction, w);
    w.U8(static_cast<uint8_t>(m.action));
    w.U8(m.param);
    return w.buf;
}

bool Decode(Reader& r, DockActionMsg& m) {
    const uint8_t a = r.U8();
    if (a > static_cast<uint8_t>(DockActionType::AcceptMission)) return false;
    m.action = static_cast<DockActionType>(a);
    m.param = r.U8();
    return r.ok();
}

std::vector<uint8_t> Encode(const MapUploadMsg& m) {
    Writer w;
    Start(MsgType::MapUpload, w);
    w.Bytes(m.proof.data(), m.proof.size());
    WriteMap(w, m.map);
    return w.buf;
}

bool Decode(Reader& r, MapUploadMsg& m) {
    r.Bytes(m.proof.data(), m.proof.size());
    return ReadMap(r, m.map);
}

std::vector<uint8_t> Encode(const ChatMsg& m) {
    Writer w;
    Start(MsgType::Chat, w);
    w.Str(m.text);
    return w.buf;
}

bool Decode(Reader& r, ChatMsg& m) {
    m.text = r.Str();
    return r.ok();
}

std::vector<uint8_t> Encode(const ChatTypingMsg& m) {
    Writer w;
    Start(MsgType::ChatTyping, w);
    w.U8(m.typing ? 1 : 0);
    return w.buf;
}

bool Decode(Reader& r, ChatTypingMsg& m) {
    m.typing = r.U8() != 0;
    return r.ok();
}

// ===========================================================================
// Server -> Client
// ===========================================================================
std::vector<uint8_t> Encode(const ServerInfoMsg& m) {
    Writer w;
    Start(MsgType::ServerInfo, w);
    w.U16(m.version);
    w.Str(m.serverName);
    w.U8(m.playersOnline);
    w.U8(m.maxPlayers);
    w.U8(m.spectators);
    w.U8(m.maxSpectators);
    WriteSettings(w, m.settings);
    w.Bytes(m.realmSalt.data(), m.realmSalt.size());
    w.I64(m.totalDelivered);
    return w.buf;
}

bool Decode(Reader& r, ServerInfoMsg& m) {
    m.version = r.U16();
    m.serverName = r.Str();
    m.playersOnline = r.U8();
    m.maxPlayers = r.U8();
    m.spectators = r.U8();
    m.maxSpectators = r.U8();
    ReadSettings(r, m.settings);
    r.Bytes(m.realmSalt.data(), m.realmSalt.size());
    m.totalDelivered = r.I64();
    return r.ok();
}

std::vector<uint8_t> Encode(const ErrorMsg& m) {
    Writer w;
    Start(MsgType::Error, w);
    w.U8(static_cast<uint8_t>(m.code));
    w.Str(m.text);
    return w.buf;
}

bool Decode(Reader& r, ErrorMsg& m) {
    m.code = static_cast<ErrorCode>(r.U8());
    m.text = r.Str();
    return r.ok();
}

std::vector<uint8_t> Encode(const LoginChallengeMsg& m) {
    Writer w;
    Start(MsgType::LoginChallenge, w);
    w.Bytes(m.nonce.data(), m.nonce.size());
    return w.buf;
}

bool Decode(Reader& r, LoginChallengeMsg& m) {
    r.Bytes(m.nonce.data(), m.nonce.size());
    return r.ok();
}

std::vector<uint8_t> Encode(const WelcomeMsg& m) {
    Writer w;
    Start(MsgType::Welcome, w);
    w.U8(static_cast<uint8_t>(m.role));
    w.I32(m.playerId);
    w.U32(m.worldSeed);
    WriteSettings(w, m.settings);
    w.I64(m.totalDelivered);
    w.F32(m.serverTime);
    w.U32(m.tick);
    return w.buf;
}

bool Decode(Reader& r, WelcomeMsg& m) {
    const uint8_t role = r.U8();
    if (role != static_cast<uint8_t>(Role::Spectator) && role != static_cast<uint8_t>(Role::Player)) return false;
    m.role = static_cast<Role>(role);
    m.playerId = r.I32();
    m.worldSeed = r.U32();
    ReadSettings(r, m.settings);
    m.totalDelivered = r.I64();
    m.serverTime = r.F32();
    m.tick = r.U32();
    return r.ok();
}

std::vector<uint8_t> Encode(const SnapshotMsg& m) {
    Writer w;
    Start(MsgType::Snapshot, w);
    w.U32(m.tick);
    w.F32(m.serverTime);
    w.I64(m.totalDelivered);
    w.U8(m.stationStage);
    w.U8(m.hasMe ? 1 : 0);
    if (m.hasMe) {
        const SnapMe& e = m.me;
        w.I32(e.shipId);
        w.U32(e.ackSeq);
        // Für die Vorhersage genaue Werte (float), keine Quantisierung
        w.F32(e.x);
        w.F32(e.y);
        w.F32(e.z);
        w.F32(e.vx);
        w.F32(e.vz);
        w.F32(e.yaw);
        w.F32(e.bank);
        w.F32(e.hp);
        w.F32(e.maxHp);
        w.U8(QUnit(e.hitFlash));
        w.U8(QUnit(e.thrustVisual));
        // Mehrere Ja/Nein-Werte in EINEM Byte ("Bitfeld"): Bit 0 (Wert 1) =
        // stürzt ab, Bit 1 (2) = Scanner an, Bit 2 (4) = baut ab. Decode
        // prüft jedes Bit mit "flags & Wert".
        uint8_t flags = 0;
        if (e.crashing) flags |= 1;
        if (e.scanOn) flags |= 2;
        if (e.mining) flags |= 4;
        w.U8(flags);
        w.I32(e.cargo);
        w.I32(e.cargoMax);
        w.I64(e.balance);
        for (uint8_t u : e.upgrades) w.U8(u);
        w.U8(e.shipType);
        const size_t nh = std::min<size_t>(e.hangar.size(), MAX_HANGAR);
        w.U8(static_cast<uint8_t>(nh));
        for (size_t i = 0; i < nh; ++i) {
            w.U8(e.hangar[i].type);
            for (uint8_t u : e.hangar[i].upgrades) w.U8(u);
        }
        w.F32(e.scanEnergy);
        w.F32(e.scanPhase);
        w.U8(e.mineKind);
        w.I32(e.mineId);
        w.F32(e.mineT);
        w.I8(e.dockedAt);
        w.I32(e.lastSale);
        w.F32(e.shipTime);
        w.F32(e.roundTime);
        w.U16(e.round);
        w.U16(e.kills);
        w.F32(e.respawnT);
        w.U8(e.lives);
        // Mission: Typ, und nur bei laufender Mission Fortschritt/Ziel/Frachter
        w.I8(e.missionType);
        if (e.missionType >= 0) {
            w.U8(e.missionProgress);
            w.U8(e.missionGoal);
            w.U8(e.missionAlive);
        }
        const size_t no = std::min<size_t>(e.offers.size(), limits::MAX_MISSIONS);
        w.U8(static_cast<uint8_t>(no));
        for (size_t i = 0; i < no; ++i) w.U8(e.offers[i]);
        const size_t nm = std::min<size_t>(e.markers.size(), MAX_MARKERS);
        w.U8(static_cast<uint8_t>(nm));
        for (size_t i = 0; i < nm; ++i) {
            w.U8(e.markers[i].kind);
            w.I16(QPos(e.markers[i].x));
            w.I16(QPos(e.markers[i].z));
        }
        // Funkspruch: Restzeit in Zehntelsekunden (höchstens 25,5 s)
        w.I8(e.cue);
        w.U8(static_cast<uint8_t>(std::clamp(e.cueT * 10.0f, 0.0f, 255.0f)));
    }
    w.U16(static_cast<uint16_t>(std::min<size_t>(m.ships.size(), 65535)));
    for (size_t i = 0; i < m.ships.size() && i < 65535; ++i) {
        const SnapShip& s = m.ships[i];
        w.U32(s.id);
        w.U8(s.kind);
        w.U8(s.variant);
        w.I16(QPos(s.x));
        w.U8(QHeight(s.y));
        w.I16(QPos(s.z));
        w.U16(QAngle(s.yaw));
        w.U16(QAngle(s.bank));
        w.U8(QUnit(s.hpFrac));
        w.U8(QUnit(s.thrustVisual));
        w.U8(QUnit(s.hitFlash));
        // Bitfeld wie oben: 1 = stürzt ab, 2 = feindlich, 4 = baut ab,
        // 8 = angedockt, 16 = tippt im Chat
        uint8_t flags = 0;
        if (s.crashing) flags |= 1;
        if (s.hostile) flags |= 2;
        if (s.mining) flags |= 4;
        if (s.docked) flags |= 8;
        if (s.chatting) flags |= 16;
        w.U8(flags);
        // Das Abbauziel nur, wenn das Schiff gerade abbaut (spart 4 Byte je
        // Schiff). Decode liest es nur dann, wenn Bit 4 gesetzt ist.
        if (s.mining) {
            w.I16(QPos(s.mineX));
            w.I16(QPos(s.mineZ));
        }
        w.I16(s.playerId);
    }
    w.U16(static_cast<uint16_t>(std::min<size_t>(m.bullets.size(), 65535)));
    for (size_t i = 0; i < m.bullets.size() && i < 65535; ++i) {
        const SnapBullet& b = m.bullets[i];
        w.I16(QPos(b.x));
        w.U8(QHeight(b.y));
        w.I16(QPos(b.z));
        w.I16(QVel(b.vx));
        w.I16(QVel(b.vz));
        w.U8(b.type);
    }
    w.U16(static_cast<uint16_t>(std::min<size_t>(m.wrecks.size(), 65535)));
    for (size_t i = 0; i < m.wrecks.size() && i < 65535; ++i) {
        const SnapWreck& k = m.wrecks[i];
        w.U32(k.id);
        w.I16(QPos(k.x));
        w.I16(QPos(k.z));
        w.U16(QAngle(k.yaw));
        w.U16(QAngle(k.tilt));
        w.U8(k.variant);
        w.U16(k.amount);
        w.U16(k.initial);
        // Alter des Wracks in ganzen Sekunden (verschwindet nach 300 s; für
        // die Darstellung genügt diese Genauigkeit)
        w.U16(ClampU16(static_cast<int64_t>(k.age)));
    }
    return w.buf;
}

bool Decode(Reader& r, SnapshotMsg& m) {
    m.tick = r.U32();
    m.serverTime = r.F32();
    m.totalDelivered = r.I64();
    m.stationStage = r.U8();
    m.hasMe = r.U8() != 0;
    if (m.hasMe) {
        SnapMe& e = m.me;
        e.shipId = r.I32();
        e.ackSeq = r.U32();
        e.x = r.F32();
        e.y = r.F32();
        e.z = r.F32();
        e.vx = r.F32();
        e.vz = r.F32();
        e.yaw = r.F32();
        e.bank = r.F32();
        e.hp = r.F32();
        e.maxHp = r.F32();
        e.hitFlash = DUnit(r.U8());
        e.thrustVisual = DUnit(r.U8());
        const uint8_t flags = r.U8();
        e.crashing = (flags & 1) != 0;
        e.scanOn = (flags & 2) != 0;
        e.mining = (flags & 4) != 0;
        e.cargo = r.I32();
        e.cargoMax = r.I32();
        e.balance = r.I64();
        for (uint8_t& u : e.upgrades) u = r.U8();
        e.shipType = r.U8();
        const uint8_t nh = r.U8();
        if (nh > MAX_HANGAR) return false;
        e.hangar.assign(nh, SnapMe::Parked{});
        for (auto& h : e.hangar) {
            h.type = r.U8();
            for (uint8_t& u : h.upgrades) u = r.U8();
        }
        e.scanEnergy = r.F32();
        e.scanPhase = r.F32();
        e.mineKind = r.U8();
        e.mineId = r.I32();
        e.mineT = r.F32();
        e.dockedAt = r.I8();
        e.lastSale = r.I32();
        e.shipTime = r.F32();
        e.roundTime = r.F32();
        e.round = r.U16();
        e.kills = r.U16();
        e.respawnT = r.F32();
        e.lives = r.U8();
        e.missionType = r.I8();
        if (e.missionType >= 0) {
            e.missionProgress = r.U8();
            e.missionGoal = r.U8();
            e.missionAlive = r.U8();
        }
        const uint8_t no = r.U8();
        if (no > limits::MAX_MISSIONS) return false;
        e.offers.assign(no, 0);
        for (uint8_t& o : e.offers) o = r.U8();
        const uint8_t nm = r.U8();
        if (nm > MAX_MARKERS) return false;
        e.markers.assign(nm, SnapMe::Marker{});
        for (auto& mk : e.markers) {
            mk.kind = r.U8();
            mk.x = DPos(r.I16());
            mk.z = DPos(r.I16());
        }
        e.cue = r.I8();
        e.cueT = r.U8() / 10.0f;
    }
    const uint16_t ns = r.U16();
    if (ns > MAX_LIST) return false;
    m.ships.clear();
    m.ships.reserve(ns);
    for (uint16_t i = 0; i < ns && r.ok(); ++i) {
        SnapShip s;
        s.id = r.U32();
        s.kind = r.U8();
        s.variant = r.U8();
        s.x = DPos(r.I16());
        s.y = DHeight(r.U8());
        s.z = DPos(r.I16());
        s.yaw = DAngle(r.U16());
        s.bank = DAngle(r.U16());
        s.hpFrac = DUnit(r.U8());
        s.thrustVisual = DUnit(r.U8());
        s.hitFlash = DUnit(r.U8());
        const uint8_t flags = r.U8();
        s.crashing = (flags & 1) != 0;
        s.hostile = (flags & 2) != 0;
        s.mining = (flags & 4) != 0;
        s.docked = (flags & 8) != 0;
        s.chatting = (flags & 16) != 0;
        if (s.mining) {
            s.mineX = DPos(r.I16());
            s.mineZ = DPos(r.I16());
        }
        s.playerId = r.I16();
        m.ships.push_back(s);
    }
    const uint16_t nb = r.U16();
    if (nb > MAX_LIST) return false;
    m.bullets.clear();
    m.bullets.reserve(nb);
    for (uint16_t i = 0; i < nb && r.ok(); ++i) {
        SnapBullet b;
        b.x = DPos(r.I16());
        b.y = DHeight(r.U8());
        b.z = DPos(r.I16());
        b.vx = DVel(r.I16());
        b.vz = DVel(r.I16());
        b.type = r.U8();
        m.bullets.push_back(b);
    }
    const uint16_t nw = r.U16();
    if (nw > MAX_LIST) return false;
    m.wrecks.clear();
    m.wrecks.reserve(nw);
    for (uint16_t i = 0; i < nw && r.ok(); ++i) {
        SnapWreck k;
        k.id = r.U32();
        k.x = DPos(r.I16());
        k.z = DPos(r.I16());
        k.yaw = DAngle(r.U16());
        k.tilt = DAngle(r.U16());
        k.variant = r.U8();
        k.amount = r.U16();
        k.initial = r.U16();
        k.age = static_cast<float>(r.U16());
        m.wrecks.push_back(k);
    }
    return r.ok();
}

std::vector<uint8_t> Encode(const DepositsMsg& m) {
    Writer w;
    Start(MsgType::Deposits, w);
    w.U16(static_cast<uint16_t>(std::min<size_t>(m.deposits.size(), 65535)));
    for (size_t i = 0; i < m.deposits.size() && i < 65535; ++i) {
        const NetDeposit& d = m.deposits[i];
        w.U32(d.id);
        w.I16(QPos(d.x));
        w.I16(QPos(d.z));
        w.U16(QAngle(d.yaw));
        // Größe der Lagerstätte in 1/20-Schritten (0,05), höchstens 12,75
        w.U8(ClampRound<uint8_t>(d.scale * 20.0f, 0.0f, 255.0f));
        w.U8(d.variant);
        w.U16(d.amount);
        w.U16(d.initial);
    }
    return w.buf;
}

bool Decode(Reader& r, DepositsMsg& m) {
    const uint16_t n = r.U16();
    if (n > MAX_LIST) return false;
    m.deposits.clear();
    m.deposits.reserve(n);
    for (uint16_t i = 0; i < n && r.ok(); ++i) {
        NetDeposit d;
        d.id = r.U32();
        d.x = DPos(r.I16());
        d.z = DPos(r.I16());
        d.yaw = DAngle(r.U16());
        d.scale = static_cast<float>(r.U8()) / 20.0f;
        d.variant = r.U8();
        d.amount = r.U16();
        d.initial = r.U16();
        m.deposits.push_back(d);
    }
    return r.ok();
}

std::vector<uint8_t> Encode(const EventsMsg& m) {
    Writer w;
    Start(MsgType::Events, w);
    w.U16(static_cast<uint16_t>(std::min<size_t>(m.events.size(), 65535)));
    for (size_t i = 0; i < m.events.size() && i < 65535; ++i) {
        const NetEvent& e = m.events[i];
        w.U8(static_cast<uint8_t>(e.kind));
        switch (e.kind) {
            case EventKind::Sound:
                w.U8(static_cast<uint8_t>(e.sfx));
                w.I16(QPos(e.x));
                w.I16(QPos(e.z));
                w.U8(QUnit(std::min(1.0f, e.volume)));
                // Verzögerung in 1/50 s (20 ms), höchstens 255/50 = 5,1 s.
                // Die längste im Spiel ist die Siegesfanfare mit 1,3 s.
                w.U8(ClampRound<uint8_t>(e.delay * 50.0f, 0.0f, 255.0f));
                w.I32(e.sourceShip);
                break;
            case EventKind::Explosion:
                w.I16(QPos(e.x));
                w.U8(QHeight(e.y));
                w.I16(QPos(e.z));
                // Explosionsgröße in 1/50-Schritten, höchstens 5,1 (größte: 2,0)
                w.U8(ClampRound<uint8_t>(e.size * 50.0f, 0.0f, 255.0f));
                break;
            case EventKind::Sparks:
                w.I16(QPos(e.x));
                w.U8(QHeight(e.y));
                w.I16(QPos(e.z));
                WriteColor(w, e.color);
                w.U8(e.count);
                break;
            case EventKind::Notice:
                w.Str(e.text);
                WriteColor(w, e.color);
                break;
            case EventKind::Goal:
                w.I32(e.playerId);
                w.Str(e.text);
                w.F32(e.seconds);
                w.U16(e.round);
                break;
            case EventKind::StationGrow:
                w.U8(e.stage);
                break;
            case EventKind::Respawned:
                break;
        }
    }
    return w.buf;
}

bool Decode(Reader& r, EventsMsg& m) {
    const uint16_t n = r.U16();
    if (n > MAX_LIST) return false;
    m.events.clear();
    m.events.reserve(n);
    for (uint16_t i = 0; i < n && r.ok(); ++i) {
        NetEvent e;
        const uint8_t kind = r.U8();
        if (kind > static_cast<uint8_t>(EventKind::Respawned)) return false;
        e.kind = static_cast<EventKind>(kind);
        switch (e.kind) {
            case EventKind::Sound: {
                const uint8_t sfx = r.U8();
                if (sfx >= static_cast<uint8_t>(Sfx::Count)) return false;
                e.sfx = static_cast<Sfx>(sfx);
                e.x = DPos(r.I16());
                e.z = DPos(r.I16());
                e.volume = DUnit(r.U8());
                e.delay = static_cast<float>(r.U8()) / 50.0f;
                e.sourceShip = r.I32();
                break;
            }
            case EventKind::Explosion:
                e.x = DPos(r.I16());
                e.y = DHeight(r.U8());
                e.z = DPos(r.I16());
                e.size = static_cast<float>(r.U8()) / 50.0f;
                break;
            case EventKind::Sparks:
                e.x = DPos(r.I16());
                e.y = DHeight(r.U8());
                e.z = DPos(r.I16());
                e.color = ReadColor(r);
                e.count = r.U8();
                break;
            case EventKind::Notice:
                e.text = r.Str();
                e.color = ReadColor(r);
                break;
            case EventKind::Goal:
                e.playerId = r.I32();
                e.text = r.Str();
                e.seconds = r.F32();
                e.round = r.U16();
                break;
            case EventKind::StationGrow:
                e.stage = r.U8();
                break;
            case EventKind::Respawned:
                break;
        }
        m.events.push_back(e);
    }
    return r.ok();
}

std::vector<uint8_t> Encode(const PlayersMsg& m) {
    Writer w;
    Start(MsgType::Players, w);
    w.I64(m.totalDelivered);
    w.U8(m.spectators);
    w.U8(m.maxPlayers);
    w.U16(static_cast<uint16_t>(std::min<size_t>(m.players.size(), 65535)));
    for (size_t i = 0; i < m.players.size() && i < 65535; ++i) {
        const PlayerEntry& p = m.players[i];
        w.I32(p.id);
        w.Str(p.name);
        w.I32(p.shipId);
        w.I64(p.balance);
        w.U16(p.round);
        w.F32(p.shipTime);
        w.F32(p.roundTime);
        for (uint8_t u : p.upgrades) w.U8(u);
        // Bitfeld: 1 = hat ein Schiff (nicht abgestürzt), 2 = angedockt
        uint8_t flags = 0;
        if (p.alive) flags |= 1;
        if (p.docked) flags |= 2;
        w.U8(flags);
        w.U16(p.kills);
        w.U16(p.goals);
        w.U8(p.lives);
    }
    return w.buf;
}

bool Decode(Reader& r, PlayersMsg& m) {
    m.totalDelivered = r.I64();
    m.spectators = r.U8();
    m.maxPlayers = r.U8();
    const uint16_t n = r.U16();
    if (n > MAX_LIST) return false;
    m.players.clear();
    m.players.reserve(n);
    for (uint16_t i = 0; i < n && r.ok(); ++i) {
        PlayerEntry p;
        p.id = r.I32();
        p.name = r.Str();
        p.shipId = r.I32();
        p.balance = r.I64();
        p.round = r.U16();
        p.shipTime = r.F32();
        p.roundTime = r.F32();
        for (uint8_t& u : p.upgrades) u = r.U8();
        const uint8_t flags = r.U8();
        p.alive = (flags & 1) != 0;
        p.docked = (flags & 2) != 0;
        p.kills = r.U16();
        p.goals = r.U16();
        p.lives = r.U8();
        m.players.push_back(p);
    }
    return r.ok();
}

std::vector<uint8_t> Encode(const HighscoresMsg& m) {
    Writer w;
    Start(MsgType::Highscores, w);
    w.U16(static_cast<uint16_t>(std::min<size_t>(m.entries.size(), 65535)));
    for (size_t i = 0; i < m.entries.size() && i < 65535; ++i) {
        const HighscoreEntry& h = m.entries[i];
        w.Str(h.name);
        w.F32(h.seconds);
        w.U16(h.round);
        w.I64(h.when);
    }
    return w.buf;
}

bool Decode(Reader& r, HighscoresMsg& m) {
    const uint16_t n = r.U16();
    if (n > MAX_LIST) return false;
    m.entries.clear();
    m.entries.reserve(n);
    for (uint16_t i = 0; i < n && r.ok(); ++i) {
        HighscoreEntry h;
        h.name = r.Str();
        h.seconds = r.F32();
        h.round = r.U16();
        h.when = r.I64();
        m.entries.push_back(h);
    }
    return r.ok();
}

std::vector<uint8_t> Encode(const SettingsMsg& m) {
    Writer w;
    Start(MsgType::Settings, w);
    WriteSettings(w, m.settings);
    return w.buf;
}

bool Decode(Reader& r, SettingsMsg& m) {
    ReadSettings(r, m.settings);
    return r.ok();
}

std::vector<uint8_t> Encode(const MapUploadResultMsg& m) {
    Writer w;
    Start(MsgType::MapUploadResult, w);
    w.U8(m.ok ? 1 : 0);
    w.Str(m.text);
    return w.buf;
}

bool Decode(Reader& r, MapUploadResultMsg& m) {
    m.ok = r.U8() != 0;
    m.text = r.Str();
    return r.ok();
}

std::vector<uint8_t> Encode(const MapDataMsg& m) {
    Writer w;
    Start(MsgType::MapData, w);
    w.U8(m.custom ? 1 : 0);
    WriteMap(w, m.map);
    return w.buf;
}

bool Decode(Reader& r, MapDataMsg& m) {
    m.custom = r.U8() != 0;
    return ReadMap(r, m.map);
}

std::vector<uint8_t> Encode(const ChatLineMsg& m) {
    Writer w;
    Start(MsgType::ChatLine, w);
    w.I32(m.playerId);
    w.Str(m.name);
    w.Str(m.text);
    return w.buf;
}

bool Decode(Reader& r, ChatLineMsg& m) {
    m.playerId = r.I32();
    m.name = r.Str();
    m.text = r.Str();
    return r.ok();
}

// ===========================================================================
// Modellvarianten
// Statt einer Modell-ID (die je Programm anders vergeben sein könnte, siehe
// Models.hpp) geht nur eine kleine Nummer übers Netz: die Stelle in einer
// festen Liste. Unbekannte Nummern werden zur ersten Variante.
// ===========================================================================
ModelIds DefaultModelIds() {
    ModelIds m;
    m.player = ModelId("craft_speederA");
    m.pirateA = ModelId("craft_speederB");
    m.pirateB = ModelId("craft_speederC");
    m.elite = ModelId("craft_speederD");
    m.miner = ModelId("craft_miner");
    m.cargoA = ModelId("craft_cargoA");
    m.cargoB = ModelId("craft_cargoB");
    m.crystalSmall = ModelId("rock_crystals");
    m.crystalA = ModelId("rock_crystalsLargeA");
    m.crystalB = ModelId("rock_crystalsLargeB");
    m.ships = {m.player, m.pirateA, m.pirateB, m.elite, m.miner, m.cargoA, m.cargoB};
    for (const std::string& name : ShipModelNames()) {
        const int id = ModelId(name);
        if (std::find(m.ships.begin(), m.ships.end(), id) == m.ships.end()) m.ships.push_back(id);
    }
    return m;
}

uint8_t ShipVariant(const ModelIds& m, int model) {
    for (size_t i = 0; i < m.ships.size(); ++i)
        if (m.ships[i] == model) return static_cast<uint8_t>(i);
    return 0;
}

int ShipModel(const ModelIds& m, uint8_t variant) {
    return variant < m.ships.size() ? m.ships[variant] : m.player;
}

uint8_t DepositVariant(const ModelIds& m, int model) {
    if (model == m.crystalA) return 1;
    if (model == m.crystalB) return 2;
    return 0;
}

int DepositModel(const ModelIds& m, uint8_t variant) {
    return variant == 1 ? m.crystalA : (variant == 2 ? m.crystalB : m.crystalSmall);
}

} // namespace net
