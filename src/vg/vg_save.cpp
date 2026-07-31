#include "vg_save.h"
#include "vg_game.h"
#include "vg_port.h"
#include "vg_replay.h"
#include <Arduino.h>
#include <string.h>

// The stored record. Packed by hand rather than by dumping VgGame: that struct
// holds asteroid arrays and trail buffers and changes every other commit, and
// writing it wholesale would both waste flash and invalidate every save on any
// unrelated edit.
//
// Magic and version are what make a bad read harmless. Without them a save from
// an older layout is read as garbage and the player arrives at the menu with a
// callsign of random bytes and a five-figure bank.
#define SAVE_MAGIC   0x504E544Du   // 'PNTM'
// 2 adds the mix. Bumping this DISCARDS an older save rather than trying to read
// it, which is what the version field is for -- a record of a different size read
// as if it were this one is exactly the "callsign of random bytes" the note above
// is about.
#define SAVE_VERSION 2

struct SaveRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t credits;
    char     callsign[4];
    uint8_t  ship;
    uint8_t  champion;
    uint8_t  hue;         // trail colour, quantised to 0..255
    uint8_t  vol_music;   // 0..255
    uint8_t  vol_sfx;
    uint8_t  reserved[3]; // keeps the record 16 bytes and leaves room to grow
};

void vg_save_load(void) {
    SaveRecord r;
    if (!vg_store_load(&r, sizeof(r)))            return;
    if (r.magic != SAVE_MAGIC)                    return;
    if (r.version != SAVE_VERSION)                return;

    // Clamp everything on the way in. Storage is the one input the game cannot
    // vouch for -- a corrupted byte here would otherwise index the ship table
    // out of bounds on the very first frame.
    vg.credits = (int)r.credits;
    if (vg.credits > CREDIT_CAP) vg.credits = CREDIT_CAP;
    if (vg.credits < 0)          vg.credits = 0;

    for (int i = 0; i < 3; i++) {
        const char c = r.callsign[i];
        vg.callsign[i] = (c >= 'A' && c <= 'Z') ? c : 'A';
    }
    vg.callsign[3] = 0;

    vg.ship      = (r.ship < SHIP_CLASSES) ? (ShipClass)r.ship : SHIP_AEGIS;
    vg.spec      = vg_spec(vg.ship);
    vg.champion  = (r.champion != 0);
    vg.trail_hue = (float)r.hue * (1.0f / 255.0f);
    vg.vol_music = (float)r.vol_music * (1.0f / 255.0f);
    vg.vol_sfx   = (float)r.vol_sfx   * (1.0f / 255.0f);

    // Hull follows the class, or a CHARIOT saved last session would come back
    // carrying an AEGIS-sized bar.
    vg.health_max = vg.spec->hull;
    vg.health     = vg.health_max;

    Serial.printf("vg_save_load: %s  %d CR  %s%s\n",
                  vg.callsign, vg.credits, vg.spec->name,
                  vg.champion ? "  CHAMPION" : "");
}

void vg_save_store(void) {
    // A replay re-runs someone's session, including whatever they won or spent.
    // Letting it write would mean rendering a recording could overwrite the
    // progress of the person who recorded it.
    if (vg_replay_suppress_save()) return;

    SaveRecord r;
    memset(&r, 0, sizeof(r));

    r.magic    = SAVE_MAGIC;
    r.version  = SAVE_VERSION;
    r.credits  = (uint16_t)((vg.credits < 0) ? 0 : vg.credits);
    r.ship     = (uint8_t)vg.ship;
    r.champion = vg.champion ? 1u : 0u;

    float h = vg.trail_hue - (float)(int)vg.trail_hue;
    if (h < 0.0f) h += 1.0f;
    r.hue = (uint8_t)(h * 255.0f);

    r.vol_music = (uint8_t)(vg.vol_music * 255.0f + 0.5f);
    r.vol_sfx   = (uint8_t)(vg.vol_sfx   * 255.0f + 0.5f);

    memcpy(r.callsign, vg.callsign, 4);

    vg_store_save(&r, sizeof(r));
}
