// ---------------------------------------------------------------------------
// MapFiles.cpp – eigene Karten speichern, laden und auflisten (Schnittstelle
// und Erklärung: MapFiles.hpp).
// ---------------------------------------------------------------------------
#include "MapFiles.hpp"
#include "raylib.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
const char* kExt = ".karte";

std::string PathOf(const std::string& stem) { return MapDir() + stem + kExt; }

bool ReadFile(const std::string& path, std::string& text) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}
} // namespace

std::string MapDir() { return std::string(GetApplicationDirectory()) + "karten/"; }

std::string MapFileStem(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size() && out.size() < 48; ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        // Umlaute und ß (UTF-8: zwei Bytes, erstes 0xC3) umschreiben
        if (c == 0xC3 && i + 1 < name.size()) {
            unsigned char d = static_cast<unsigned char>(name[i + 1]);
            const char* r = nullptr;
            switch (d) {
                case 0xA4: r = "ae"; break;   // ä
                case 0xB6: r = "oe"; break;   // ö
                case 0xBC: r = "ue"; break;   // ü
                case 0x84: r = "Ae"; break;   // Ä
                case 0x96: r = "Oe"; break;   // Ö
                case 0x9C: r = "Ue"; break;   // Ü
                case 0x9F: r = "ss"; break;   // ß
                default: break;
            }
            if (r) { out += r; ++i; continue; }
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out += static_cast<char>(c);
        else if (c < 0x80 || (c & 0xC0) == 0xC0) out += '_';   // Folgebytes eines UTF-8-Zeichens nicht noch einmal
    }
    if (out.empty()) out = "karte";
    return out;
}

std::vector<MapFileInfo> ListMaps() {
    std::vector<MapFileInfo> list;
    if (!DirectoryExists(MapDir().c_str())) return list;
    FilePathList files = LoadDirectoryFilesEx(MapDir().c_str(), kExt, false);
    for (unsigned int i = 0; i < files.count; ++i) {
        std::string stem = GetFileNameWithoutExt(files.paths[i]);
        GameMap m;
        if (!LoadMapFile(stem, m)) continue;
        list.push_back({stem, m.name.empty() ? stem : m.name});
    }
    UnloadDirectoryFiles(files);
    std::sort(list.begin(), list.end(), [](const MapFileInfo& a, const MapFileInfo& b) { return a.name < b.name; });
    return list;
}

bool MapFileExists(const std::string& stem) { return FileExists(PathOf(stem).c_str()); }

bool LoadMapFile(const std::string& stem, GameMap& out) {
    std::string text;
    if (stem.empty() || !ReadFile(PathOf(stem), text)) return false;
    return MapFromText(text, out);
}

// Wie der Spielstand (Save.cpp): erst eine Hilfsdatei schreiben, dann
// umbenennen. So bleibt bei einem Abbruch die alte Karte heil.
bool SaveMapFile(const GameMap& m) {
    if (!DirectoryExists(MapDir().c_str()) && MakeDirectory(MapDir().c_str()) != 0) return false;
    const std::string path = PathOf(MapFileStem(m.name));
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << MapToText(m);
        f.close();
        if (!f) return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        // Windows: rename überschreibt keine vorhandene Datei
        std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    }
    return true;
}
