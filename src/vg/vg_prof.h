#pragma once
#include <stdint.h>

// Frame-time counters, written where the work happens and read by the telemetry
// line in main.cpp.
//
// ===========================================================================
// BEFORE YOU BELIEVE A COUNTER, MAKE IT PROVE IT CANNOT BE ZERO
//
// Four of the counters in this file were wrong at the same time, in four different ways,
// and every one of them read as a plausible number rather than as a fault:
//
//   tnt      declared, reset and read -- and never written. It reported 0 for an effect
//            costing over a millisecond, and that 0 was quoted three times: twice as
//            evidence the boundary warning was cheap, once as evidence it was expensive.
//
//   aa       counts every BLENDED line, not just antialiased ones, because the slice field
//            holds LINE_AA, LINE_ADD and LINE_SUB alike. Making the ship trails additive
//            moved them out of `ln` and into `aa`, which looked like the line cost falling
//            and antialiasing switching itself on. It was neither, and it invalidated a
//            published measurement.
//
//   g_w_*    read thirty lines AFTER the block that zeroes them for the next frame. A whole
//            session's world split came back as five zeros from counters that were working
//            perfectly.
//
//   WORLD    the host returned as soon as it matched the line before it, so the line was
//            printed, transmitted, and never read.
//
// THE COMMON FACTOR IS THE ZERO. A dead counter, a counter read too late and a counter
// nobody collected all produce the same output as a thing that genuinely cost nothing, and
// the reader cannot tell them apart. Three of these survived for hours because they were
// first read in a state where zero was the correct answer -- at the title screen, in the
// quiet part of a session, before the fight.
//
// So: a new counter is not trusted until it has been read in a case where it MUST be
// non-zero. That is one deliberate measurement, and it would have caught all four in a
// minute each.
//
// AND A SPLIT CARRIES THE WHOLE IT SPLITS. Six counters that are supposed to add up to
// g_sub_world went four commits before anybody could check whether they did, because the
// parts and the whole were reported by different instruments over different frames. They
// do -- to 1.2%, which is the micros() calls -- but that was luck, not verification.
// vg_replay_note_world carries the total for exactly this reason.
//
// WHAT IS PRINTED AND WHAT IS ASKED FOR. The periodic report is for WATCHING: the phases,
// the band budget against its 768 us, the frame distribution, the audio. The deep splits
// are for ASKING, on serial 'd', because they answer a question once and then cost a line
// of terminal every two seconds for ever. Twelve lines a window pushed the ones that
// mattered off the top of the screen, which is how a counter reading zero goes unnoticed.
// ===========================================================================
//
// A HEADER RATHER THAN `extern` AT THE USE SITE. These were declared inline in
// vg_render.cpp and vg_sfx.cpp, in function scope, which works and is silently
// unsafe: a function-local extern is a promise about a symbol defined somewhere
// else in the program, and nothing checks it against the definition. Change one
// of these to uint64_t in main.cpp and the two translation units disagree about
// its width, the linker matches them on name alone, and the number quietly goes
// wrong -- which is the worst way for a diagnostic to fail, because a diagnostic
// is what you reach for when you already do not trust the numbers.
//
// Microseconds, reset per frame by main.cpp. They measure SUBMIT -- building the
// primitive list -- and not the raster, which happens later under DMA and is
// timed separately.
extern uint32_t g_sub_star, g_sub_arena, g_sub_world, g_sub_hud;

// Just the mixer, so it can be told apart from the I2S write it feeds.
extern uint32_t g_sfx_render_us;

