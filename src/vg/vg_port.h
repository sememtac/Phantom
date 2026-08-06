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


// ---- Touch -----------------------------------------------------------------

#define VG_MAX_TOUCH 5

bool vg_touch_init(void);

// Fills xs/ys with up to VG_MAX_TOUCH active points in PANEL coordinates
// (origin top-left, +x right, +y down) and returns the count. The game needs
// more than one point so the throttle can be held while firing.
int  vg_touch_read(uint16_t* xs, uint16_t* ys);

// HOW OFTEN THE BUS WAS REFUSED, since boot and never reset.
//
// The I2C mutex is bounded now -- it was portMAX_DELAY, and one recorded crash is a task
// watchdog in the input poll with a 7999 ms worst frame beside it. A frame that cannot get
// the bus loses one frame of input instead of the game.
//
// So this has to be VISIBLE. A silent bounded wait is a dropped read nobody knows about,
// which is a worse diagnostic than the hang it replaced: if this climbs, something is
// holding the bus far longer than the sensor task's own reads ever do.
uint32_t vg_i2c_denied(void);

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
// Reads the PMU's interrupt status and latches a power-key press. MUST be called
// from whichever thread owns the I2C bus and from nowhere else: it switches the bus
// clock to 400 kHz and back, and the touch controller and IMU run it at 1 MHz. See
// the note at the definition for what happens when two cores do this at once.
void vg_pmu_poll(void);
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

// ---- Audio -----------------------------------------------------------------
//
// Mono 16-bit at VG_AUDIO_RATE. The game generates its own sound rather than
// playing files -- see vg_sfx.cpp -- so this seam carries samples and nothing
// else: no notion of a clip, a channel or a volume, all of which belong above it.
//
// NON-BLOCKING BY CONTRACT. vg_audio_write returns how many samples it actually
// took and the caller is expected to shrug at a short write. A port that blocked
// here would put the audio buffer on the critical path of the frame, which is the
// same mistake the serial link made and cost two minutes of frozen screen.
#define VG_AUDIO_RATE 22050

bool vg_audio_init(void);
int  vg_audio_write(const int16_t* samples, int n);
// How many samples the output has CONSUMED since the last call, which is how
// many the caller now owes it. Advances an internal clock, so call it once per
// pass and use what it returns.
//
// FOR THE INLINE PATH ONLY -- a replay or a capture, where the render thread owns
// the audio and must not wait on the codec. The audio task does not use this at
// all: it renders a fixed chunk and lets vg_audio_write_paced set the pace, which
// is a better clock than this one because it is the actual sample clock.
//
// Not "room". The obvious implementation asked I2SClass::availableForWrite() --
// and that comes from Print, which returns 0 unless a subclass overrides it, and
// I2SClass does not. Every frame concluded there was no space, generated nothing,
// and the game was silent by arithmetic while the codec sat there working.
int  vg_audio_due(void);

// The same write, ALLOWED TO WAIT for room in the DMA ring. Only for a caller that
// is not on the render thread -- which since the synth moved to core 0 is where the
// audio task lives, and waiting there is not a cost but the mechanism: the codec
// paces the producer, so nothing above has to work out how many samples are owed.
int  vg_audio_write_paced(const int16_t* samples, int n);

// ---- IMU -------------------------------------------------------------------

bool vg_imu_init(void);

// Raw accelerometer in g. Returns false if no new sample is available; the
// caller keeps its previous value in that case.
bool vg_imu_read(float* ax, float* ay, float* az);
