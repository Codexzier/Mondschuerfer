// ---------------------------------------------------------------------------
// TiltShift.cpp – Kameraeffekt "Tilt-Shift" (Schnittstelle: TiltShift.hpp).
//
// Ablauf:
//   - Der Shader wird beim ersten Begin() mit eingeschaltetem Effekt
//     übersetzt (dann gibt es sicher schon den OpenGL-Kontext).
//   - Das Bild (RenderTexture) hat immer die Fenstergröße; ändert sich das
//     Fenster, wird es neu angelegt.
//   - Der Shader mittelt für jeden Bildpunkt 24 Nachbarpunkte auf einer
//     Spirale. Der Radius ist in der Bildmitte 0 (scharf) und wächst zum
//     oberen und unteren Rand hin auf höchstens kMaxBlur der Bildhöhe mal
//     Stärke.
// ---------------------------------------------------------------------------
#include "TiltShift.hpp"
#include "ClientConfig.hpp"

namespace {

// Größter Weichzeichner-Radius bei Stärke 100 %, als Anteil der Bildhöhe
// (bei 900 Pixeln Höhe etwa 11 Pixel).
constexpr float kMaxBlur = 0.012f;

const char* kFragmentShader = R"(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 texel;      // Größe eines Bildpunkts in Texturkoordinaten
uniform float radius;    // größter Radius in Bildpunkten (Rand oben/unten)
out vec4 finalColor;
void main() {
    // 0 in der Bildmitte, 1 am oberen bzw. unteren Rand. Der Streifen
    // |d| < 0.2 bleibt ganz scharf, danach wächst die Unschärfe weich an.
    float d = abs(fragTexCoord.y - 0.5) * 2.0;
    float r = smoothstep(0.2, 1.0, d) * radius;
    vec3 sum = texture(texture0, fragTexCoord).rgb;
    if (r > 0.25) {
        // 24 Punkte auf einer Spirale (goldener Winkel 2.39996 rad): gleichmäßig
        // über die Kreisfläche verteilt, ohne sichtbares Muster.
        for (int i = 1; i <= 24; ++i) {
            float a = float(i) * 2.39996;
            vec2 o = vec2(cos(a), sin(a)) * sqrt(float(i) / 24.0) * r;
            sum += texture(texture0, fragTexCoord + o * texel).rgb;
        }
        sum /= 25.0;
    }
    finalColor = vec4(sum, 1.0);
}
)";

Shader shader_{};
bool shaderTried_ = false;   // Übersetzen schon versucht (nicht jedes Frame neu)
bool shaderOk_ = false;
int locTexel_ = -1;
int locRadius_ = -1;
RenderTexture2D target_{};
bool active_ = false;        // Begin() hat umgelenkt, End() muss zurückschalten

bool LoadShaderOnce() {
    if (!shaderTried_) {
        shaderTried_ = true;
        // nullptr = raylibs Standard-Vertex-Shader
        shader_ = LoadShaderFromMemory(nullptr, kFragmentShader);
        locTexel_ = GetShaderLocation(shader_, "texel");
        locRadius_ = GetShaderLocation(shader_, "radius");
        // Schlägt das Übersetzen fehl, liefert raylib seinen Standard-Shader;
        // die Locations sind dann -1 und der Effekt bleibt aus.
        shaderOk_ = IsShaderValid(shader_) && locTexel_ >= 0 && locRadius_ >= 0;
    }
    return shaderOk_;
}

} // namespace

namespace tiltshift {

void Begin(Color clear) {
    active_ = false;
    if (g_clientConfig.tiltShift <= 0.0f || !LoadShaderOnce()) return;
    const int w = GetScreenWidth(), h = GetScreenHeight();
    if (w <= 0 || h <= 0) return;
    if (target_.id == 0 || target_.texture.width != w || target_.texture.height != h) {
        if (target_.id != 0) UnloadRenderTexture(target_);
        target_ = LoadRenderTexture(w, h);
        SetTextureFilter(target_.texture, TEXTURE_FILTER_BILINEAR);
    }
    BeginTextureMode(target_);
    ClearBackground(clear);
    active_ = true;
}

void End() {
    if (!active_) return;
    active_ = false;
    EndTextureMode();
    const float w = static_cast<float>(target_.texture.width), h = static_cast<float>(target_.texture.height);
    const float texel[2] = {1.0f / w, 1.0f / h};
    const float radius = g_clientConfig.tiltShift * kMaxBlur * h;
    SetShaderValue(shader_, locTexel_, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader_, locRadius_, &radius, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(shader_);
    // Negative Höhe: Bilder aus BeginTextureMode stehen in OpenGL auf dem Kopf.
    DrawTextureRec(target_.texture, {0.0f, 0.0f, w, -h}, {0.0f, 0.0f}, WHITE);
    EndShaderMode();
}

void Shutdown() {
    if (target_.id != 0) UnloadRenderTexture(target_);
    target_ = RenderTexture2D{};
    if (shaderOk_) UnloadShader(shader_);
    shaderOk_ = false;
    shaderTried_ = false;
}

} // namespace tiltshift
