// Umsetzung von pl_mpeg: Die Bibliothek besteht nur aus pl_mpeg.h; der Code
// entsteht dort, wo PL_MPEG_IMPLEMENTATION gesetzt ist – genau einmal, hier.
// Eigene Übersetzungseinheit (als C), damit die Warnungen des Spiels
// (-Wall -Wextra) nicht für fremden Code gelten. Benutzt von src/ClipPlayer.cpp.
// pl_mpeg.h selbst bindet nur <stdint.h> ein, braucht aber size_t und FILE.
#include <stddef.h>
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
