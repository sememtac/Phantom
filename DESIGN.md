# PHANTOM — design

A dogfight sim built for the ESP32-S3, structured as a brutal intergalactic
knockout tournament. You enter under a three-character callsign, pick a ship,
and fly four matches. Lose one and it is over.

This document is the design spine. It is deliberately opinionated about *why*
each rule exists, because most of them exist to protect one thing: the trade
between speed and engagement capability. Top speed is how you survive and
disengage; low speed is the only way to fight. Every system below either
sharpens that choice or gets out of its way.

---

## The tournament

### Format

Sixteen entrants, single elimination, four rounds: R16, quarter-final,
semi-final, final. The player flies four matches. The other eleven resolve in
simulation.

- **Lose and the run ends.** Straight back to the main menu.
- **Win the final and the game is over.** You won. That is the whole arc — a
  coin-op run you are trying to survive, not a campaign you progress through.
- There are no continues, no lives and no second chances.

### Seeding

The bracket is seeded conventionally and **the player is placed in the #1 slot**.
That is not flattery, it is difficulty design: conventional seeding means the top
slot meets #16, then the 8/9 winner, then the 4/5 side, then the #2 half. The
opposition escalates every round for free, with no difficulty scalar anywhere in
the code.

Flavour-wise the unknown entrant simply gets *placed* at the top of the sheet.
Nobody knows what a Phantom is. Everyone else has to explain that.

The other fifteen are generated from the same three axes the player chose —
callsign, trail colour, ship — and seeded 2–16 by a rating derived from their
ship spec plus a skill roll.

### Simulating the rest

Unplayed matches resolve as a weighted coin flip on the two ratings, with a real
upset rate of roughly 20–25%. Upsets are *content*: the #2 seed dying in the
first round is the kind of thing that makes the bracket sheet worth looking at,
and it means the final is not knowable from the entry screen.

