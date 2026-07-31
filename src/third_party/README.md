# Third-party

`es8311.[ch]`, `es8311_reg.h` — Espressif's ES8311 codec driver, Apache-2.0.
Vendored rather than pulled as a dependency because it is the only part of this
project that is not ours, it does not change, and a codec register sequence is
not something worth re-deriving from a datasheet to save 700 lines.

Unmodified. When Phantom is embedded in another firmware, that firmware's own
audio layer replaces `vg_audio_*` in the port and these files come out with it.
