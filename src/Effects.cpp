// ---------------------------------------------------------------------------
// Effects.cpp – Schattenkarte, Sandsturm und Bildblitz der 3D-Welt
// (Schnittstelle und Überblick: Effects.hpp).
//
// Schattenkarte, Ablauf je Bild (aus World::Draw, Render.cpp):
//   1. BeginShadowPass: den gerade benutzten Framebuffer (Bild der
//      Kameraeffekte oder Bildschirm) und den Bildausschnitt merken, auf den
//      eigenen Framebuffer umschalten und die Kamera durch die Sonne ersetzen.
//      Die Sonne ist eine Parallelprojektion (MatrixOrtho): Ihr Licht fällt
//      überall in derselben Richtung ein. Sie schaut auf das Gebiet, das die
//      Spielkamera zeigt, und deckt es mit einem Quadrat ab.
//   2. Render.cpp zeichnet die bewegten Objekte. Übrig bleibt im
//      Tiefenpuffer für jeden Bildpunkt der Karte der Abstand zur Sonne.
//   3. EndShadowPass: alles zurückstellen, die Tiefentextur auf den
//      Texturplatz kShadowSlot legen und dem Beleuchtungs-Shader die
//      Matrix der Sonne geben. Er rechnet damit jeden Bildpunkt in die Karte
//      um und vergleicht (Assets.cpp: Shadow()).
//   Den Framebuffer ändert raylib nur über BeginTextureMode/EndTextureMode,
//   und EndTextureMode schaltet immer auf den Bildschirm zurück. Mitten in
//   World::Draw ist aber schon das Bild der Kameraeffekte (TiltShift.cpp)
//   eingestellt. Deshalb fragt diese Datei den aktuellen Framebuffer und
//   Bildausschnitt direkt bei OpenGL ab (glGetIntegerv über GLFW wie in
//   TiltShift.cpp) und stellt ihn danach wieder her.
//   Die Karte hat neben der Tiefentextur auch ein (ungenutztes) Farbbild:
//   Ein Framebuffer ganz ohne Farbbild gilt bei manchen Treibern als
//   unvollständig.
//
// Sandsturm: SetFog gibt Mitte, Radien, Deckkraft und Zeit an den
//   Beleuchtungs-Shader (Modelle) und an den Effekt-Shader dieser Datei
//   (Render-Batch: Ringe, Strahlen, Partikel). Der Effekt-Shader blendet die
//   Effekte im Staub aus (Alpha), statt sie einzufärben – so verschwinden
//   additive Leuchteffekte genauso wie normal gemischte.
// ---------------------------------------------------------------------------
#include "Effects.hpp"
#include "Assets.hpp"
#include "ClientConfig.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <string>

// Aus GLFW (in raylib enthalten): Adresse einer OpenGL-Funktion
extern "C" void (*glfwGetProcAddress(const char* name))(void);

namespace {

// Wie stark der Schatten das Sonnenlicht wegnimmt (1 = ganz, dann bleiben
// nur Grund- und Himmelslicht).
constexpr float kShadowStrength = 0.8f;

// --- Effekt-Shader für den Render-Batch --------------------------------------------
// Wie raylibs Standard-Shader (Textur · Vertexfarbe), dazu der Nebel. Die
// Eckpunkte des Render-Batches liegen schon in Weltkoordinaten (rlgl rechnet
// rlPushMatrix/rlTranslatef selbst ein), daher ist vertexPosition gleich der
// Weltposition.
const char* kEffectVertex = R"(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragPosition;
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragPosition = vertexPosition;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kEffectFragmentHead = R"(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragPosition;
uniform sampler2D texture0;
out vec4 finalColor;
)";

const char* kEffectFragmentMain = R"(
void main() {
    vec4 c = texture(texture0, fragTexCoord) * fragColor;
    c.a *= 1.0 - FogAmount(fragPosition);
    finalColor = c;
}
)";

// --- OpenGL-Abfrage (Framebuffer und Bildausschnitt merken) -----------------------
#if defined(_WIN32)
#define FX_APIENTRY __stdcall
#else
#define FX_APIENTRY
#endif
using PfnGetIntegerv = void(FX_APIENTRY*)(unsigned int, int*);
constexpr unsigned int GL_FRAMEBUFFER_BINDING_ = 0x8CA6;
constexpr unsigned int GL_VIEWPORT_ = 0x0BA2;
PfnGetIntegerv getIntegerv_ = nullptr;
bool glTried_ = false;

// --- Zustand -------------------------------------------------------------------------
unsigned int shadowFbo_ = 0;     // Framebuffer der Schattenkarte (0 = keiner)
unsigned int shadowColor_ = 0;   // dessen Farbbild (nur damit er vollständig ist)
unsigned int shadowDepth_ = 0;   // dessen Tiefentextur = die Schattenkarte
int shadowSize_ = 0;             // Auflösung (Bildpunkte je Seite)
int shadowFailed_ = 0;           // Auflösung, die nicht ging (nicht jedes Bild neu versuchen)
int prevFbo_ = 0;                // vor dem Schattendurchgang eingestellter Framebuffer
int prevViewport_[4] = {0, 0, 0, 0};
Matrix prevProj_{}, prevView_{};
Matrix lightVP_{};               // Blick- mal Projektionsmatrix der Sonne

