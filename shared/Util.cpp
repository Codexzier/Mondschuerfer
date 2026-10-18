// ---------------------------------------------------------------------------
// Util.cpp – Umsetzung der Hilfsfunktionen aus Util.hpp (Text formatieren,
// Winkeldifferenz, Kleinschreibung).
//
// Wird in Client, Server und Lasttest mitgebaut (Teil von shared/). Braucht
// nur die C/C++-Standardbibliothek und die Konstante PI aus raylib.h, keine
// raylib-Funktionen. Die kurzen Funktionen (ForwardFromYaw, YawTowards,
// Dist2D) stehen als inline direkt im Header.
// ---------------------------------------------------------------------------
#include "Util.hpp"
#include <cstdarg>
#include <cstdio>
#include <vector>

// Variable Argumentliste ("..."): Die Werte werden über va_list an vsnprintf
// weitergereicht. Eine va_list darf man nur EINMAL durchlaufen. Weil
// vsnprintf eventuell zweimal gebraucht wird, legt va_copy vorher eine Kopie
// an. Jedes va_start/va_copy braucht ein passendes va_end, auch in den
// frühen Rücksprüngen.
std::string Fmt(const char* fmt, ...) {
    // Erst mit einem kleinen Puffer versuchen; vsnprintf meldet die nötige Länge,
    // falls der Text nicht hineinpasst. Dann ein zweites Mal mit passendem Puffer.
    // Fast alle Texte im Spiel sind kürzer als 256 Zeichen; der Puffer liegt
    // auf dem Stack, also kostet der Normalfall keine Speicheranforderung.
    char small[256];
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(small, sizeof(small), fmt, args);   // Länge OHNE abschließende 0
    va_end(args);
    if (needed < 0) {
        // Fehler im Formattext (sehr selten): leerer Text statt Absturz
        va_end(copy);
        return std::string();
    }
    if (needed < static_cast<int>(sizeof(small))) {
        va_end(copy);
        return std::string(small, static_cast<size_t>(needed));
    }
    // Zu lang: Puffer in genau passender Größe (+1 für die abschließende 0)
    std::vector<char> big(static_cast<size_t>(needed) + 1);
    vsnprintf(big.data(), big.size(), fmt, copy);
    va_end(copy);
    return std::string(big.data(), static_cast<size_t>(needed));
}

// Rechnung: b - a um PI verschieben, mit fmodf in den Bereich einer vollen
// Umdrehung (0..2*PI) bringen und wieder um PI zurückschieben. fmodf kann bei
// negativen Zahlen ein negatives Ergebnis liefern, deshalb die Korrektur.
// Beispiel: a = 170°, b = -170° -> b - a = -340° -> Ergebnis +20°.
float AngleDiff(float a, float b) {
    float d = fmodf(b - a + PI, 2.0f * PI);
    if (d < 0) d += 2.0f * PI;
    return d - PI;
}

// Bewusst ohne std::tolower: Das hängt von der eingestellten Sprache
// ("locale") des Rechners ab. Für Kontonamen muss das Ergebnis aber auf
// jedem Client und Server gleich sein (siehe Hinweis in Util.hpp).
std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}
