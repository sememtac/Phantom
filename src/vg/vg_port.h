#pragma once
#include <stdint.h>

// ===========================================================================
// PORTING SEAM
//
// This is the ONLY interface between the game and the host firmware. Every
// hardware touch -- panel, touchscreen, IMU -- goes through these functions.
// No other vg_*.cpp file includes a driver header.
//
// Standalone builds use vg_port_co5300.cpp, which drives the panel over raw
// QSPI and reads the CST9220 / QMI8658 via SensorLib.
//
// To embed the game in another firmware: drop vg_port_co5300.cpp from the build
// and add your own vg_port_*.cpp implementing these same functions on top of
// whatever display / touch / IMU layer that firmware already has. Nothing else
// in the game changes.
//
// Note for such a port: a UI-oriented IMU layer often exposes only a rotation
// quadrant at ~10 Hz, which is far too coarse to fly with. The port needs a raw
// accelerometer read at >=100 Hz (see vg_imu_read below).
// ===========================================================================

// ---- Panel -----------------------------------------------------------------

bool vg_panel_init(void);

// Blit a full-width band of `h` rows at row `y`. Pixels are RGB565, row-major,
// SCR_W pixels per row. Called NUM_BANDS times per frame.
//
// ASYNCHRONOUS: this queues the DMA and returns while it is still in flight, so
// the caller can rasterise the next band into its *other* buffer meanwhile.
// Contract:
//   - `pixels` must stay untouched until a later push_band or vg_panel_wait
//     returns; the implementation blocks on the previous transfer before
//     starting a new one, so alternating two buffers is safe.
//   - Callers must vg_panel_wait() before reusing the buffer of the last band
//     pushed.
void vg_panel_push_band(int y, int h, const uint16_t* pixels);

// Block until any queued band transfer has completed.
void vg_panel_wait(void);

void vg_panel_brightness(uint8_t level);

// ---- Touch -----------------------------------------------------------------

#define VG_MAX_TOUCH 5

bool vg_touch_init(void);

// Fills xs/ys with up to VG_MAX_TOUCH active points in PANEL coordinates
// (origin top-left, +x right, +y down) and returns the count. The game needs
// more than one point so the throttle can be held while firing.
int  vg_touch_read(uint16_t* xs, uint16_t* ys);

// ---- Buttons ---------------------------------------------------------------

#define VG_BTN_A 0x01   // GPIO 0  (BOOT)
#define VG_BTN_B 0x02   // GPIO 18

void vg_buttons_init(void);

// Bitmask of buttons currently held. The hardware is active-low; that is
// normalised here so nothing above the seam has to know.
uint8_t vg_buttons_read(void);

// The PWR key, which is not a GPIO -- it belongs to the AXP2101 and is only
// visible through that chip's interrupt registers over I2C. False if the PMU
// does not answer, in which case the game simply never sees the key.
bool vg_pmu_init(void);
// True once per short press of PWR. Latched, so a press is never missed for
// having happened between polls.
bool vg_pmu_pwr_pressed(void);
// Every PMU interrupt bit seen since boot -- diagnostics only.
void vg_pmu_seen(uint8_t* st3);
// One-shot boot dump: bus scan, chip id, interrupt enables and status.
void vg_pmu_dump(void);

// ---- Persistent storage ----------------------------------------------------
//
// One small blob, deliberately not a key/value API. The game has exactly one
// record worth keeping, and a blob keeps versioning in a single place instead
// of scattering it across a dozen key names that then have to agree.
//
// All three may fail without consequence: a board with no usable storage simply
// forgets between power cycles, which is worse than persisting but far better
// than refusing to boot.
bool vg_store_init(void);
bool vg_store_load(void* data, unsigned len);   // false if nothing stored yet
bool vg_store_save(const void* data, unsigned len);

// A SECOND, separate blob, for diagnostics only. Deliberately not part of the
// save: a crash record is written by code that has just established something
// went wrong, and it must not be able to take the player's progress with it.
bool vg_store_diag_load(void* data, unsigned len);
bool vg_store_diag_save(const void* data, unsigned len);

// ---- IMU -------------------------------------------------------------------

bool vg_imu_init(void);

// Raw accelerometer in g. Returns false if no new sample is available; the
// caller keeps its previous value in that case.
bool vg_imu_read(float* ax, float* ay, float* az);
