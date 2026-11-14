#pragma once
// ---------------------------------------------------------------------------
// AssetPack.hpp – Asset-Paket "assets.pak": alle Modelle, Bilder, Töne,
// Clips und Einsatzkarten in einer einzigen verschlüsselten Datei
// (Umsetzung: AssetPack.cpp).
//
// Wofür ist die Datei da?
//   Neben der ausführbaren Datei soll kein Asset mehr als Einzeldatei liegen
//   (wie bei anderen Spielen). Das Werkzeug tools/PackAssets.cpp packt beim
//   Build den Asset-Ordner in assets.pak; das Spiel liest daraus und
//   entschlüsselt jede Datei erst im Arbeitsspeicher (src/Assets.cpp).
//
// Grenzen (bewusst):
//   Der Schlüssel steckt im Spiel, sonst könnte es die Dateien nicht lesen.
//   Wer die ausführbare Datei mit einem Debugger untersucht oder Modelle und
//   Texturen von der Grafikkarte mitschneidet, kommt an die Assets. Das
//   Paket schützt gegen Durchklicken, Entpackprogramme und Hex-Editoren; der
//   eigentliche Schutz ist die Lizenz der Assets. Weil der Quelltext
//   öffentlich ist, steht der Schlüssel NICHT im Quelltext: CMake liest ihn
//   aus der Schlüsseldatei (assets.key, steht in .gitignore) und schreibt ihn
//   verwürfelt in build/generated/PakKey.inc.
//
// Aufbau der Datei (Zahlen little-endian):
//   Kopf, 64 Byte, unverschlüsselt:
//     Byte  0..3   "MSPK" (Kennung)
//     Byte  4..7   Version (1)
//     Byte  8..23  Salz (16 Zufallsbytes, bei jedem Packen neu)
//     Byte 24..27  Anzahl Dateien
//     Byte 28..31  Länge des Inhaltsverzeichnisses in Byte
//     Byte 32..63  HMAC-SHA256 über Byte 0..31 und das verschlüsselte
//                  Inhaltsverzeichnis (falscher Schlüssel oder veränderte
//                  Datei fallen sofort auf)
//   Inhaltsverzeichnis, verschlüsselt (auch die Dateinamen sind nicht
//   lesbar), je Datei:
//     2 Byte Länge des Pfads, Pfad (relativ zum Asset-Ordner, "/" als
//     Trenner, z. B. "models/craft_speederA.glb"), 8 Byte Lage in der Datei,
//     8 Byte Länge, 32 Byte HMAC-SHA256 der verschlüsselten Daten
//   danach die Dateien, jede für sich verschlüsselt.
//
// Verschlüsselung: ChaCha20 (RFC 8439, ohne fremde Bibliothek). Aus dem
// Paketschlüssel und dem Salz entstehen per HMAC zwei Schlüssel, einer zum
// Verschlüsseln und einer für die Prüfsummen. Das Inhaltsverzeichnis nutzt
// Nonce 0, Datei Nummer i (in der Reihenfolge des Verzeichnisses) Nonce i + 1.
//
// Braucht weder raylib noch ein Fenster: Spiel und Werkzeug nutzen denselben
// Code.
// ---------------------------------------------------------------------------
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pak {

using Key = std::array<uint8_t, 32>;

constexpr size_t kHeaderSize = 64;
constexpr uint32_t kVersion = 1;

// Paketschlüssel aus dem Inhalt der Schlüsseldatei: SHA-256 über alle Bytes.
// CMake rechnet dasselbe mit file(SHA256) (siehe CMakeLists.txt).
Key KeyFromFileContents(const std::string& contents);

// Schlüssel zum Verschlüsseln und für die Prüfsummen eines Pakets
struct Keys {
    Key enc{};
    Key mac{};
};
Keys DeriveKeys(const Key& key, const uint8_t salt[16]);

// ChaCha20: verschlüsselt und entschlüsselt (dieselbe Rechnung) len Bytes an Ort und Stelle
void ChaCha20(const Key& key, uint64_t nonce, uint8_t* data, size_t len);

struct Entry {
    std::string path;            // relativ zum Asset-Ordner, z. B. "sounds/scifi/laserSmall_000.ogg"
    uint64_t offset = 0;         // Lage der Daten in der Paketdatei
    uint64_t size = 0;           // Länge in Byte
    uint64_t nonce = 0;          // Nummer im Inhaltsverzeichnis + 1
    std::array<uint8_t, 32> mac{};   // HMAC-SHA256 der verschlüsselten Daten
};

// Inhaltsverzeichnis in Klartext (vor dem Verschlüsseln) bzw. zurück
std::vector<uint8_t> WriteIndex(const std::vector<Entry>& entries);
bool ReadIndex(const std::vector<uint8_t>& data, uint32_t count, std::vector<Entry>& out);

// Liest ein Paket. Jede Datei wird beim Lesen geprüft und entschlüsselt; die
// Paketdatei wird dafür jedes Mal kurz geöffnet (kein offener Zustand, also
// auch aus mehreren Threads nutzbar).
class Reader {
public:
    // false: Datei fehlt, ist kein Paket, falscher Schlüssel oder beschädigt
    // (err sagt, was).
    bool Open(const std::string& file, const Key& key, std::string& err);
    bool IsOpen() const { return open_; }

    // Eintrag zum Pfad (relativ zum Asset-Ordner) oder nullptr
    const Entry* Find(const std::string& path) const;
    // Alle Einträge, nach Pfad sortiert
    const std::vector<Entry>& Entries() const { return entries_; }

    // Daten des Eintrags nach out (mindestens e.size Bytes). false bei
    // Lesefehler oder falscher Prüfsumme.
    bool Read(const Entry& e, uint8_t* out) const;
    bool Read(const std::string& path, std::vector<uint8_t>& out) const;

private:
    bool open_ = false;
    std::string file_;
    Keys keys_;
    std::vector<Entry> entries_;
};

} // namespace pak
