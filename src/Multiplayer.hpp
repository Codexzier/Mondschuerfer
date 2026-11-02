#pragma once
// ---------------------------------------------------------------------------
// MultiplayerScreen: alles, was der Spieler im Mehrspieler SIEHT und BEDIENT.
//
// Das Hauptmenü (main.cpp) erzeugt diese Klasse, sobald "Mehrspieler"
// gewählt wird, und reicht ihr ab dann jedes Frame Update, Zeichnen und Ton
// weiter. Wird sie zerstört (zurück ins Hauptmenü), trennt sie die
// Verbindung zum Server und stellt die Mondfarbe wieder auf Grau.
//
// Die eigentliche Netzwerkarbeit macht NetClient (src/NetClient.hpp): er
// baut die Verbindung auf, meldet an und füllt eine "Spiegelwelt" (World),
// die hier mit denselben Funktionen gezeichnet wird wie im Einzelspieler
// (World::Draw aus Render.cpp, DrawHud/DrawDockPanel aus Hud.cpp).
//
// Unterzustände (Phase), in dieser Reihenfolge durchlaufen:
//
//   Address     Serveradresse eingeben (vorbelegt aus g_clientConfig) und
//               verbinden. Fehler (Server nicht erreichbar ...) stehen in
//               der Statuszeile.
//      |  NetClient meldet NetState::Lobby (ServerInfo empfangen)
//      v
//   Lobby       Servername, Spieler online, Einstellungen des Servers.
//               Knöpfe: Zuschauen / Mitspielen / Trennen.
//      |-- Zuschauen --> Spectating: ganze Karte von oben, links die
//      |                 Spielerliste, rechts Gesamtmenge und Bestenliste.
//      |-- Mitspielen -> Login: Name + Passwort, "Anmelden" oder
//      |                 "Neues Konto anlegen".
//      |                    |  NetClient meldet NetState::Playing
//      |                    v
//      |                 Rollenwahl (Pilot oder SG-Team, die letzte Wahl ist
//      |                 vorausgewählt), dann
//      |                 Playing: Spiel wie im Einzelspieler, zusätzlich
//      |                 "Beste Piloten", Namen über fremden Schiffen,
//      |                 Anzeige nach Absturz, Abschlussanimation beim Ziel,
//      |                 Chat unten links (ChatBox, Chat.hpp).
//      |                 Pause (ESC) mit Weiterspielen / Optionen / Abmelden;
//      |                 die Optionen (OptionsScreen) liegen wie die Pause
//      |                 über dem Spiel, das auf dem Server weiterläuft.
//      |                 SG-Team: Command Center (CommandCenterScreen::
//      |                 DrawOnline: Teams gründen, beitreten, Tor anwählen),
//      |                 im Einsatz Ego-Ansicht zu Fuß mit dem HUD der Figur.
//
//   Verbindungsverlust in irgendeiner Phase -> zurück zu Address mit Fehlertext.
//
// Threads: Alles läuft im Spiel-Thread (Hauptschleife in main.cpp).
// NetClient arbeitet seine Netzwerkpakete innerhalb von NetClient::Update ab,
// das hier einmal pro Frame aufgerufen wird – es gibt keinen zweiten Thread.
//
// Musik: In Address, Lobby und Login läuft die Menümusik, in Playing und
// Spectating die Spielmusik (UpdateAudio, siehe Audio.hpp).
//
// Steuerung: Das eigene Schiff lenkt ein PilotControls-Objekt (Tastatur,
// Maus, Controller; src/Controls.hpp) – dieselbe Steuerung wie im
// Einzelspieler. Ihre Befehle gehen als Eingabepakete an den Server; das
// Netzwerkprotokoll ist dafür unverändert. Zu Fuß steuert TrooperControls
// (Maus und rechter Stick: Blick, der Mauszeiger ist dann eingefangen,
// CaptureCursor); die Eingaben gehen ebenfalls als Eingabepakete an den Server.
//
// Abhängigkeiten: NetClient, Chat, ClientConfig (das gemeinsame g_clientConfig),
// Options (Optionen in der Pause), Controls (Flugsteuerung), World
// (shared/), Ui, Hud, Audio, Assets (Mondfarbe), raylib.
// ---------------------------------------------------------------------------
#include "Chat.hpp"
#include "ClientConfig.hpp"
#include "CommandCenter.hpp"
#include "Controls.hpp"
#include "NetClient.hpp"
#include "Options.hpp"
#include "Ui.hpp"
#include "World.hpp"
#include <string>
#include <vector>

