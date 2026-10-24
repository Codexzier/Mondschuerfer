// ---------------------------------------------------------------------------
// TiltShift.cpp – Kameraeffekte der 3D-Welt: Tilt-Shift, Kantenglättung
// (MSAA) und Glätten per Shader (FXAA) (Schnittstelle: TiltShift.hpp).
//
// Ablauf:
//   - Shader und OpenGL-Funktionen werden beim ersten Begin() geladen (dann
//     gibt es sicher schon den OpenGL-Kontext).
//   - Die Bilder haben immer die Fenstergröße; ändert sich das Fenster oder
//     die Kantenglättung, werden sie neu angelegt.
//   - Kantenglättung an (2x bis 16x): gezeichnet wird in ein Bild mit so
//     vielen Abtastpunkten pro Bildpunkt (MSAA-Framebuffer aus
//     Renderbuffern). End() rechnet es per glBlitFramebuffer in ein normales
//     Bild (resolve_) um, das der Shader lesen kann. Kantenglättung aus:
//     gezeichnet wird direkt in resolve_.
//     raylib kann solche Renderbuffer nicht selbst anlegen; die vier nötigen
//     OpenGL-Funktionen holt LoadGlOnce() über glfwGetProcAddress (GLFW
//     steckt in raylib). Fehlen sie oder kann die Grafikkarte kein MSAA,
//     bleibt die Kantenglättung aus.
//   - Der Shader mittelt für jeden Bildpunkt 24 Nachbarpunkte auf einer
//     Spirale. Der Radius ist in der Bildmitte 0 (scharf) und wächst zum
//     oberen und unteren Rand hin auf höchstens kMaxBlur der Bildhöhe mal
//     Stärke.
//   - FXAA (Fxaa() im Shader): sucht anhand der Helligkeit der vier
//     diagonalen Nachbarn die Richtung einer Kante und mittelt entlang dieser
//     Kante. Die Stärke mischt zwischen Original (0) und geglättet (1).
//   MSAA und FXAA lassen sich zusammen benutzen.
//   - Stil-Look "Retro-Pixel" (g_clientConfig.pixelLook): Der Shader liest
//     für alle Bildpunkte eines Blocks dieselbe Stelle und verringert die
//     Farbtiefe (mit Dithern). Nur das Bild der 3D-Welt, HUD und Menüs
//     kommen erst danach und bleiben scharf.
//   - Bildblitz (fx::ScreenFlash, Render.cpp) und Sandsturm-Hintergrund
//     (fx::SandColor) gehören zu den Grafikeffekten in Effects.hpp.
//   Der Shader liest das Bild nur mit textureLod(..., 0.0): texture() braucht
//   die Ableitungen der Texturkoordinaten, und die sind in Schleifen und
//   Verzweigungen, die je Bildpunkt anders laufen (der Weichzeichner), nicht
//   festgelegt. Manche Grafiktreiber liefern dort dann Unsinn (weiße oder
//   flackernde Flächen).
// ---------------------------------------------------------------------------
#include "TiltShift.hpp"
#include "ClientConfig.hpp"
#include "Effects.hpp"
#include "rlgl.h"
#include <algorithm>
#include <cmath>

// Aus GLFW (in raylib enthalten): Adresse einer OpenGL-Funktion
extern "C" void (*glfwGetProcAddress(const char* name))(void);

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
uniform float fxaa;      // FXAA-Stärke 0..1 (0 = aus)
uniform float pixel;     // Retro-Pixel: Blockgröße in Bildpunkten (0 = aus)
uniform float flash;     // Bildblitz einer nahen Explosion 0..1
out vec4 finalColor;