NPCs do **not** carry damage between rounds. Only the player does. See
[Credits](#credits) — that asymmetry is the difficulty curve.

### The bracket sheet

Shown between rounds, and available before the first match to scout.

A 16-entry bracket drawn mirrored — eight left, eight right, final in the
middle — is very nearly square, which fits a 480×480 panel almost suspiciously
well. Nine columns of three characters fits at small text with room to spare.

- The **whole bracket is always visible** as the default view. Dragging pans a
  zoomed level for reading detail. Swiping is for inspection, never for reaching
  content you cannot otherwise see.
- Opens auto-centred on the player's next match.
- Each slot shows callsign, trail colour and a ship glyph. This makes the sheet
  **tactical** rather than decorative — seeing a BALLISTA two rounds out is
  something you can plan for.
- Eliminated callsigns strike through and drop to the dim ink level.

---

## A match

One on one. Deathmatch. First ship destroyed loses.

- **No clock, no decision, no draw.** A match ends when somebody dies.
- **Mutual destruction is a player loss.** The player died, so the player does
  not advance. Implementation: resolve player damage before enemy damage within
  the frame, so a simultaneous kill falls the right way.
- There is no stalemate risk. With hull regeneration removed, every hit is
  permanent, so the match strictly progresses toward a conclusion with each one —
  and the AI closes to engage rather than running.

Flying into the arena boundary is still instantly fatal. That is what gives the
high-speed escape option a real cost: you are least able to turn at exactly the
moment you are covering ground fastest.

---

## Ships

Chosen once at entry and locked for the whole tournament. Choice is **free** —
these are playstyles, not a power ladder. If they cost credits they would become
a progression treadmill, which is not what this is.

Each is named for the single quality that defines it.

### AEGIS — the shield

The reference ship, and the one an average player will have a good time with
without ever picking anything else. Decent speed, the most forgiving hull, solid
damage, no glaring weakness. Everything else is measured against it.

### LANCE — the point

For players who do the opposite of lazy shooting. Enormous damage on a clean
hit, almost nothing on a graze, so only correct geometry pays. Slightly slower
than AEGIS as the price of that ceiling. A demanding ship that is genuinely
worse than AEGIS in the hands of someone who sprays.

### CHARIOT — the speed

Fast, agile, fragile. A big magazine of weak, poorly-tracking missiles and the
fastest airframe in the tournament. Rewards running circles and saturating the
sky. The shallow damage falloff means aim barely matters — volume does. Balanced
by a hull that comes apart if anything touches it.

### BALLISTA — the range

Torsion artillery. The longest lock range, the highest damage, and a missile
that is *slower than everything it shoots at but stays alive for eighteen
seconds* — it cannot run you down, it simply refuses to go away. The slowest
airframe by a distance, a punishing lock time, and three rounds with a six
second reload. Helpless once something fast is inside its range.

### Specs

First pass. All of these want hardware time before anyone believes them.

| | AEGIS | LANCE | CHARIOT | BALLISTA |
|---|---|---|---|---|
| Top speed | 420 | 390 | 460 | 340 |
| Turn rate | 1.9 | 1.9 | 2.2 | 1.6 |
| Hull | 110 | 95 | 70 | 90 |
| Missile damage | 20 | 32 | 12 | 40 |
| Graze floor | 0.60× | 0.20× | 0.85× | 0.50× |
| Missile speed | 340 | 340 | 380 | 300 |
| Missile turn | 2.5 | 2.5 | 1.7 | 2.3 |
| Missile life | 10s | 10s | 7s | 18s |
| Lock range | 1600 | 1600 | 1300 | 2800 |
| Lock time | 0.45s | 0.60s | 0.25s | 1.10s |
| Magazine | 6 | 4 | 12 | 3 |
| Reload | 3.2s | 4.0s | 1.4s | 6.0s |

Against a 110-hull AEGIS that is roughly: AEGIS 6 clean hits, LANCE 4 clean but
**17** if it keeps grazing, CHARIOT 10 with a twelve-round magazine and a 1.4s
reload, BALLISTA 3.

`AGILITY_SLOW_BONUS` / `AGILITY_FAST_MALUS` — the speed-versus-turn trade —
should be per-ship too. Part of CHARIOT's identity is that it keeps more of its
agility at high speed, and part of BALLISTA's is that it keeps almost none.

### These are also the enemy roster

Every ship is something the player will *fight* three or four times a run, so
each needs a legible tell from the receiving end:

- **AEGIS** — a fair fight.
- **LANCE** — one missile that really hurts, rarely fired.
- **CHARIOT** — a swarm you have to out-turn, none of which individually matters.
- **BALLISTA** — something arriving from beyond your own lock range.

If two of them read the same from the cockpit, the ship glyph on the bracket
sheet stops meaning anything and scouting stops being a mechanic.

---

## Damage

### Proximity falloff

Missiles already carry a proximity fuse (`MISSILE_HIT_RADIUS`, 18 units). Right
now a detonation is binary. It becomes **scaled by how close it actually went
off**, with the floor of that curve set per ship.

```
damage = spec.missile_damage * lerp(spec.graze_floor, 1.0, 1 - d/HIT_RADIUS)
```

This one knob is what makes LANCE and CHARIOT exist as playstyles rather than as
different numbers. A high-yield narrow warhead demands correct geometry; a
low-yield wide one rewards saturation. It belongs to the **attacker's warhead**,
not to the target's armour — it is a shooting distinction, so it lives on the
missile.

It also applies symmetrically when the player is shooting, which turns missile
accuracy into a skill expression instead of a coin flip on the fuse.

### Hull

- Hull is an absolute pool in points, not a 0..1 fraction, so ships can differ.
- **There is no regeneration.** `HEALTH_REGEN_DELAY`, `HEALTH_REGEN_RATE` and
  `HEALTH_REGEN_FLOOR` come out. Damage is permanent for the entire tournament
  and the only way to undo it is to pay for it.
- `ENEMY_HP` stops being a discrete counter and becomes the same hull-and-falloff
  model, so NPCs run on `ShipSpec` too and shot quality matters when the player
  is the one shooting.

The current `DMG_MISSILE 0.34` against a 0..1 hull means everything dies in three
hits. That was only survivable because regen caught you, and it leaves the credit
economy no room to operate — you would be either untouched or dead, with nothing
in between worth buying back. Hence the numbers above.

### Collision

**Any collision kills the player outright.** The wall, an asteroid, another
ship — on any hull, at any speed. There is no collision damage number.

This used to be three values: 26 for an asteroid, 42 for a ram, fatal for the
wall. A rock was therefore survivable on a full hull and lethal on a low one, so
the player had to carry a running estimate of which contacts they could afford.
That is a lot of hidden arithmetic to ask of someone in a turning fight, and the
answer it produced was never interesting. One rule replaces it: touch anything
and the run is over.

It stays a **loss** even when the opponent dies in the same instant. Player death
resolves first, for the same reason a mutual kill has always been a loss — you
died, so you do not advance.

### The suicide run

Some pilots will trade their ship for yours.

Willingness is rolled **once per pilot** at spawn (`ENEMY_KAMIKAZE_CHANCE`), so a
given opponent either is that sort or is not, for the whole match. Rolling it per
frame would make every enemy occasionally suicidal, which reads as a bug rather
than as a character.

A willing pilot commits when their **hull is nearly gone**
(`ENEMY_KAMIKAZE_HULL`) and the player is **close** (`ENEMY_KAMIKAZE_RANGE`).
Both conditions matter: the first means they are dying anyway and the ship has
stopped being an asset worth protecting, and the second makes it a decision taken
during a fight rather than a behaviour they arrived with.

Once committed it never clears. A pilot who has decided to ram does not change
their mind.

The run sits **above missile evasion** in the AI's priority order — they no longer
care what is chasing them — and **below wall avoidance**, because the wall cannot
be aimed at anybody. They fly at full throttle, which also means they cannot
shoot, since firing needs a low speed. The aim point is slightly *past* the
player: a pursuit curve that converges exactly on its target arrives with the
closing speed fallen to nothing, which is a stern chase rather than a collision.

The only warning is a line on the radio and a contact that stops manoeuvring and
points straight at you.

---

## Credits

A persistent meta-currency. Credits survive death and carry into the next
tournament.

Crucially they buy **durability, not power**. They are spent down and they never
make a ship better, so the meta layer does not soften the game the way
meta-progression usually does. A good run funds the next one; that is all.

### Earning

| Round won | Pays |
|---|---|
| R16 | 100 |
| QF | 175 |
| SF | 300 |
| Final (champion) | 600 |

Plus a condition bonus of up to +50% of base, scaled by hull remaining. A clean
run tops out around 1,750; a scrappy one lands nearer 1,200.

That bonus does mean flying well pays twice — you earn more *and* need less
repair. The flat base is large enough to dominate, so one bad match does not
spiral the run.

### Repair

Between rounds, at a flat **4 credits per hull point**, on every ship.

Partial repair is the entire point of the system. The interesting decision is
never "repair or not," it is *"twenty points now, or bank it for the semi where I
know I will need it."* The repair screen should reuse the throttle-strip
interaction — a slider is already a control the player knows.

Flat pricing needs no per-ship adjustment: a bigger pool costs proportionally
more to maintain simply because there is more of it, so AEGIS's 110 points cost
57% more to restore than CHARIOT's 70. Hull size is therefore economically
neutral, and a ship's real advantage is its falloff profile.

If you cannot afford to repair, that is your problem. This is where the
brutality lives.

### The bank

Capped at **1,500** — roughly one strong run's earnings. Without a ceiling a
patient player banks five thousand, full-repairs every round forever, and the
system evaporates.

A tournament entry fee is the more interesting version of this (very coin-op, and
it makes a failed run genuinely cost something) but it introduces a going-broke
failure state. Prove the simple economy on hardware first.

### Why this is the difficulty curve

**The player is the only entrant who carries damage.** The semi-final opponent
arrives fresh; the player arrives at whatever they could afford. Opponents get
better through seeding, the player gets more worn down, and credits are the only
brake on that.

That is a better escalation lever than varying the arena per round, because it
falls out of systems already being built — and it means the economy is not a side
layer bolted onto the game, it *is* the game's difficulty curve.

---

## Persistence (NVS)

- Credit balance
- Player callsign, ship and trail colour (as defaults for the next entry)
- Reigning champion's callsign — seeded into the next bracket
- Best round reached

---

## What this changes in the code

The load-bearing refactor is **`ShipSpec`**. Every row of the spec table is
currently a `#define` in `cfg_flight.h` or `cfg_combat.h`. They become fields on
a struct, and both the player and the AI read from the same one.

1. **Done.** `ShipSpec` struct; player and enemy both carry one. Fly all four
   types. Nothing else works until this lands, and it is independently testable
   — you find out whether CHARIOT is actually fun before a line of tournament
   code exists.
2. **Done.** Hull as an absolute pool; regen removed; proximity falloff on
   damage.
3. **Done.** Tournament state machine: bracket generation, seeding, simulation,
   advance. Matches are strictly 1v1 and end on a death.
4. **Done.** Bracket sheet rendering and panning, ship select, pause menu, and
   the screen flow that joins them.
5. **Done.** Callsign entry — three letter wheels rather than a keyboard, and a
   hue-only colour picker. Saturation and value are pinned, because a chooser
   that exposed either would let a player select a trail they cannot see.
6. **Done.** Ship trails, driven by throttle: each sample stores the power it
   was laid down at, so a ship at idle leaves a stub, a ship at full throttle
   streams a long ribbon, and a burst of speed stays lit in the tail after the
   ship has slowed. Level of detail along the ribbon keeps a 7.7s trail from
   costing 7.7s worth of primitives.
7. **Done.** Credit economy and the repair screen.
8. **Done.** NVS persistence: credits, callsign, ship, trail colour and champion
   status. Hull is deliberately excluded — it belongs to a run, not to the
   player.

Built since, beyond the original plan:

9. **Pilot voices.** Six archetypes, three lines each across taunt / fire / hurt
   / death. See below.
10. **The narrative loop.** Winning sets a permanent flag, and the intro crawl's
    closing line changes from "THEY CALL HIM…" to "THEY CALL YOU…". The rumour
    the game opens with becomes a rumour about you.
11. **A menu-only backdrop** — a lensed supermassive black hole, fixed at
    infinity so it never pans, with its own brightness ceiling since the menu
    has no HUD to protect.

Still open: sound, more arena shapes, a gun for close-in work, and TE sync.

### Pilot voices

Fifteen generated strangers with three-letter tags is a spreadsheet. What makes
them opponents is that they talk at you, at the four moments you are already
paying attention: when they size you up, when they shoot, when you hurt them,
and when you kill them.

Six archetypes — **BUTCHER**, **ZEALOT**, **OPERATOR**, **HUNTER**, **REVENANT**,
**PALADIN** — with three lines each. Enough that a sixteen-entrant bracket does
not repeat itself inside a round, few enough that each stays a recognisable
person rather than a bag of lines.

The death lines carry the setting. The IFT runs this for viewing figures and tax
revenue; a game that opens by saying so and then has pilots politely explode is
not telling the truth about itself. So they die badly, and they die on the radio
where the player has to listen to it.

Two rules hold the system together:

- **Priority is the enum order** — taunt, fire, hurt, death — so a death cry can
  never be stepped on by the next round going wide, while an equal event still
  refreshes and two hits read as two.
- **Personality is rolled independently** of ship, seeding and hue. Tying them
  together would make the whole bracket readable at a glance.

Lines are capped at 26 characters, which is what fits beside a callsign tag at
readable size. The limit does the writing a favour: radio chatter under fire is
clipped, not composed.

### Trail colour and the palette rule

The interface follows AmberConsole's discipline: *hierarchy is brightness,
inverse video and blink — never hue.* Trail colour is hue, so it breaks that
rule, and the resolution is to make the exception absolute:

**Hue is reserved for identity and nothing else.** Every instrument, readout and
piece of arena geometry stays amber-on-brightness. The only coloured things in
the world are ship trails.

Hue therefore stops being decoration and becomes IFF. In a wireframe game a
distant contact is four pixels, but its trail is a long coloured stroke — colour
is *how you know who that is*. Used for exactly one thing, the rule gets stronger
rather than weaker.

---

## Open

- Every number in the spec table, the payout table and the repair price. They
  are internally consistent but completely unvalidated on hardware.
- Hull-to-damage ratio in particular sets run length now that nothing heals.
  Target: enough hits absorbed across a run that the credit system has something
  to decide about.
- Whether the AI can actually fly all four specs competently, or whether
  `vg_ai.cpp` needs per-archetype behaviour. BALLISTA especially — an AI that
  does not use its range advantage is just a slow AEGIS.
- Narrative framing for the loss screen. Round reached and credits banked for
  now; the fiction comes later.
- Sound. Still entirely unused, and a lock tone is worth more here than most of
  the above.