// AUDIO DELIVERY. Two numbers, and the interesting one is BLOCKED TIME.
//
// Since the synth moved to core 0 the audio task is allowed to wait on the codec,
// and it does -- it renders a fixed chunk and hands it over with a real timeout,
// so the hardware paces the loop instead of a clock model guessing at it. That
// inverts the health signal:
//
//   blocked  microseconds per second spent waiting for the DMA ring to have room.
//            HIGH IS GOOD. It means the ring is full, the queue is as deep as the
//            hardware allows, and the producer is comfortably ahead. Near zero
//            means the render cannot keep up and the codec is running on fumes.
//   short    samples the driver would not take even after waiting out the whole
//            timeout. Non-zero is a real fault: the ring is not draining.
//
// This replaced a modelled queue depth, which was measuring the wrong thing. The
// model assumed a short write meant "the ring is full"; in this driver it also
// means "ESP_LOGE", once per call, over USB CDC, from inside the audio task -- so
// the instrument that was meant to find the stall was causing a bigger one.
//
// Written on core 0 by the audio task, read and reset on core 1 by the telemetry.
// That race is benign in the same way the frame counters' is: a torn read costs
// one wrong diagnostic line out of a report every two seconds, and adding a lock
// to a diagnostic would put the audio task behind the renderer, which is the one
// thing the move to core 0 was for.
extern uint32_t g_audio_blocked_us;  // time spent waiting on the ring
extern uint32_t g_audio_short;       // samples refused even after the wait

// THE MIX'S HEADROOM, which is a different question from delivery and has to be
// asked separately -- the crackle on acceleration was delivery and the one on a
// collision is not.
//
// `peak` is the largest absolute sum the mixer produced BEFORE the soft rail, in
// units where 1.0 is full scale. It is the honest number: what the voices actually
// came to, not what came out. A collision was measured at 1.94.
//
// `knee` counts samples the soft rail acted on at all -- so it says how much of a
// window was loud enough to be shaped, which is the difference between a peak that
// brushed the curve once and one that sat on it.
extern float    g_synth_peak;        // largest |sum| before the rail, 1.0 = rail
extern uint32_t g_synth_knee;        // samples the soft rail shaped

// The HUD's own split. `hud` is the largest single item left in submit and it is
// half a dozen unrelated instruments, so one number for it cannot say which. What
// is left over after these two is everything else in vg_draw_hud, and the
// telemetry works it out by subtraction rather than timing it a third time.
extern uint32_t g_hud_radar, g_hud_throttle;

// THE GRID'S TWO HALVES, AND THEY NOW RUN ON DIFFERENT CORES.
//
// Added to choose the split point and kept because it is what judges it. The arena
// grid was the largest single item on submit's critical path, and its two loops are
// independent -- hoops around the tube, rails along it -- so they were separated:
// hoops on core 1 with the world, rails on core 0 with the instruments.
//
// So these two are no longer parts of one total. They are the two cores' shares, and
// the useful reading is whether they BALANCE against what else each core carries:
//
//   core 1   starfield + hoops + world objects
//   core 0   rails + instruments
//
// `sub` on the telemetry line is the slower of the two, so the gap between them is
// what is still recoverable. Measured after the split at hoops 812 and rails 595 --
// note the rails got slower moving cores, from 426, most likely flash-cache
// contention with the audio task, which also lives on core 0. A finer split therefore
// pays less than the arithmetic suggests.
extern uint32_t g_arena_hoop, g_arena_rail;

