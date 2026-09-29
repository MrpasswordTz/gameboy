# Test and homebrew ROMs

These are bundled so that `make test` works out of the box.

## `tests/` — hardware conformance suites

* **Blargg's test ROMs** (`cpu_instrs`, `instr_timing`, `mem_timing`,
  `mem_timing-2`, `halt_bug`, `interrupt_time`, `oam_bug`, `dmg_sound`,
  `cgb_sound`) — by Shay Green (blargg), freely redistributable.
  Source: <https://github.com/retrio/gb-test-roms>
* **dmg-acid2 / cgb-acid2** — PPU conformance, by Matt Currie, MIT licensed.
  Source: <https://github.com/mattcurrie/dmg-acid2> and
  <https://github.com/mattcurrie/cgb-acid2>

Blargg's ROMs report their verdict in one of two ways: over the serial port
(read with `--serial --break-on-serial-done`) or in cartridge RAM
(read with `--memresult`). A few only draw the result on screen — capture it
with `--screenshot`.

The acid2 ROMs draw a reference image. Ours matches the published hardware
capture pixel for pixel; see the comparison in the top-level README.

## `homebrew/` — a real game to try

* **µCity 1.3** by Antonio Niño Díaz — an open-source (GPL-3.0) SimCity-like
  city builder for the Game Boy Color. Exercises MBC5 banking, CGB palettes,
  the window layer and sprites all at once, which makes it a good smoke test.
  Source: <https://github.com/AntonioND/ucity>

Run it:

```bash
./bin/gameboy roms/homebrew/ucity.gbc
```

All ROMs here belong to their respective authors and are included under their
own licences, which are not the same as this project's.
