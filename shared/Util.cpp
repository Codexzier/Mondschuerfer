#include "Util.hpp"
#include <cstdarg>
#include <cstdio>
#include <vector>

std::string Fmt(const char* fmt, ...) {
    // Erst mit einem kleinen Puffer versuchen; vsnprintf meldet die nötige Länge,
    // falls der Text nicht hineinpasst. Dann ein zweites Mal mit passendem Puffer.
    char small[256];
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(small, sizeof(small), fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(copy);
        return std::string();
    }
    if (needed < static_cast<int>(sizeof(small))) {
        va_end(copy);
        return std::string(small, static_cast<size_t>(needed));
    }
    std::vector<char> big(static_cast<size_t>(needed) + 1);
    vsnprintf(big.data(), big.size(), fmt, copy);
    va_end(copy);
    return std::string(big.data(), static_cast<size_t>(needed));
}

float AngleDiff(float a, float b) {
    float d = fmodf(b - a + PI, 2.0f * PI);
    if (d < 0) d += 2.0f * PI;
    return d - PI;
}

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}