// Startoptionen von der Kommandozeile (für automatische Tests, siehe main.cpp).
// Im normalen Spiel sind alle leer: dann beginnt der Bildschirm bei "Address".
struct MultiplayerOptions {
    std::string connect;          // --mp-connect ADRESSE: sofort verbinden
    std::string name;             // --mp-name NAME
    std::string password;         // --mp-pass PASSWORT (nur für Tests!)
    bool registerAccount = false; // --mp-register: Konto anlegen (bei "Name vergeben" anmelden)
    bool spectate = false;        // --mp-spectate: direkt zuschauen
    bool autopilot = false;       // --mp-autopilot: Autopilot fliegt das eigene Schiff
};

class MultiplayerScreen {
public:
    explicit MultiplayerScreen(const MultiplayerOptions& opt = MultiplayerOptions{});
    ~MultiplayerScreen();
    MultiplayerScreen(const MultiplayerScreen&) = delete;
    MultiplayerScreen& operator=(const MultiplayerScreen&) = delete;

    // Einmal pro Frame, in dieser Reihenfolge (siehe App::Frame in main.cpp):
    void Update(float frameDt);   // Netzwerk, Eingaben, Kamera, Feuerwerk (frameDt ungekürzt, siehe main.cpp)
    void UpdateAudio(float dt);   // Töne der Spiegelwelt und Dauergeräusche
    void Draw();                  // zwischen BeginDrawing/ClearBackground und EndDrawing

    // true: der Spieler möchte zurück ins Hauptmenü (main.cpp zerstört uns dann)
    bool WantsMenu() const { return wantsMenu_; }

    // Taste M wurde gedrückt (main.cpp schaltet den Ton um, wir zeigen eine Meldung)
    void OnMuteToggled(bool muted);

    // Eine Zeile für --report (automatische Tests): Phase, Spieler, Kontostand ...
    std::string Report() const;

    // Zu Fuß im Einsatz (nicht in Pause und Chat): main.cpp fängt den Mauszeiger ein
    bool CaptureCursor() const;

private:
    enum class Phase { Address, Lobby, Login, Playing, Spectating };

    // --- Zustandswechsel ----------------------------------------------------
    void SetPhase(Phase p);
    void StartConnect();
    void BackToAddress(const std::string& reason);
    void EnterLobby();
    void EnterLogin();
    void EnterPlaying();
    void EnterSpectating();
    void SubmitLogin(bool registerAccount);
    void HandleNetwork();          // Fehler, Zustandsänderungen und Ereignisse von NetClient
    void SetStatus(const std::string& text, Color c);

    // --- Spielen --------------------------------------------------------------
    void ReadGameInput(float dt, ShipInput& held, bool& scan, bool& interact);
    void UpdateAutopilotDock(float dt);
    void UpdatePlayingCamera(float dt);
    bool ReadFootInput(float dt, TrooperInput& in);   // zu Fuß; false = keine Eingaben (Figur bleibt stehen)
    const Trooper* OwnTrooper() const;                // eigene Figur im Einsatz, sonst nullptr
    bool InCommand() const;                           // Rolle SG-Team ohne Figur: im Command Center
    void ChooseRole(int role);                        // 0 Pilot, 1 SG-Team: an den Server schicken
    void UpdateFireworks(float dt);
    void FireworkBurst(Vector3 center, float spread, int strength);

    // --- Zuschauen --------------------------------------------------------------
    void UpdateSpectatorCamera(float dt);
    Rectangle SpectatorMapArea() const;

    // --- Zeichnen ---------------------------------------------------------------
    void DrawBackdrop();           // Mondlandschaft hinter den Menüs
    void DrawAddress();
    void DrawLobby();
    void DrawLogin();
    void DrawPlaying();
    void DrawSpectating();
    void DrawTopPilots(const World& w);
    Rectangle TopPilotsRect() const;
    void DrawShipNames(const World& w);
    void DrawTeamMarkers();                 // Figuren der SG-Teams auf der Minikarte (Piloten)
    void DrawCommand();
    void DrawNoShip(const World& w);
    void DrawGoalAnimation();
    void DrawPause();
    void DrawPlayerList(Rectangle r);
    void DrawHighscores(Rectangle r);
    void DrawSpectatorMarkers(const World& w);
    void DrawNotices(const World& w, float top);

    // Name eines Spielers zu einem Schiff (leer, wenn unbekannt)
    std::string NameForShip(const Ship& s) const;

