#include "Assets.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

Assets g_assets;

namespace {

// ---------------------------------------------------------------------------
// Die GLB-Dateien des Space Kits haben einen Wurzelknoten "tmpParent", der in
// der Szene nicht aufgeführt ist, aber trotzdem als Elternknoten eingetragen
// ist. Der in raylib eingebaute glTF-Parser (cgltf) lehnt das ab. Beim Laden
// entfernen wir deshalb nur im Speicher den Eintrag  "children":[1],  (durch
// Leerzeichen gleicher Länge ersetzt). Die Dateien selbst bleiben unverändert.
// ---------------------------------------------------------------------------
unsigned char* LoadFilePatched(const char* fileName, int* dataSize) {
    *dataSize = 0;
    FILE* f = fopen(fileName, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return nullptr; }
    auto* data = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(size)));
    size_t read = fread(data, 1, static_cast<size_t>(size), f);
    fclose(f);
    *dataSize = static_cast<int>(read);

    if (read > 20 && memcmp(data, "glTF", 4) == 0) {
        const char* pattern = "\"children\":[1],\"name\":\"tmpParent\"";
        const size_t patLen = strlen(pattern);
        const size_t replaceLen = strlen("\"children\":[1],");
        // Nur im JSON-Block suchen
        unsigned int jsonLen = 0;
        memcpy(&jsonLen, data + 12, 4);
        size_t end = 20 + static_cast<size_t>(jsonLen);
        if (end > read) end = read;
        for (size_t i = 20; i + patLen <= end; ++i) {
            if (memcmp(data + i, pattern, patLen) == 0) {
                memset(data + i, ' ', replaceLen);
                break;
            }
        }
    }
    return data;
}

// Die Gesteinsfarben des Kits sind orange (Mars). Für die Mondoberfläche
// werden sie beim Laden auf Grautöne umgefärbt.
bool Near(Color a, int r, int g, int b) {
    return std::abs(a.r - r) < 6 && std::abs(a.g - g) < 6 && std::abs(a.b - b) < 6;
}

void ApplyMoonPalette(Model& m) {
    for (int i = 0; i < m.materialCount; ++i) {
        Color& c = m.materials[i].maps[MATERIAL_MAP_DIFFUSE].color;
        if (Near(c, 232, 133, 99))       c = {172, 174, 184, 255};  // rock
        else if (Near(c, 179, 94, 66))   c = {118, 120, 132, 255};  // rockDark
        else if (Near(c, 250, 148, 115)) c = {196, 197, 204, 255};  // rockTrack
    }
}

const char* kVertexShader = R"(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPosition;
out vec3 fragNormal;
out vec4 fragColor;
void main() {
    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kFragmentShader = R"(
#version 330
in vec3 fragPosition;
in vec3 fragNormal;
in vec4 fragColor;
uniform vec4 colDiffuse;
uniform vec3 lightDir;
uniform vec3 viewPos;
uniform vec3 emissive;
out vec4 finalColor;
void main() {
    vec3 n = normalize(fragNormal);
    vec3 l = normalize(-lightDir);
    float diff = max(dot(n, l), 0.0);
    float sky = 0.5 + 0.5 * n.y;
    vec3 base = colDiffuse.rgb * fragColor.rgb;
    vec3 col = base * (0.36 + 0.16 * sky + 0.62 * diff);
    vec3 v = normalize(viewPos - fragPosition);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
    col += vec3(0.10, 0.12, 0.16) * rim;
    col += emissive;
    finalColor = vec4(col, colDiffuse.a * fragColor.a);
}
)";

} // namespace

float AngleDiff(float a, float b) {
    float d = fmodf(b - a + PI, 2.0f * PI);
    if (d < 0) d += 2.0f * PI;
    return d - PI;
}

Matrix TRS(Vector3 pos, float yaw, float scale) {
    Matrix m = MatrixMultiply(MatrixScale(scale, scale, scale), MatrixRotateY(yaw));
    return MatrixMultiply(m, MatrixTranslate(pos.x, pos.y, pos.z));
}

