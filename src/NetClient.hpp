#pragma once
// ---------------------------------------------------------------------------
// NetClient: Verbindung des Spiels zum Mehrspieler-Server.
//
// Aufgaben:
//   - Verbindung über ENet aufbauen und halten (Zustände siehe NetState).
//   - Anmeldung: Konto anlegen oder anmelden (Challenge-Response, Crypto.hpp),
//     oder als Zuschauer beitreten.
//   - Eine "Spiegelwelt" (World mit isMirror = true) aus den Nachrichten des
//     Servers füllen, damit Render.cpp und Hud.cpp sie wie im Einzelspieler
//     zeichnen können:
//       * Kulisse (Mondoberfläche, Stationen) selbst erzeugen: gleicher Seed
//         und gleiche Einstellungen wie der Server (World::BuildMirror).
//       * Fremde Schiffe interpolieren: Sie werden INTERP_DELAY Sekunden
//         "in der Vergangenheit" zwischen zwei Snapshots weich bewegt.
//       * Eigenes Schiff vorhersagen ("Client-side prediction"): Eingaben
//         wirken sofort lokal mit World::UpdateShipPhysics. Kommt ein
//         Snapshot, wird auf den Serverzustand zurückgesetzt und alle noch
//         nicht bestätigten Eingaben werden erneut angewendet
//         ("Reconciliation"). So fühlt sich die Steuerung direkt an, obwohl
//         der Server bestimmt, was wirklich passiert.
//   - Ereignisse (Töne, Explosionen, Meldungen, Ziel erreicht) an das Spiel
//     weitergeben.
//   - Die aktive Karte des Servers (MapData) in die Spiegelwelt setzen.
//   - Chat: Nachrichten und "tippt gerade" schicken, empfangene Zeilen
//     weitergeben (das Chatfenster selbst ist src/Chat.hpp).
//   - Karten hochladen (Karteneditor, UploadMap).
//
// Benutzt von: src/Multiplayer.cpp (Menüs, Anzeige), src/MapEditor.cpp
// ("An Server senden"). Abhängigkeiten: ENet,
// shared/Protocol.hpp, shared/Crypto.hpp, shared/World.hpp.
// ---------------------------------------------------------------------------
#include "Protocol.hpp"
#include "World.hpp"
#include <memory>
#include <string>
#include <vector>

enum class NetState {
    Disconnected,   // keine Verbindung (Anfangszustand, nach Trennung oder Fehler)
    Connecting,     // Verbindungsaufbau läuft (höchstens ~5 Sekunden)
    Lobby,          // verbunden, ServerInfo empfangen, noch nicht angemeldet
    LoggingIn,      // Anmeldung/Registrierung läuft
    Playing,        // angemeldet als Spieler
    Spectating,     // Zuschauer
};

class NetClient {
public:
    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // --- Verbindung ------------------------------------------------------
    // address: "host" oder "host:port" (Standardport net::DEFAULT_PORT).
    // Liefert false bei ungültiger Adresse (dann steht der Grund in TakeError()).
    bool Connect(const std::string& address);
    void Disconnect();                    // meldet sich ab (Leave) und trennt
    NetState State() const;
    std::string TakeError();              // letzter Fehlertext, "" wenn keiner; wird geleert
    bool HasInfo() const;                 // ServerInfo empfangen?
    const net::ServerInfoMsg& Info() const;
    float PingMs() const;

    // --- Anmeldung (nur im Zustand Lobby) ---------------------------------
    // Das Passwort wird nur lokal zur Schlüsselberechnung verwendet.
    void Register(const std::string& name, const std::string& password);
    void Login(const std::string& name, const std::string& password);
    void Spectate();
    // Karte hochladen (Karteneditor): wie Login, aber statt anzumelden wird die
    // Karte geschickt. Fehler (falsches Passwort ...) kommen über TakeError();
    // die Antwort des Servers über TakeUploadResult. Danach Disconnect().
    void UploadMap(const std::string& name, const std::string& password, const GameMap& map);
    // true einmalig, wenn die Antwort auf UploadMap da ist
    bool TakeUploadResult(bool& ok, std::string& text);

    // --- Jedes Frame ---------------------------------------------------------
    // held: gehaltene Tasten (Schub, Drehen, Feuer). scanPressed/interactPressed:
    // in diesem Frame gedrückt (E / F). Arbeitet Netzwerknachrichten ab,
    // schickt Eingaben (30/s), sagt das eigene Schiff voraus, interpoliert die
    // anderen und aktualisiert Partikel in der Spiegelwelt.
    // dt = echte Bildzeit, NICHT auf 1/20 s gekürzt: Die Eingabeuhr muss genau
    // 30 Eingaben pro Sekunde erzeugen, so viele, wie der Server verbraucht.
    void Update(float dt, const ShipInput& held, bool scanPressed, bool interactPressed);

    // Stationsfenster: Anfrage an den Server
    void SendDockAction(net::DockActionType action, int param = 0);

    // Chat (nur als Spieler): Nachricht schicken bzw. melden, ob die
    // Chat-Eingabe offen ist (Sprechblase am eigenen Schiff für die anderen).
    void SendChat(const std::string& text);
    void SendChatTyping(bool typing);

    // --- Spielwelt ---------------------------------------------------------------
    bool WorldReady() const;              // Welcome empfangen, Welt aufgebaut
    World& GetWorld();
    const World& GetWorld() const;
    const net::WelcomeMsg& Welcome() const;
    int MyPlayerId() const;               // -1 für Zuschauer
    const std::string& MyName() const;

    // --- Ranglisten -----------------------------------------------------------
    const std::vector<net::PlayerEntry>& Players() const;       // alle angemeldeten Spieler
    const std::vector<net::HighscoreEntry>& Highscores() const; // kürzeste Zeit zuerst
    long long TotalDelivered() const;                           // alle Spieler zusammen
    int Spectators() const;

    // --- Ereignisse (werden beim Abholen geleert) ------------------------------
    // Töne mit Position (eigene Schüsse sind schon lokal vorhergesagt enthalten)
    std::vector<SoundEvent> TakeSounds();

    struct GoalNotice {
        bool mine = false;      // true: ich habe das Ziel erreicht
        std::string name;
        float seconds = 0.0f;
        int round = 1;
    };
    std::vector<GoalNotice> TakeGoals();
    bool TakeRespawned();                 // true einmalig nach einem neuen eigenen Schiff
    std::vector<net::ChatLineMsg> TakeChat();   // neue Chat-Zeilen (älteste zuerst)
    bool TakeWorldRebuilt();              // true einmalig nach (erneutem) Welcome

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
