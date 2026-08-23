#pragma once
#include <stdint.h>
// The hardware RNG. Seeded from the clock here, which is enough for a session
// seed -- a replay supplies its own numbers and never reaches this.
uint32_t esp_random(void);