// FXAA (vereinfachte Form nach Timothy Lottes). Grenzwerte:
// REDUCE_MIN/REDUCE_MUL verhindern zu lange Suchrichtungen in dunklen bzw.
// flachen Bereichen, SPAN_MAX begrenzt die Suche auf 8 Bildpunkte.
const float FXAA_REDUCE_MIN = 1.0 / 128.0;
const float FXAA_REDUCE_MUL = 1.0 / 8.0;
const float FXAA_SPAN_MAX = 8.0;
vec3 Fxaa(vec2 uv) {
    const vec3 toLuma = vec3(0.299, 0.587, 0.114);
    float lNW = dot(textureLod(texture0, uv + vec2(-1.0, -1.0) * texel, 0.0).rgb, toLuma);
    float lNE = dot(textureLod(texture0, uv + vec2(1.0, -1.0) * texel, 0.0).rgb, toLuma);
    float lSW = dot(textureLod(texture0, uv + vec2(-1.0, 1.0) * texel, 0.0).rgb, toLuma);
    float lSE = dot(textureLod(texture0, uv + vec2(1.0, 1.0) * texel, 0.0).rgb, toLuma);
    float lM = dot(textureLod(texture0, uv, 0.0).rgb, toLuma);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    // Richtung quer zum Helligkeitsgefälle = entlang der Kante
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * FXAA_REDUCE_MUL, FXAA_REDUCE_MIN);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * texel;
    vec3 a = 0.5 * (textureLod(texture0, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                    textureLod(texture0, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 b = a * 0.5 + 0.25 * (textureLod(texture0, uv - dir * 0.5, 0.0).rgb +
                               textureLod(texture0, uv + dir * 0.5, 0.0).rgb);
    // Liegt das weitere Mittel außerhalb der Nachbarhelligkeiten, hat es
    // über die Kante hinaus gegriffen: dann das engere nehmen.
    float lB = dot(b, toLuma);
    return (lB < lMin || lB > lMax) ? a : b;
}

// Geordnetes Dithern: Schwelle 0..1 aus einer 4x4-Bayer-Matrix (die
// Schwellen sind so verteilt, dass benachbarte Blöcke möglichst verschieden sind).
float Bayer4(vec2 p) {
    const float m[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    return (m[int(p.x) + int(p.y) * 4] + 0.5) / 16.0;
}

void main() {
    // Retro-Pixel: Alle Bildpunkte eines Blocks lesen dieselbe Stelle (die
    // Blockmitte), so entstehen große "Pixel".
    vec2 uv = fragTexCoord;
    vec2 cell = vec2(0.0);
    if (pixel > 1.0) {
        vec2 block = texel * pixel;
        cell = floor(uv / block);
        uv = (cell + 0.5) * block;
    }
    // 0 in der Bildmitte, 1 am oberen bzw. unteren Rand. Der Streifen
    // |d| < 0.2 bleibt ganz scharf, danach wächst die Unschärfe weich an.
    float d = abs(uv.y - 0.5) * 2.0;
    float r = smoothstep(0.2, 1.0, d) * radius;
    vec3 sum = textureLod(texture0, uv, 0.0).rgb;
    // FXAA nicht beim Pixel-Look: Die harten Kanten sind dort gewollt.
    if (fxaa > 0.0 && pixel <= 1.0) sum = mix(sum, Fxaa(uv), fxaa);
    if (r > 0.25) {
        // 24 Punkte auf einer Spirale (goldener Winkel 2.39996 rad): gleichmäßig
        // über die Kreisfläche verteilt, ohne sichtbares Muster.
        for (int i = 1; i <= 24; ++i) {
            float a = float(i) * 2.39996;
            vec2 o = vec2(cos(a), sin(a)) * sqrt(float(i) / 24.0) * r;
            sum += textureLod(texture0, uv + o * texel, 0.0).rgb;
        }
        sum /= 25.0;
    }
    // Bildblitz: zu warmem Weiß hin aufhellen
    sum = mix(sum, vec3(1.0, 0.93, 0.8), flash);
    // Retro-Pixel: nur 12 Helligkeitsstufen je Farbkanal. Die Schwelle
    // wechselt von Block zu Block (Dithern), damit Verläufe nicht in breite
    // Streifen zerfallen. floor(x + Schwelle) rundet im Mittel richtig.
    if (pixel > 1.0) sum = floor(sum * 12.0 + Bayer4(mod(cell, 4.0))) / 12.0;
    // Mit der Vertexfarbe multiplizieren (beim Bild der Welt WHITE, also ohne
    // Wirkung): Zeichnet ein Treiber eine nachfolgende 2D-Fläche noch mit
    // diesem Shader, behält sie so wenigstens ihre Farbe und Durchsichtigkeit
    // statt weiß zu erscheinen.
    finalColor = vec4(sum, 1.0) * fragColor;
}
)";

// --- OpenGL-Funktionen für die MSAA-Renderbuffer ---------------------------
#if defined(_WIN32)
#define TS_APIENTRY __stdcall
#else
#define TS_APIENTRY
#endif
using PfnGenRenderbuffers = void(TS_APIENTRY*)(int, unsigned int*);
using PfnDeleteRenderbuffers = void(TS_APIENTRY*)(int, const unsigned int*);
using PfnBindRenderbuffer = void(TS_APIENTRY*)(unsigned int, unsigned int);
using PfnRenderbufferStorageMultisample = void(TS_APIENTRY*)(unsigned int, int, unsigned int, int, int);
using PfnGetIntegerv = void(TS_APIENTRY*)(unsigned int, int*);
constexpr unsigned int GL_RENDERBUFFER_ = 0x8D41;
constexpr unsigned int GL_RGBA8_ = 0x8058;
constexpr unsigned int GL_DEPTH_COMPONENT24_ = 0x81A6;
constexpr unsigned int GL_MAX_SAMPLES_ = 0x8D57;
constexpr int GL_COLOR_BUFFER_BIT_ = 0x4000;

PfnGenRenderbuffers glGenRenderbuffers_ = nullptr;
PfnDeleteRenderbuffers glDeleteRenderbuffers_ = nullptr;
PfnBindRenderbuffer glBindRenderbuffer_ = nullptr;
PfnRenderbufferStorageMultisample glRenderbufferStorageMultisample_ = nullptr;
bool glTried_ = false;
int maxSamples_ = 0;         // höchste MSAA-Stufe der Grafikkarte (0 = kein MSAA)

// --- Zustand -----------------------------------------------------------------
Shader shader_{};
bool shaderTried_ = false;   // Übersetzen schon versucht (nicht jedes Frame neu)
bool shaderOk_ = false;
int locTexel_ = -1;
int locRadius_ = -1;
int locFxaa_ = -1;
int locPixel_ = -1;
int locFlash_ = -1;
RenderTexture2D resolve_{};  // normales Bild, das der Shader liest
unsigned int msaaFbo_ = 0;   // MSAA-Framebuffer (0 = keiner)
unsigned int msaaColor_ = 0; // dessen Farb-Renderbuffer (Tiefe löscht rlUnloadFramebuffer)
int msaaSamples_ = 0, msaaW_ = 0, msaaH_ = 0;
bool active_ = false;        // Begin() hat umgelenkt, End() muss zurückschalten
bool usingMsaa_ = false;     // dieses Frame in msaaFbo_ gezeichnet

bool LoadShaderOnce() {
    if (!shaderTried_) {
        shaderTried_ = true;
        // nullptr = raylibs Standard-Vertex-Shader
        shader_ = LoadShaderFromMemory(nullptr, kFragmentShader);
        locTexel_ = GetShaderLocation(shader_, "texel");
        locRadius_ = GetShaderLocation(shader_, "radius");
        locFxaa_ = GetShaderLocation(shader_, "fxaa");
        locPixel_ = GetShaderLocation(shader_, "pixel");
        locFlash_ = GetShaderLocation(shader_, "flash");
        // Schlägt das Übersetzen fehl, liefert raylib seinen Standard-Shader;
        // die Locations sind dann -1 und alle Effekte bleiben aus.
        shaderOk_ = IsShaderValid(shader_) && locTexel_ >= 0 && locRadius_ >= 0 && locFxaa_ >= 0;
    }
    return shaderOk_;
}

void LoadGlOnce() {
    if (glTried_) return;
    glTried_ = true;
    glGenRenderbuffers_ = reinterpret_cast<PfnGenRenderbuffers>(glfwGetProcAddress("glGenRenderbuffers"));
    glDeleteRenderbuffers_ = reinterpret_cast<PfnDeleteRenderbuffers>(glfwGetProcAddress("glDeleteRenderbuffers"));
    glBindRenderbuffer_ = reinterpret_cast<PfnBindRenderbuffer>(glfwGetProcAddress("glBindRenderbuffer"));
    glRenderbufferStorageMultisample_ =
        reinterpret_cast<PfnRenderbufferStorageMultisample>(glfwGetProcAddress("glRenderbufferStorageMultisample"));
    const auto getIntegerv = reinterpret_cast<PfnGetIntegerv>(glfwGetProcAddress("glGetIntegerv"));
    if (glGenRenderbuffers_ && glDeleteRenderbuffers_ && glBindRenderbuffer_ && glRenderbufferStorageMultisample_ && getIntegerv)
        getIntegerv(GL_MAX_SAMPLES_, &maxSamples_);
    if (maxSamples_ < 2) maxSamples_ = 0;
}

void UnloadMsaa() {
    if (msaaFbo_ != 0) rlUnloadFramebuffer(msaaFbo_);
    if (msaaColor_ != 0) glDeleteRenderbuffers_(1, &msaaColor_);
    msaaFbo_ = msaaColor_ = 0;
    msaaSamples_ = msaaW_ = msaaH_ = 0;
}

// Legt den MSAA-Framebuffer passend an (oder behält ihn). false = geht nicht.
bool EnsureMsaa(int w, int h, int samples) {
    if (msaaFbo_ != 0 && msaaW_ == w && msaaH_ == h && msaaSamples_ == samples) return true;
    UnloadMsaa();
    unsigned int depth = 0;
    glGenRenderbuffers_(1, &msaaColor_);
    glBindRenderbuffer_(GL_RENDERBUFFER_, msaaColor_);
    glRenderbufferStorageMultisample_(GL_RENDERBUFFER_, samples, GL_RGBA8_, w, h);
    glGenRenderbuffers_(1, &depth);
    glBindRenderbuffer_(GL_RENDERBUFFER_, depth);
    glRenderbufferStorageMultisample_(GL_RENDERBUFFER_, samples, GL_DEPTH_COMPONENT24_, w, h);
    glBindRenderbuffer_(GL_RENDERBUFFER_, 0);
    msaaFbo_ = rlLoadFramebuffer();
    rlFramebufferAttach(msaaFbo_, msaaColor_, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_RENDERBUFFER, 0);
    rlFramebufferAttach(msaaFbo_, depth, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
    if (!rlFramebufferComplete(msaaFbo_)) {
        UnloadMsaa();
        return false;
    }
    msaaSamples_ = samples;
    msaaW_ = w;
    msaaH_ = h;
    return true;
}

} // namespace

namespace tiltshift {

int MaxSamples() {
    LoadGlOnce();
    return maxSamples_;
}

void Begin(Color clear) {
    active_ = false;
    if (!LoadShaderOnce()) return;
    LoadGlOnce();
    const int w = GetScreenWidth(), h = GetScreenHeight();
    if (w <= 0 || h <= 0) return;
    if (resolve_.id == 0 || resolve_.texture.width != w || resolve_.texture.height != h) {
        if (resolve_.id != 0) UnloadRenderTexture(resolve_);
        resolve_ = LoadRenderTexture(w, h);
        SetTextureFilter(resolve_.texture, TEXTURE_FILTER_BILINEAR);
        // Am Bildrand nicht auf die gegenüberliegende Seite greifen (raylib
        // stellt standardmäßig "Wiederholen" ein)
        SetTextureWrap(resolve_.texture, TEXTURE_WRAP_CLAMP);
    }
    int samples = g_clientConfig.msaa < maxSamples_ ? g_clientConfig.msaa : maxSamples_;
    usingMsaa_ = samples >= 2 && EnsureMsaa(w, h, samples);
    // Klappt eine Stufe nicht (Framebuffer unvollständig), ab jetzt höchstens
    // die nächstkleinere versuchen – statt jedes Frame neu anzulegen.
    if (samples >= 2 && !usingMsaa_) maxSamples_ = samples / 2 >= 2 ? samples / 2 : 0;
    if (!usingMsaa_ && msaaFbo_ != 0) UnloadMsaa();   // ausgeschaltet: Speicher freigeben
    // BeginTextureMode braucht nur id und Größe; ein Farbbild hat der
    // MSAA-Framebuffer nicht (texture.id bleibt 0).
    RenderTexture2D target = resolve_;
    if (usingMsaa_) {
        target = RenderTexture2D{};
        target.id = msaaFbo_;
        target.texture.width = w;
        target.texture.height = h;
    }
    BeginTextureMode(target);
    // Im Sandsturm verschwindet auch der Hintergrund (Horizont) im Staub
    const float sand = g_clientConfig.sandstorm;
    if (sand > 0.0f) {
        const Color dust = fx::SandColor();
        const float k = 0.5f + 0.5f * sand;
        clear = Color{static_cast<unsigned char>(clear.r + (dust.r - clear.r) * k), static_cast<unsigned char>(clear.g + (dust.g - clear.g) * k),
                      static_cast<unsigned char>(clear.b + (dust.b - clear.b) * k), 255};
    }
    ClearBackground(clear);
    active_ = true;
}

bool Active() { return active_; }

void End() {
    if (!active_) return;
    active_ = false;
    EndTextureMode();
    const int iw = resolve_.texture.width, ih = resolve_.texture.height;
    if (usingMsaa_) {
        // MSAA-Bild in das normale Bild umrechnen (jeder Bildpunkt = Mittel
        // seiner Abtastpunkte)
        rlBindFramebuffer(RL_READ_FRAMEBUFFER, msaaFbo_);
        rlBindFramebuffer(RL_DRAW_FRAMEBUFFER, resolve_.id);
        rlBlitFramebuffer(0, 0, iw, ih, 0, 0, iw, ih, GL_COLOR_BUFFER_BIT_);
        rlDisableFramebuffer();   // wieder der Bildschirm (Lesen und Schreiben)
    }
    const float w = static_cast<float>(iw), h = static_cast<float>(ih);
    const float texel[2] = {1.0f / w, 1.0f / h};
    const float radius = g_clientConfig.tiltShift * kMaxBlur * h;
    const float fxaa = g_clientConfig.fxaa;
    // Retro-Pixel: Stufe 1..3 = so viele Bildzeilen wie ein alter Bildschirm
    // (360, 240, 160); die Blockgröße folgt daraus und aus der Fensterhöhe.
    static const float kPixelLines[4] = {0.0f, 360.0f, 240.0f, 160.0f};
    const int look = g_clientConfig.pixelLook;
    const float pixel = look >= 1 && look <= 3 ? std::max(2.0f, roundf(h / kPixelLines[look])) : 0.0f;
    const float flash = fx::ScreenFlash();
    // Werte erst nach BeginShaderMode setzen: SetShaderValue schaltet in
    // OpenGL auf den Shader um, ohne dass raylib davon weiß. So stimmen
    // OpenGL und raylib beim Zeichnen sicher überein.
    BeginShaderMode(shader_);
    SetShaderValue(shader_, locTexel_, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader_, locRadius_, &radius, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locFxaa_, &fxaa, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locPixel_, &pixel, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locFlash_, &flash, SHADER_UNIFORM_FLOAT);
    // Negative Höhe: Bilder aus BeginTextureMode stehen in OpenGL auf dem Kopf.
    DrawTextureRec(resolve_.texture, {0.0f, 0.0f, w, -h}, {0.0f, 0.0f}, WHITE);
    EndShaderMode();
    // Das Bild sofort zeichnen und wieder den Standard-Shader in OpenGL
    // aktivieren, bevor HUD und Menüs kommen
    rlDrawRenderBatchActive();
    rlEnableShader(rlGetShaderIdDefault());
}

void Shutdown() {
    if (msaaFbo_ != 0) UnloadMsaa();
    if (resolve_.id != 0) UnloadRenderTexture(resolve_);
    resolve_ = RenderTexture2D{};
    if (shaderOk_) UnloadShader(shader_);
    shaderOk_ = false;
    shaderTried_ = false;
}

} // namespace tiltshift
