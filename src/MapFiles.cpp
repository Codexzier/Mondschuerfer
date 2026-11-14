// ---------------------------------------------------------------------------
// MapFiles.cpp – eigene Karten speichern, laden und auflisten (Schnittstelle
// und Erklärung: MapFiles.hpp).
// ---------------------------------------------------------------------------
#include "MapFiles.hpp"
#include "Assets.hpp"
#include "raylib.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
const char* kExt = ".karte";

std::string PathOf(const std::string& stem) { return MapDir() + stem + kExt; }
// Mitgelieferte Karten (assets/karten bzw. im Asset-Paket, nur lesen)
std::string BuiltinDir() { return g_assets.AssetDir() + "karten/"; }
std::string BuiltinPathOf(const std::string& stem) { return BuiltinDir() + stem + kExt; }

bool ReadFile(const std::string& path, std::string& text) {
    if (InAssetPack(path)) {
        std::vector<unsigned char> data;
        if (!LoadAssetBytes(path, data)) return false;
        text.assign(data.begin(), data.end());
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}
} // namespace

std::string MapDir() { return std::string(GetApplicationDirectory()) + "karten/"; }

// Erst die eigenen Karten, dann die mitgelieferten, die keine eigene Karte
// mit demselben Dateinamen ersetzt.
std::vector<MapFileInfo> ListMaps() {
    std::vector<MapFileInfo> list;
    for (const std::string& dir : {MapDir(), BuiltinDir()}) {
        for (const std::string& file : ListAssetFiles(dir, kExt)) {
            std::string stem = GetFileNameWithoutExt(file.c_str());
            if (std::any_of(list.begin(), list.end(), [&](const MapFileInfo& o) { return o.stem == stem; })) continue;
            GameMap m;
            if (!LoadMapFile(stem, m)) continue;
            list.push_back({stem, m.name.empty() ? stem : m.name, m.kind, m.theme, m.size, m.sky});
        }
    }
    std::sort(list.begin(), list.end(), [](const MapFileInfo& a, const MapFileInfo& b) { return a.name < b.name; });
    return list;
}

bool MapFileExists(const std::string& stem) {
    return FileExists(PathOf(stem).c_str()) || AssetExists(BuiltinPathOf(stem));
}

bool LoadMapFile(const std::string& stem, GameMap& out) {
    std::string text;
    if (stem.empty() || (!ReadFile(PathOf(stem), text) && !ReadFile(BuiltinPathOf(stem), text))) return false;
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