// THE TWO HALVES' WALL TIMES, which is what the split has to be balanced against and is
// not what was balanced against the first time.
//
// `sub` is the slower of the two, so the only number that decides where work should go is
// each half's TOTAL. g_sub_hud is not that: it brackets vg_draw_hud alone, and group B
// also carries the rear-view patch, the lock box, the missile markers and the overlays --
// about 900 us that no counter named. Balanced against the part instead of the whole, the
// arena grid went onto the core that was already busier, and a comparison taken on a
// lighter scene reported it as a saving.
//
//   a   the kick to the await: starfield, hoops, world objects, course gate
//   b   the whole of submit_instruments: rails, and every instrument
//
// The GAP between them is what is recoverable, and its sign says which way to move.
//
// MEASURED ACROSS 62 FIGHT WINDOWS, and the answer is that the gap is gone and the
// question has moved. Mean A 2632, mean B 2693 -- balanced to 2%. Neither core is the
// bottleneck: A is the slower half in 24 windows of 62 and B in the other 38, and which
// one it is flips with the scene rather than settling.
//
// So there is nothing left to move. `sub` is near its floor for the total work, and the
// only way down is to do less of it, not to put it somewhere else. Taking an item out of
// one half now buys only the gap, not the item: removing the mirror's 814 us from B would
// make A the bottleneck at 2632, saving 61 us -- or about 437 after rebalancing, because
// the two cores share whatever is left.
//
// WHAT SWINGS IS `world`, AND IT SWINGS 4.5x: 615 us in the quietest fight window and 2794
// in the busiest, against a starfield that never leaves 132-137. It tracks the frame rate
// almost exactly -- every window above 60 fps had world under 970, and every window below
// 54 had it over 1190. That is the shakiness, and it is the submit cost of ships, missiles,
// debris and trails scaling with how much is happening.
//
// The cockpit is NOT part of it. `can` measured 1748-1779 across the same 62 windows, which
// is flat. An earlier claim in this project that `can` swings 2.8x within a fight was wrong:
// those two readings were different STATES, not two moments of one fight.
extern uint32_t g_sub_a, g_sub_b;

// THE UPDATE, WHICH HAD NEVER BEEN MEASURED AT ALL.
//
// `upd` is about 760 us and it was ONE number for the whole simulation. Submit has had a
// per-layer split since early on and blit is accounted to the microsecond, so the update
// was the last phase of the frame that could only be guessed at -- and a phase nobody
// has measured is exactly where an assumption goes to live.
//
// It is also the phase with the least room for error, because it is entirely SERIAL and
// entirely BEFORE the flush. Submit's two halves run on two cores and blit overlaps the
// wire; nothing here overlaps anything. Every microsecond is on the critical path.
//
// The split follows the code's real shape rather than a guess at the cost:
//
//   pre     everything vg_game_update does before the state's own update: the wipe,
//           the alerts, the engine and flatline voices, the menu-tap resolve
//   ship    the flight model and the frame's transform: throttle, agility, bank, R. Two
//           spans, because the arena and the backdrop sit between its halves.
//   arena   vg_arena_step and the clearance test
//   sky     vg_sky_orient -- the backdrop rides the rotation only
//   field   the scenery that is only points: stars, motes, asteroids
//   trail   the player's ribbon, and the cutscene ship with its own
//   enemy    every live opponent's transform and ribbon, inside the world step
//   ord     missiles, debris and fireballs -- the same transform, three more pools
//   vfx     the tail: vg_vfx_tick, the shake, the buzz, the HUD decay, the radio
//   ai      vg_update_enemy for each opponent. The only THINKING in the list.
//   combat  passes, missile logic, lock and threat
//
// Microseconds, and they ACCUMULATE rather than assign, because a long frame is split
// into sub-steps and the whole point is the frame's total. main.cpp resets them.
//
// What the nine do not cover is the rest of the state's own function plus the replay
// note, and the telemetry gets that by subtraction rather than by timing it again --
// the same trick the HUD line uses.
// SPLIT AFTER THE FIRST READING, which is why `ship` is three counters and not one. It
// came out the largest single item at 183 us, and the flight model it is named for is a
// dozen scalar lerps and one 3x3 -- so the number could not have been the model, and a
// span that big with the wrong name on it is worse than no span at all.
//
// Measured in a fight, of 753 us:
//
//     field 113   arena 92   ai 84    vfx 74   oth 74   enemy 68
//     sky 62      pre 50     ship 41  cbt 37   trail 36  ord 21
//
// THE MODEL IS 41 US. The other 142 were the arena and the backdrop, exactly as the size
// of the number said they had to be.
//
// AND THE ARENA IS THE BIGGER OF THE TWO IN FLIGHT, which is the reverse of what the menu
// showed. On the title screen it reads arena 25, sky 58; in a match it is arena 92, sky 62.
// The backdrop barely moves -- it is a fixed rotation of a fixed point set, so it should
// not -- while the arena costs nearly four times more once the ship is really travelling
// through it.
//
// So the menu did not merely under-report the arena, it INVERTED THE RANKING, and a
// conclusion drawn there would have sent the next optimisation at the wrong one of the two.
// This is the third time in this project that a figure taken on one scene has misdescribed
// another; the difference is that this time it was expected and checked.
//
// What the ranking says overall is that there is no lever in upd. The largest span is 15%
// of it, the top four are within 40 us of each other, and the next 100 us would have to
// come from four places at once.
extern uint32_t g_upd_pre, g_upd_ship, g_upd_arena, g_upd_sky, g_upd_field,
                g_upd_trail, g_upd_enemy, g_upd_ord, g_upd_vfx,
                g_upd_ai, g_upd_combat;