Shader effect_{};
bool effectTried_ = false, effectOk_ = false;
int locFogCenter_ = -1, locFogParams_ = -1;
bool effectsActive_ = false;     // BeginEffects hat den Shader eingeschaltet

bool fogOn_ = false;
Vector3 fogCenter_{};
Vector4 fogParams_{};            // wie fogParams im Shader (Effects.hpp)

float screenFlash_ = 0.0f;

void LoadGlOnce() {
    if (glTried_) return;
    glTried_ = true;
    getIntegerv_ = reinterpret_cast<PfnGetIntegerv>(glfwGetProcAddress("glGetIntegerv"));
}

bool LoadEffectOnce() {
    if (!effectTried_) {
        effectTried_ = true;
        const std::string fragment = std::string(kEffectFragmentHead) + fx::kFogGlsl + kEffectFragmentMain;
        effect_ = LoadShaderFromMemory(kEffectVertex, fragment.c_str());
        locFogCenter_ = GetShaderLocation(effect_, "fogCenter");
        locFogParams_ = GetShaderLocation(effect_, "fogParams");
        effectOk_ = IsShaderValid(effect_) && locFogCenter_ >= 0 && locFogParams_ >= 0;
    }
    return effectOk_;
}

void UnloadShadowMap() {
    // rlUnloadFramebuffer löscht die angehängte Tiefentextur mit
    if (shadowFbo_ != 0) rlUnloadFramebuffer(shadowFbo_);
    if (shadowColor_ != 0) rlUnloadTexture(shadowColor_);
    shadowFbo_ = shadowColor_ = shadowDepth_ = 0;
    shadowSize_ = 0;
}

