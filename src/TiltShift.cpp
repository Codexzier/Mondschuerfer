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
//   - Das Zwischenbild hat keine Kantenglättung (MSAA gilt nur für das
//     Fenster). Deshalb glättet der Shader den Bildpunkt selbst mit FXAA
//     (Fxaa() unten): Er sucht anhand der Helligkeit der vier diagonalen
//     Nachbarn die Richtung einer Kante und mittelt entlang dieser Kante.
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

// FXAA (vereinfachte Form nach Timothy Lottes). Grenzwerte:
// REDUCE_MIN/REDUCE_MUL verhindern zu lange Suchrichtungen in dunklen bzw.
// flachen Bereichen, SPAN_MAX begrenzt die Suche auf 8 Bildpunkte.
const float FXAA_REDUCE_MIN = 1.0 / 128.0;
const float FXAA_REDUCE_MUL = 1.0 / 8.0;
const float FXAA_SPAN_MAX = 8.0;
vec3 Fxaa(vec2 uv) {
    const vec3 toLuma = vec3(0.299, 0.587, 0.114);
    float lNW = dot(texture(texture0, uv + vec2(-1.0, -1.0) * texel).rgb, toLuma);
    float lNE = dot(texture(texture0, uv + vec2(1.0, -1.0) * texel).rgb, toLuma);
    float lSW = dot(texture(texture0, uv + vec2(-1.0, 1.0) * texel).rgb, toLuma);
    float lSE = dot(texture(texture0, uv + vec2(1.0, 1.0) * texel).rgb, toLuma);
    float lM = dot(texture(texture0, uv).rgb, toLuma);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    // Richtung quer zum Helligkeitsgefälle = entlang der Kante
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * FXAA_REDUCE_MUL, FXAA_REDUCE_MIN);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * texel;
    vec3 a = 0.5 * (texture(texture0, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                    texture(texture0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(texture0, uv - dir * 0.5).rgb +
                               texture(texture0, uv + dir * 0.5).rgb);
    // Liegt das weitere Mittel außerhalb der Nachbarhelligkeiten, hat es
    // über die Kante hinaus gegriffen: dann das engere nehmen.
    float lB = dot(b, toLuma);
    return (lB < lMin || lB > lMax) ? a : b;
}

void main() {
    // 0 in der Bildmitte, 1 am oberen bzw. unteren Rand. Der Streifen
    // |d| < 0.2 bleibt ganz scharf, danach wächst die Unschärfe weich an.
    float d = abs(fragTexCoord.y - 0.5) * 2.0;
    float r = smoothstep(0.2, 1.0, d) * radius;
    vec3 sum = Fxaa(fragTexCoord);
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