    MultiplayerOptions opt_;
    NetClient net_;
    // Serveradresse und Name stehen im gemeinsamen g_clientConfig (main.cpp
    // lädt es beim Start). Absichtlich keine eigene Kopie hier – Begründung
    // in ClientConfig.hpp.
    Phase phase_ = Phase::Address;
    int phaseFrames_ = 0;          // Frames seit dem letzten Phasenwechsel (siehe SetPhase)
    bool wantsMenu_ = false;
    float elapsed_ = 0.0f;

    // Menüs
    ui::TextField addressField_;
    ui::TextField nameField_;
    ui::TextField passwordField_;
    std::string status_;           // Statuszeile (Fehler oder Hinweis)
    Color statusColor_ = ui::MUTED;
    bool connecting_ = false;      // Verbindungsaufbau angestoßen, noch keine Lobby
    bool authPending_ = false;     // Anmeldung/Registrierung läuft
    bool authWasRegister_ = false;
    float authStartT_ = 0.0f;
    bool spectatePending_ = false; // "Zuschauen" angefragt, Welcome steht noch aus
    float spectateStartT_ = 0.0f;
    bool retryLogin_ = false;      // nach "Name vergeben" (Testablauf) anmelden, sobald wieder in der Lobby
    int lobbySel_ = 0;
    int pauseSel_ = 0;

    // automatische Testabläufe (--mp-...)
    bool autoConnectDone_ = false;
    bool autoActionDone_ = false;
    bool autoRegisterFallback_ = false;   // bei "Name vergeben" einmal mit Anmelden versuchen

    // Hintergrund für die Menüs: Mondlandschaft ohne Schiffe (World::BuildMirror)
    World backdrop_;
    float backdropOrbit_ = 0.0f;

    // Kamera
    Camera3D cam_{};
    Vector3 camTarget_{};
    float zoom_ = 1.1f;            // Spielen: Abstand der Kamera (Mausrad, rechter Stick)
    float padZoom_ = 0.0f;         // Zoom vom rechten Stick (in Mausrad-Rasten), von ReadGameInput an die Kamera
    bool snapCam_ = true;          // nächstes Frame direkt aufs Schiff springen (kein Schwenk)
    float specZoom_ = 1.0f;        // Zuschauer: 1 = ganze Karte, kleiner = näher
    Vector2 specCenter_{0, 0};     // Zuschauer: Kartenpunkt (x, z) in der Mitte der Kartenfläche
    bool dragging_ = false;
    bool specPointerWasDown_ = false;   // PointerDown im vorigen Frame (Beginn des Ziehens, siehe UpdateSpectatorCamera)
    int listScroll_ = 0;           // Zuschauer: erste sichtbare Zeile der Spielerliste

    // Spielen
    PilotControls controls_;       // Flugsteuerung: Tastatur, Maus, Controller (Controls.hpp)
    bool paused_ = false;
    int pauseFrames_ = 0;
    bool optionsOpen_ = false;     // in der Pause: Optionen statt Pausenmenü anzeigen
    OptionsScreen options_;
    int dockFrames_ = 0;           // Frames seit dem Andocken (gegen "F dockt sofort wieder ab")
    float undockSentT_ = -1.0f;    // > 0: Abdocken angefragt, Antwort des Servers steht aus
    float autoDockT_ = 0.0f;       // Autopilot: Zeit an der Station
    float autoDockNextAction_ = 0.0f;
    bool hadShip_ = false;         // schon einmal ein eigenes Schiff gesehen (für "zerstört")
    ChatBox chat_;                 // Chatfenster unten links
    bool chatTypingSent_ = false;  // zuletzt an den Server gemeldet: Chat-Eingabe offen

    // SG-Team
    bool roleChoice_ = false;      // Rollenwahl nach der Anmeldung offen
    int roleSel_ = 0;              // markierte Karte (0 Pilot, 1 SG-Team)
    int roleFrames_ = 0;           // Frames seit dem Öffnen der Rollenwahl
    bool inCommand_ = false;       // im Command Center (für OpenOnline beim Betreten)
    CommandCenterScreen command_;
    TrooperControls trooperControls_;   // Steuerung zu Fuß (Ego-Ansicht)
    int lookTrooper_ = -1;         // Figur, zu der lookYaw_/lookPitch_ gehören
    float lookYaw_ = 0.0f;         // Blickrichtung zu Fuß (Maus/rechter Stick)
    float lookPitch_ = 0.0f;

    // Abschlussanimation "Ziel erreicht"
    float goalAnimT_ = -1.0f;      // < 0: keine Animation
    float goalSeconds_ = 0.0f;
    int goalRound_ = 1;
    float burstT_ = 0.0f;          // Zeit bis zur nächsten Feuerwerksrakete
};