// WHAT ELSE IS IN SUBMIT'S GROUP B, which is the largest unexamined phase left.
//
// g_sub_b is the half's total and g_sub_hud brackets vg_draw_hud alone. The note above
// already says the difference is "about 900 us that no counter named" -- these name it.
//
// Worth doing for VARIANCE rather than for the mean. sub was measured swinging 2560 to
// 3400 us between fights, and an 840 us swing is what makes the frame rate shaky rather
// than merely low, which is the complaint. A mean cannot show which part swings.
//
//   lock    vg_draw_lock_box: the target reticle and its box
//   canopy  vg_canopy_prim, the one primitive the whole cockpit rides on
//   marks   the steering indicator, target markers, missile markers, threat indicator
//   over    vg_draw_overlays: the radio, the alerts, whatever is being said
//
// What is left of g_sub_b after these, the HUD and the mirror is the glitch and shake
// arithmetic, which the telemetry gets by subtraction rather than by timing it twice.
extern uint32_t g_sub_lock, g_sub_canopy, g_sub_marks, g_sub_over;

// WHAT `world` IS MADE OF, which is the phase that swings.
//
// g_sub_world was measured at 615 us in the quietest fight window and 2794 in the busiest,
// against a starfield that never leaves 132-137. It tracks the frame rate almost exactly,
// so it is the shakiness -- and it was one number for eight different kinds of object.
//
// A curve to flatten rather than a cost to shave, so what matters is not the mean of each
// of these but which one GROWS when the fight gets busy. Read the range, not the average.
//
//   motes   the speed cue and the debris shards: many points, no structure
//   rocks   the asteroid sort and the asteroids
//   trails  every ship's ribbon. The comment on draw_ship_trail calls these the single
//           largest primitive source in a fight, which this is here to confirm or deny.
//   ships   the hulls, the entry gate, and the cutscene ship's own ribbon -- which sits
//           between them in draw order. Left there: moving a draw call to tidy up a
//           counter is how a rendering bug gets made, and the cutscene is not a fight.
//   msl     the missiles: a body, a head and a trail each
//   fire     the fireballs: concentric rings, and the most of them at once
extern uint32_t g_w_motes, g_w_rocks, g_w_trails, g_w_ships, g_w_msl, g_w_fire;

// THE FRAME'S LAST I2C, split three ways.
//
// `in` is ~680 us and the touch controller is the only I2C left on the render thread -- the
// IMU and the PMU moved to a core 0 task. The touch read CANNOT follow them: CST226's
// getPoint consumes the status buffer's fresh-data marker, so a 4 ms poller eats the sample
// and the frame sees no contacts on most frames. That was tried and it killed steering while
// leaving the throttle looking fine, because the throttle retains its last value.
//
// So the question is not whether to move it but what it is made of:
//
//   lock   blocked in i2c_take, waiting for the sensor task to finish with the bus
//   touch  the whole of vg_touch_read, lock included
//
// `in` minus touch is the partitioning and gesture work, which is pure arithmetic.
extern uint32_t g_in_touch, g_in_lock;

// PANEL DMA TRANSFERS ABANDONED after a two second wait. Should be 0 for ever; anything
// else means the SPI engine is not keeping up and the picture is being degraded to keep
// the game alive. Counted rather than printed at the point of failure -- see panel_reap
// for what printing it from there cost.
extern uint32_t g_panel_wedges;
