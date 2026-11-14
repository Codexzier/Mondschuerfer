// ---------------------------------------------------------------------------
// PackAssets.cpp – Werkzeug: packt den Asset-Ordner in eine verschlüsselte
// Paketdatei (Aufbau: src/AssetPack.hpp).
//
// Aufruf (macht CMake bei jedem Build, wenn sich ein Asset geändert hat):
//   PackAssets <Asset-Ordner> <Schlüsseldatei> <Ausgabe, z. B. assets.pak>
//
// Ablauf:
//   1. Alle Dateien im Asset-Ordner sammeln (mit Unterordnern, sortiert).
//      Ausgelassen wird nur der Ordner NewAssets (Original-Zips der Kits,
//      das Spiel braucht sie nicht).
//   2. Paketschlüssel = SHA-256 der Schlüsseldatei, Salz = Zufall.
//   3. Jede Datei verschlüsseln und hinter Kopf und Inhaltsverzeichnis
//      schreiben, dann Inhaltsverzeichnis und Kopf ausfüllen.
//   4. Zur Kontrolle das fertige Paket mit pak::Reader (demselben Code wie im
//      Spiel) wieder lesen und jede Datei mit dem Original vergleichen.
//   Geschrieben wird zuerst <Ausgabe>.tmp; erst wenn alles stimmt, wird es
//   umbenannt. Bei einem Fehler endet das Programm mit Code 1 (Build bricht ab).
// ---------------------------------------------------------------------------
#include "AssetPack.hpp"
#include "Crypto.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* kSkipDirs[] = {"NewAssets"};

bool ReadAll(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

int Fail(const std::string& msg) {
    fprintf(stderr, "PackAssets: %s\n", msg.c_str());
    return 1;
}

void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) return Fail("Aufruf: PackAssets <Asset-Ordner> <Schluesseldatei> <Ausgabe.pak>");
    const fs::path dir = argv[1];
    const fs::path out = argv[3];
    const fs::path tmp = out.string() + ".tmp";

    // 1. Dateien sammeln
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return Fail("Asset-Ordner nicht gefunden: " + dir.string());
    std::vector<std::string> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(dir);
        if (it->is_directory() && it.depth() == 0 &&
            std::find(std::begin(kSkipDirs), std::end(kSkipDirs), rel.generic_string()) != std::end(kSkipDirs)) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file()) files.push_back(rel.generic_u8string());   // UTF-8, wie u8path unten
    }
    if (ec) return Fail("Asset-Ordner nicht lesbar: " + ec.message());
    if (files.empty()) return Fail("Asset-Ordner ist leer: " + dir.string());
    std::sort(files.begin(), files.end());

    // 2. Schlüssel
    std::vector<uint8_t> keyFile;
    if (!ReadAll(argv[2], keyFile) || keyFile.empty()) return Fail(std::string("Schluesseldatei fehlt oder ist leer: ") + argv[2]);
    const pak::Key key = pak::KeyFromFileContents(std::string(keyFile.begin(), keyFile.end()));
    uint8_t head[pak::kHeaderSize] = {'M', 'S', 'P', 'K'};
    Put32(head + 4, pak::kVersion);
    crypto::RandomBytes(head + 8, 16);
    const pak::Keys keys = pak::DeriveKeys(key, head + 8);

    // 3. Schreiben. Die Länge des Inhaltsverzeichnisses steht schon vorher
    // fest (hängt nur an den Pfaden), also auch, wo die Daten beginnen.
    std::vector<pak::Entry> entries(files.size());
    for (size_t i = 0; i < files.size(); ++i) entries[i].path = files[i];
    const size_t indexSize = pak::WriteIndex(entries).size();
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return Fail("Ausgabe nicht beschreibbar: " + tmp.string());
    const std::vector<char> placeholder(pak::kHeaderSize + indexSize, 0);
    f.write(placeholder.data(), static_cast<std::streamsize>(placeholder.size()));
    uint64_t offset = placeholder.size();
    std::vector<crypto::Digest> plainHash(files.size());
    std::vector<uint8_t> data;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!ReadAll(dir / fs::u8path(files[i]), data)) return Fail("Datei nicht lesbar: " + files[i]);
        if (files[i].size() > 0xFFFF) return Fail("Pfad zu lang: " + files[i]);
        plainHash[i] = crypto::Sha256(data.data(), data.size());
        pak::Entry& e = entries[i];
        e.offset = offset;
        e.size = data.size();
        e.nonce = i + 1;
        pak::ChaCha20(keys.enc, e.nonce, data.data(), data.size());
        const crypto::Digest mac = crypto::HmacSha256(keys.mac.data(), keys.mac.size(), data.data(), data.size());
        std::copy(mac.begin(), mac.end(), e.mac.begin());
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        offset += data.size();
    }
    std::vector<uint8_t> index = pak::WriteIndex(entries);
    pak::ChaCha20(keys.enc, 0, index.data(), index.size());
    Put32(head + 24, static_cast<uint32_t>(entries.size()));
    Put32(head + 28, static_cast<uint32_t>(index.size()));
    std::vector<uint8_t> signedPart(head, head + 32);
    signedPart.insert(signedPart.end(), index.begin(), index.end());
    const crypto::Digest headMac = crypto::HmacSha256(keys.mac.data(), keys.mac.size(), signedPart.data(), signedPart.size());
    std::copy(headMac.begin(), headMac.end(), head + 32);
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(head), pak::kHeaderSize);
    f.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
    f.close();
    if (!f) return Fail("Schreiben fehlgeschlagen: " + tmp.string());

    // 4. Kontrolle mit demselben Lesecode wie im Spiel
    pak::Reader reader;
    std::string err;
    if (!reader.Open(tmp.string(), key, err)) return Fail("Kontrolle: " + err);
    if (reader.Entries().size() != files.size()) return Fail("Kontrolle: Anzahl der Dateien stimmt nicht");
    for (size_t i = 0; i < files.size(); ++i) {
        if (!reader.Read(files[i], data)) return Fail("Kontrolle: nicht lesbar: " + files[i]);
        if (crypto::Sha256(data.data(), data.size()) != plainHash[i]) return Fail("Kontrolle: Inhalt falsch: " + files[i]);
    }

    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    if (ec) return Fail("Umbenennen fehlgeschlagen: " + ec.message());
    printf("PackAssets: %zu Dateien, %.1f MB -> %s (geprueft)\n", files.size(), static_cast<double>(offset) / (1024.0 * 1024.0),
           out.string().c_str());
    return 0;
}