// Legt die Schattenkarte in der Auflösung size an (oder behält sie).
// Achtung: rlFramebufferAttach/rlFramebufferComplete schalten danach auf den
// Bildschirm (Framebuffer 0) – der Aufrufer stellt den Framebuffer selbst ein.
bool EnsureShadowMap(int size) {
    if (shadowFbo_ != 0 && shadowSize_ == size) return true;
    UnloadShadowMap();
    shadowFbo_ = rlLoadFramebuffer();
    if (shadowFbo_ == 0) return false;
    shadowColor_ = rlLoadTexture(nullptr, size, size, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    shadowDepth_ = rlLoadTextureDepth(size, size, false);
    rlFramebufferAttach(shadowFbo_, shadowColor_, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(shadowFbo_, shadowDepth_, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(shadowFbo_)) {
        UnloadShadowMap();
        return false;
    }
    shadowSize_ = size;
    return true;
}

void RestoreTarget() {
    rlEnableFramebuffer(static_cast<unsigned int>(prevFbo_));
    rlViewport(prevViewport_[0], prevViewport_[1], prevViewport_[2], prevViewport_[3]);
}

} // namespace

namespace fx {

bool BeginShadowPass(const Camera3D& cam, float drawRange) {
    const int size = g_clientConfig.shadows;
    // Ohne Schattenkarte: der Shader rechnet keinen Schatten (Stärke 0)
    g_assets.SetShadow(MatrixIdentity(), kShadowSlot, 0.0f, 0.0f);
    // In der Übersicht (Zuschauer, ganze Karte) wären die Schatten nur
    // Bruchteile eines Bildpunkts: dort keine Schattenkarte.
    if (size <= 0 || size == shadowFailed_ || drawRange > 200.0f) return false;
    LoadGlOnce();
    if (!getIntegerv_) return false;

    // Bisher Gesammeltes noch in das alte Bild zeichnen, dann merken, wohin
    // gerade gezeichnet wird
    rlDrawRenderBatchActive();
    getIntegerv_(GL_FRAMEBUFFER_BINDING_, &prevFbo_);
    getIntegerv_(GL_VIEWPORT_, prevViewport_);
    if (!EnsureShadowMap(size)) {
        shadowFailed_ = size;
        RestoreTarget();
        TraceLog(LOG_WARNING, "Schattenkarte %dx%d nicht möglich, Schatten bleiben aus", size, size);
        return false;
    }

    // Gebiet der Karte: Die Kamera schaut schräg von vorn auf ihr Ziel; im
    // Bild ist hinter dem Ziel mehr Boden zu sehen als davor. Das Quadrat
    // wächst deshalb mit dem Kameraabstand (Zoom) und liegt etwas hinter dem
    // Ziel (in Blickrichtung).
    const Vector3 look = Vector3Subtract(cam.target, cam.position);
    const float camDist = Vector3Length(look);
    Vector3 fwd{look.x, 0.0f, look.z};
    fwd = Vector3Length(fwd) > 0.001f ? Vector3Normalize(fwd) : Vector3{0.0f, 0.0f, -1.0f};
    const float half = std::clamp(camDist * 1.35f, 40.0f, 120.0f);
    Vector3 center = Vector3Add(cam.target, Vector3Scale(fwd, camDist * 0.3f));
    center.y = 0.0f;
    // Achsen der Sonnenkamera: sun = Blickrichtung, right/up = waagerecht
    // und senkrecht in der Karte
    const Vector3 sun = g_assets.SunDirection();
    const Vector3 right = Vector3Normalize(Vector3CrossProduct(sun, {0.0f, 1.0f, 0.0f}));
    const Vector3 up = Vector3CrossProduct(right, sun);
    // Mittelpunkt auf ganze Bildpunkte der Karte runden. Sonst liegen die
    // Bildpunkte bei jeder Bewegung etwas anders, und die Schattenkanten
    // flimmern.
    const float texel = 2.0f * half / static_cast<float>(size);
    const float a = Vector3DotProduct(center, right), b = Vector3DotProduct(center, up);
    center = Vector3Add(center, Vector3Scale(right, roundf(a / texel) * texel - a));
    center = Vector3Add(center, Vector3Scale(up, roundf(b / texel) * texel - b));
    const Vector3 eye = Vector3Subtract(center, Vector3Scale(sun, 150.0f));
    const Matrix view = MatrixLookAt(eye, center, up);
    const Matrix proj = MatrixOrtho(-half, half, -half, half, 1.0, 300.0);

    prevProj_ = rlGetMatrixProjection();
    prevView_ = rlGetMatrixModelview();
    // Die Karte darf nicht gleichzeitig gelesen und beschrieben werden:
    // Texturplatz leeren, solange in sie gezeichnet wird
    rlActiveTextureSlot(kShadowSlot);
    rlDisableTexture();
    rlActiveTextureSlot(0);
    rlEnableFramebuffer(shadowFbo_);
    rlViewport(0, 0, size, size);
    rlClearColor(255, 255, 255, 255);
    rlClearScreenBuffers();
    rlSetMatrixProjection(proj);
    rlSetMatrixModelview(view);
    lightVP_ = MatrixMultiply(view, proj);
    return true;
}

void EndShadowPass() {
    rlDrawRenderBatchActive();
    RestoreTarget();
    rlSetMatrixProjection(prevProj_);
    rlSetMatrixModelview(prevView_);
    rlActiveTextureSlot(kShadowSlot);
    rlEnableTexture(shadowDepth_);
    rlActiveTextureSlot(0);
    g_assets.SetShadow(lightVP_, kShadowSlot, kShadowStrength, 1.0f / static_cast<float>(shadowSize_));
}

// Radien aus der Stärke a (0..1): bei 10 % bleibt fast alles sichtbar
// (klar bis ca. 55 m, Staub erst ab ca. 120 m), bei 100 % ist nur ein Kreis
// von 14 m um das Schiff klar und ab 40 m alles im Staub.
void SetFog(Vector3 center, float amount, float time) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    fogOn_ = a > 0.0f;
    fogCenter_ = center;
    const float clear = Lerp(60.0f, 14.0f, a);
    const float full = clear + Lerp(70.0f, 26.0f, a);
    fogParams_ = fogOn_ ? Vector4{clear, full, 0.5f + 0.5f * a, time} : Vector4{0.0f, 0.0f, 0.0f, 0.0f};
    const Color c = SandColor();
    g_assets.SetFog(center, Vector3{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f}, fogParams_);
    if (fogOn_ && LoadEffectOnce()) {
        SetShaderValue(effect_, locFogCenter_, &fogCenter_, SHADER_UNIFORM_VEC3);
        SetShaderValue(effect_, locFogParams_, &fogParams_, SHADER_UNIFORM_VEC4);
    }
}

// Wie FogAmount im Shader, nur ohne die Schwaden.
float Visibility(Vector3 p) {
    if (!fogOn_) return 1.0f;
    const float d = Dist2D(p, fogCenter_);
    const float t = std::clamp((d - fogParams_.x) / (fogParams_.y - fogParams_.x), 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t) * fogParams_.z;
}

// Staub: auf dem grauen Mond ein warmes Graubraun, auf dem blauen ein
// bläuliches Grau, auf dem orangefarbenen ein rötliches Braun.
Color SandColor() {
    switch (g_assets.GetMoonColor()) {
        case MoonColor::Blue: return Color{118, 128, 152, 255};
        case MoonColor::Orange: return Color{196, 136, 96, 255};
        default: return Color{150, 141, 128, 255};
    }
}

void BeginEffects() {
    if (!fogOn_ || !LoadEffectOnce()) return;
    BeginShaderMode(effect_);
    effectsActive_ = true;
}

void EndEffects() {
    if (!effectsActive_) return;
    EndShaderMode();
    effectsActive_ = false;
}

void SetScreenFlash(float v) { screenFlash_ = std::clamp(v, 0.0f, 1.0f); }
float ScreenFlash() { return screenFlash_; }

void Shutdown() {
    UnloadShadowMap();
    if (effectOk_) UnloadShader(effect_);
    effectOk_ = false;
    effectTried_ = false;
}

} // namespace fx
