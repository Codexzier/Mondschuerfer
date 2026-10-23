// ---------------------------------------------------------------------------
// ClipPlayer.cpp – Videoclips der Funksprüche abspielen (Schnittstelle:
// ClipPlayer.hpp).
//
// Ablauf eines Clips:
//   1. Open(): Die Datei wird zweimal geöffnet. Der erste Decoder liest nur
//      den Ton und entpackt ihn sofort ganz (die Clips sind wenige Sekunden
//      lang, 16 kHz: unter 1 MB) in einen raylib-Sound. Der zweite liest nur
//      das Bild und bleibt offen. So braucht der Ton keinen Stream, der
//      jedes Frame nachgefüllt werden müsste.
//   2. Update(): Sound starten, eigene Uhr t_ weiterzählen und so viele
//      Bilder entpacken, wie bis t_ fällig sind (Bildnummer / Bildrate).
//      Nur das letzte davon wird in RGB umgerechnet und in die Textur
//      geladen. Bild und Ton laufen ab demselben Augenblick und halten
//      gemeinsam an (Pause, Andocken).
//   3. Stop(): bei einem neuen Funkspruch oder wenn keiner mehr läuft.
// ---------------------------------------------------------------------------
#include "ClipPlayer.hpp"
#include "Config.hpp"
#include <cstddef>
#include <cstdio>   // vor pl_mpeg.h: es braucht size_t und FILE
#include "pl_mpeg.h"

ClipPlayer g_clip;

void ClipPlayer::Update(float dt, int cue, bool show) {
    if (cue != cue_) {
        Stop();
        cue_ = cue;
    }
    if (cue_ < 0 || cue_ >= cfg::MISSION_TYPE_COUNT * 3) return;
    if (!show) {
        if (hasSound_ && !paused_) PauseSound(sound_);
        paused_ = true;
        return;
    }
    if (!tried_) {
        tried_ = true;
        const char* file = cfg::MISSIONS[cue_ / 3].clip[cue_ % 3];
        if (file[0] != '\0') Open(file);
        if (hasSound_) PlaySound(sound_);
    } else if (paused_ && hasSound_) {
        ResumeSound(sound_);   // nach dem Ende des Tons ohne Wirkung
    }
    paused_ = false;
    if (!plm_ || ended_) return;

    t_ += dt;
    plm_frame_t* last = nullptr;
    while (static_cast<double>(frames_) / fps_ <= t_) {
        plm_frame_t* f = plm_decode_video(plm_);
        if (!f) { ended_ = true; break; }
        last = f;
        ++frames_;
    }
    if (last) {
        plm_frame_to_rgb(last, rgb_.data(), tex_.width * 3);
        UpdateTexture(tex_, rgb_.data());
        hasFrame_ = true;
    }
}

void ClipPlayer::Open(const std::string& file) {
    const std::string path = dir_ + file;
    if (!FileExists(path.c_str())) {
        TraceLog(LOG_WARNING, "CLIP: %s fehlt, nur Untertitel", path.c_str());
        return;
    }

    // Ton: ganz entpacken (pl_mpeg liefert immer Stereo, als float)
    if (IsAudioDeviceReady()) {
        if (plm_t* a = plm_create_with_filename(path.c_str())) {
            plm_set_video_enabled(a, 0);
            const int rate = plm_get_samplerate(a);
            std::vector<float> pcm;
            while (plm_samples_t* smp = plm_decode_audio(a))
                pcm.insert(pcm.end(), smp->interleaved, smp->interleaved + smp->count * 2);
            plm_destroy(a);
            if (rate > 0 && !pcm.empty()) {
                Wave wave{static_cast<unsigned int>(pcm.size() / 2), static_cast<unsigned int>(rate), 32, 2, pcm.data()};
                sound_ = LoadSoundFromWave(wave);   // kopiert die Daten
                hasSound_ = IsSoundValid(sound_);
            }
        }
    }

    // Bild: Decoder bleibt offen, Textur in Clipgröße
    plm_ = plm_create_with_filename(path.c_str());
    if (!plm_) return;
    plm_set_audio_enabled(plm_, 0);
    const int w = plm_get_width(plm_), h = plm_get_height(plm_);
    if (w <= 0 || h <= 0) {
        TraceLog(LOG_WARNING, "CLIP: %s hat kein lesbares Bild (nur MPEG-1)", path.c_str());
        plm_destroy(plm_);
        plm_ = nullptr;
        return;
    }
    if (plm_get_framerate(plm_) > 0.0) fps_ = plm_get_framerate(plm_);
    rgb_.assign(static_cast<size_t>(w) * h * 3, 0);
    Image img{rgb_.data(), w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8};
    tex_ = LoadTextureFromImage(img);
    SetTextureFilter(tex_, TEXTURE_FILTER_BILINEAR);
}

void ClipPlayer::Stop() {
    if (plm_) plm_destroy(plm_);
    if (tex_.id != 0) UnloadTexture(tex_);
    if (hasSound_) {
        StopSound(sound_);
        UnloadSound(sound_);
    }
    plm_ = nullptr;
    tex_ = Texture2D{};
    sound_ = Sound{};
    hasSound_ = hasFrame_ = ended_ = paused_ = tried_ = false;
    t_ = 0.0;
    fps_ = 30.0;
    frames_ = 0;
    cue_ = -1;
}
