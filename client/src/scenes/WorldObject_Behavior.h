#pragma once
#include <cstdint>

// Comportamiento por mapa y tipo de un objeto del mundo (la segunda mitad de
// MoveObjects, IDA 0x004FDC00 desde LABEL_38).  `o` es la dirección del OBJECT.
void WorldObject_PerMapTick(std::uintptr_t o);
