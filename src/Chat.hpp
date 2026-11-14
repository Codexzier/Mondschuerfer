#pragma once
// ---------------------------------------------------------------------------
// Chat.hpp – Chatfenster im Mehrspieler (Umsetzung: Chat.cpp).
//
// Wofür ist die Datei da?
//   Unten links liegt ein halb durchsichtiges schwarzes Fenster mit weißer
//   Schrift. Es zeigt die letzten Chat-Nachrichten (Spielername und Text),
//   höchstens fünf Zeilen. Bedienung:
//     - Enter öffnet die Eingabe. Das Fenster ist dann nur noch zu 20 %
//       durchsichtig, und die Flugsteuerung ruht (Tasten gehen in den Text).
//     - Enter schickt den Text ab und schließt die Eingabe. Ohne Text wird
//       nichts geschickt, die Eingabe geht nur zu. ESC bricht ab.
//     - Bei offener Eingabe blättert das Mausrad über dem Fenster im Verlauf
//       (sonst zoomt es wie immer).
//   Außerdem zeichnet DrawChatEmote die Sprechblase über einem Schiff, dessen
//   Spieler gerade tippt.
//   Das Command Center hat ein zweites, eigenes Chatfenster (Kanal Command,
//   unten in der Mitte). Dort bestätigt Enter die Knöpfe; die Eingabe öffnet
//   deshalb T oder ein Klick ins Fenster.
//
// Wer benutzt sie?
//   src/Multiplayer.cpp (MultiplayerScreen, Phase Playing). Senden und
//   Empfangen macht NetClient (SendChat, SendChatTyping, TakeChat); der
//   Server verteilt die Nachrichten an alle Spieler bzw. im Kanal Command an
//   alle im Command Center (Protocol.hpp, ChatLine).
//
// Abhängigkeiten: Ui.hpp (Texte, Textfeld), shared/Protocol.hpp (MAX_CHAT,
// erlaubte Zeichen), raylib.
// ---------------------------------------------------------------------------
#include "Ui.hpp"
#include "raylib.h"
#include <deque>
#include <string>

class ChatBox {
public:
    // openKey: Taste, die die Eingabe öffnet; hint: Hinweis in der
    // Eingabezeile, solange sie zu ist. Ist openKey nicht Enter (Command
    // Center), öffnet auch ein Klick ins Fenster die Eingabe.
    explicit ChatBox(int openKey = KEY_ENTER, const char* hint = "[Enter] Chat");

    // Neue Nachricht in den Verlauf. mine = eigene Nachricht (Name in Orange).
    void Add(const std::string& name, const std::string& text, bool mine);
    // Verlauf und Eingabe leeren (neue Verbindung)
    void Clear();

    // Jedes Frame VOR der Flugsteuerung (MultiplayerScreen::Update): Enter
    // (bzw. openKey) öffnet die Eingabe, ESC schließt sie. allowed = false (Pause,
    // Zielanimation, kein Spiel): Die Eingabe geht zu bzw. bleibt zu.
    void Update(bool allowed);
    // Eingabe offen? Dann darf das Spiel die Tastatur nicht auswerten.
    bool Active() const { return input_.focused; }
    // Offen oder in diesem Frame geöffnet/geschlossen: Die Taste, die das
    // getan hat (Enter, ESC), darf nicht noch etwas anderes auslösen
    // (Pause, Stationsfenster).
    bool Busy() const;
    // Soll das Mausrad den Verlauf blättern statt zu zoomen?
    bool WantsWheel() const;

    // Zeichnet das Fenster unten links. Liefert true, wenn Enter eine
    // Nachricht abgeschickt hat; der Text steht dann in send.
    bool Draw(std::string& send);
    // Dasselbe an anderer Stelle: linker Rand x, Unterkante bottomY, Breite
    // width (Pixel; die Höhe ist immer Height()).
    bool Draw(std::string& send, float x, float bottomY, float width);
    // Höhe des Fensters in Pixeln (die Tastenhilfe des HUD rückt darüber)
    static float Height();

private:
    struct Entry {
        std::string name, text;
        bool mine = false;
    };
    void Close();

    std::deque<Entry> entries_;      // älteste zuerst, höchstens MAX_ENTRIES
    ui::TextField input_;
    int openKey_ = KEY_ENTER;        // Taste zum Öffnen (Konstruktor)
    std::string hint_;               // Hinweis bei geschlossener Eingabe
    int scroll_ = 0;                 // so viele Zeilen vom neuesten Ende hochgeblättert
    int lastLines_ = 0;              // Zeilenzahl im vorigen Frame (neue Nachricht beim Blättern)
    float wheel_ = 0.0f;             // angefangene Mausrad-Rasten (Touchpads liefern Bruchteile)
    unsigned openedFrame_ = 0;       // ui::FrameIndex() beim Öffnen / Schließen
    unsigned closedFrame_ = 0;
    Rectangle rect_{};               // Fensterfläche im letzten Frame
};

// Sprechblase mit drei Punkten. at = Spitze der Blase unten (Pixel),
// time = laufende Zeit in Sekunden (die Punkte leuchten nacheinander auf).
void DrawChatEmote(Vector2 at, float time);
