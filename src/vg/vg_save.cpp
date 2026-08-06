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
    // WHAT `reserved` WAS FOR, AND IT WAS EXACTLY THE RIGHT SIZE.
    //
    // Three bytes, and a callsign is three characters. So the inherited Phantom's
    // name fits without growing the record and WITHOUT a version bump, which means
    // every existing save still loads.
    //
    // An old save has zeroes here, and zero is not a letter -- the load checks for
    // A-Z and treats anything else as "nobody has inherited the name". That is the
    // right answer for an old save and for a fresh one alike.
    char     phantom[3];
};

// THE COMMENT ON `reserved` USED TO SAY "keeps the record 16 bytes", AND IT WAS
// WRONG. The record is 20. It was 16 before vol_music and vol_sfx were added, and
// those two bytes pushed it to 18 and then the alignment to 20 -- without anybody
// touching `reserved` or the sentence describing it. Which is the same failure this
// table is about: a field added on one side, everything still compiling, and a
// statement about the layout quietly becoming false.
//
// So it is asserted now rather than described. If this fires, the record grew, and
// growing it means bumping SAVE_VERSION -- a record of a different size read as if
// it were this one is the "callsign of random bytes" the note above is about.
static_assert(sizeof(SaveRecord) == 20, "record grew: bump SAVE_VERSION too");

// ONE ROW PER PERSISTED FIELD, AND BOTH DIRECTIONS COME FROM IT.
//
// The hazard this removes is adding a field to one side only. Load and store were
// two independent lists of assignments in opposite directions, and nothing
// anywhere related them -- a new setting that got saved but never loaded, or
// loaded but never saved, compiles clean, runs clean, and simply does not persist.
// That is not a bug anybody finds by reading, because both functions look right on
// their own.
//
// The plan for this phase said to share one struct with VgGame and memcpy it. That
// would be worse in two ways. The CLAMPS ARE NOT BOILERPLATE: storage is the one
// input the game cannot vouch for, and the note above is about a corrupted byte
// indexing the ship table out of bounds on the first frame -- a memcpy has nowhere
// to put that. And the representations differ ON PURPOSE, floats in play against
// quantised bytes in flash, because the record is sixteen bytes by design and a
// volume slider does not need thirty-two bits.
//
// So the fields stay converted and clamped, and what becomes single is the LIST.
// Each row is the field, how it packs, and how it unpacks -- side by side, where
// leaving one out is visible.
#define VG_PROFILE_FIELDS(F)                                                       F(credits,                                                                       r.credits = (uint16_t)((vg.credits < 0) ? 0 : vg.credits),                     vg.credits = (int)r.credits;                                                   if (vg.credits > CREDIT_CAP) vg.credits = CREDIT_CAP;                          if (vg.credits < 0)          vg.credits = 0)                                 F(callsign,                                                                      memcpy(r.callsign, vg.callsign, 4),                                            for (int i = 0; i < 3; i++) {                                                      const char c = r.callsign[i];                                                  vg.callsign[i] = (c >= 'A' && c <= 'Z') ? c : 'A';                         }                                                                              vg.callsign[3] = 0)                                                          F(ship,                                                                          r.ship = (uint8_t)vg.ship,                                                     vg.ship = (r.ship < SHIP_CLASSES) ? (ShipClass)r.ship : SHIP_AEGIS;            vg.spec = vg_spec(vg.ship))                                                  F(champion,                                                                      r.champion = vg.champion ? 1u : 0u,                                            vg.champion = (r.champion != 0))                                             F(hue,                                                                           { float h = vg.trail_hue - (float)(int)vg.trail_hue;                             if (h < 0.0f) h += 1.0f;                                                       r.hue = (uint8_t)(h * 255.0f); },                                            vg.trail_hue = (float)r.hue * (1.0f / 255.0f))                               F(vol_music,                                                                     r.vol_music = (uint8_t)(vg.vol_music * 255.0f + 0.5f),                         vg.vol_music = (float)r.vol_music * (1.0f / 255.0f))                         F(phantom_tag,                                                                   memcpy(r.phantom, vg.phantom_tag, 3),                                          { bool ok = true;                                                                for (int i = 0; i < 3; i++)                                                        if (r.phantom[i] < 'A' || r.phantom[i] > 'Z') ok = false;                    if (ok) { memcpy(vg.phantom_tag, r.phantom, 3); vg.phantom_tag[3] = 0; }       else    { vg.phantom_tag[0] = 0; } })                                        F(vol_sfx,                                                                       r.vol_sfx = (uint8_t)(vg.vol_sfx * 255.0f + 0.5f),                             vg.vol_sfx = (float)r.vol_sfx * (1.0f / 255.0f))

void vg_save_load(void) {
    SaveRecord r;
    if (!vg_store_load(&r, sizeof(r)))            return;
    if (r.magic != SAVE_MAGIC)                    return;
    if (r.version != SAVE_VERSION)                return;

    // Clamped on the way in, per field, by the third column of the table above.
#define VG_PROFILE_UNPACK(name, pack, unpack) unpack;
    VG_PROFILE_FIELDS(VG_PROFILE_UNPACK)
#undef VG_PROFILE_UNPACK

    // Hull follows the class, or a CHARIOT saved last session would come back
    // carrying an AEGIS-sized bar. Derived rather than persisted, which is why it
    // is here and not a row in the table.
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

    r.magic   = SAVE_MAGIC;
    r.version = SAVE_VERSION;

#define VG_PROFILE_PACK(name, pack, unpack) pack;
    VG_PROFILE_FIELDS(VG_PROFILE_PACK)
#undef VG_PROFILE_PACK

    vg_store_save(&r, sizeof(r));
}