Matrix TRS(Vector3 pos, float yaw, float pitch, float roll, float scale) {
    Matrix m = MatrixScale(scale, scale, scale);
    m = MatrixMultiply(m, MatrixRotateZ(roll));
    m = MatrixMultiply(m, MatrixRotateX(pitch));
    m = MatrixMultiply(m, MatrixRotateY(yaw));
    return MatrixMultiply(m, MatrixTranslate(pos.x, pos.y, pos.z));
}

bool Assets::Init() {
    SetLoadFileDataCallback(LoadFilePatched);

    const std::string app = GetApplicationDirectory();
    const char* candidates[] = {"assets/", "../assets/", "../../assets/"};
    std::vector<std::string> tries = {app + "assets/"};
    for (const char* c : candidates) tries.emplace_back(c);
    for (const auto& dir : tries) {
        if (FileExists((dir + "models/craft_speederA.glb").c_str())) {
            assetDir_ = dir;
            break;
        }
    }
    if (assetDir_.empty()) {
        TraceLog(LOG_ERROR, "Assets-Ordner nicht gefunden (erwartet: assets/models/*.glb)");
        return false;
    }

    lit_ = LoadShaderFromMemory(kVertexShader, kFragmentShader);
    locLightDir_ = GetShaderLocation(lit_, "lightDir");
    locViewPos_ = GetShaderLocation(lit_, "viewPos");
    locEmissive_ = GetShaderLocation(lit_, "emissive");
    Vector3 light = Vector3Normalize({-0.45f, -1.0f, -0.35f});
    SetShaderValue(lit_, locLightDir_, &light, SHADER_UNIFORM_VEC3);
    ready_ = true;
    return true;
}

void Assets::Shutdown() {
    for (auto& e : entries_) UnloadModel(e.model);
    entries_.clear();
    byName_.clear();
    if (ready_) UnloadShader(lit_);
    ready_ = false;
}

int Assets::Id(const std::string& name) {
    auto it = byName_.find(name);
    if (it != byName_.end()) return it->second;

    Entry e;
    const std::string path = assetDir_ + "models/" + name + ".glb";
    e.model = LoadModel(path.c_str());
    if (e.model.meshCount == 0 || e.model.meshes[0].vertexCount == 0) {
        TraceLog(LOG_WARNING, "Modell konnte nicht geladen werden: %s", path.c_str());
    }
    ApplyMoonPalette(e.model);
    for (int i = 0; i < e.model.materialCount; ++i) e.model.materials[i].shader = lit_;

    // Alle Kit-Modelle haben im Wurzelknoten eine Verschiebung um (2, 0, 1.5).
    // Diese wird ausgeglichen, sodass der Ursprung wieder im Kit-Raster liegt.
    e.base = MatrixTranslate(-2.0f, 0.0f, -1.5f);
    BoundingBox b = GetModelBoundingBox(e.model);
    b.min = Vector3Add(b.min, {-2.0f, 0.0f, -1.5f});
    b.max = Vector3Add(b.max, {-2.0f, 0.0f, -1.5f});
    e.bounds = b;

    entries_.push_back(e);
    int id = static_cast<int>(entries_.size()) - 1;
    byName_[name] = id;
    return id;
}

void Assets::SetCamera(Vector3 camPos) {
    SetShaderValue(lit_, locViewPos_, &camPos, SHADER_UNIFORM_VEC3);
}

void Assets::Draw(int id, const Matrix& world, Color tint, Vector3 emissive) {
    if (id < 0 || id >= static_cast<int>(entries_.size())) return;
    Entry& e = entries_[id];
    e.model.transform = MatrixMultiply(e.base, world);
    SetShaderValue(lit_, locEmissive_, &emissive, SHADER_UNIFORM_VEC3);
    DrawModel(e.model, {0, 0, 0}, 1.0f, tint);
}

void Assets::Draw(int id, Vector3 pos, float yaw, float scale, Color tint, Vector3 emissive) {
    Draw(id, TRS(pos, yaw, scale), tint, emissive);
}
