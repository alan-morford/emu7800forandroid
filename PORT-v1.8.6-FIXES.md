# Porting the webOS v1.8.6 fixes to EMU7800Android

Source of truth: `emu7800forwebos` @ tag **v1.8.6** (commit `74b7f15`).
On this machine: `../../emu7800/com.emu7800.touchpad/`.

These are five independent fixes. Each stands alone — apply and verify one at a
time, in any order.

---

## READ THIS FIRST: do NOT run `copy_core.sh`

It would undo work rather than sync it:

1. It copies from `../emu7800source/`, which is a **snapshot of webOS v1.6.2**
   (commit `a31a3b4`, taken March 2026). The webOS app is now at v1.8.6, so that
   snapshot is roughly four releases behind. Copying from it would *revert* this
   project's core.
2. This project's core has **local modifications** despite the "do NOT modify"
   banner. Verified differences against the snapshot:
   `tia.c`, `tia.h`, `cart.h`, `machine.c`, `machine.h`.
   A verbatim copy would silently discard them.

So apply the changes below **by hand**, into the Android core as it stands. Each
one is self-contained and was checked to have a landing spot in this tree.

If you want to re-point `copy_core.sh` at the live repo later, that is a separate
job: first reconcile those five locally modified files, or the next sync loses them.

---

## Fix 1 — Cart type database (fixes Summer Games, Winter Games, Plutos, Sirius)

**Symptom:** these games render as "pixel soup" — correct background colours,
everything else noise.

**Cause:** they are **A78SGR** (SuperGame *with* 16KB RAM at $4000) and were
detected as plain **A78SG**. They build their display lists in that RAM at $6000;
with no RAM mapped there, MARIA faithfully renders ROM graphics bytes *as a
display list*. A78SG and A78SGR are byte-for-byte indistinguishable, so this is
**not inferrable from the ROM image** — it needs a database.

**Apply:**

1. Copy `plugin/src/rom_db.h` from the webOS repo into `jni/src/core/rom_db.h`
   (431 entries, generated from EMU7800's upstream `ROMProperties.csv`).
2. In `cart.c`, delete the hand-written `rom_properties_db[]` table and replace it
   with `#include "rom_db.h"`.
3. Replace the linear `lookup_rom_db()` scan with a binary search — the generated
   table is sorted by MD5:

```c
static const RomDbEntry *lookup_rom_db(const uint8_t *md5)
{
    int lo = 0, hi = (int)NUM_ROM_DB_ENTRIES - 1;

    /* Table is sorted by MD5 (see tools/gen_romdb.py). */
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        int cmp = memcmp(md5, rom_properties_db[mid].md5, 16);
        if (cmp == 0) return &rom_properties_db[mid];
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return NULL;
}
```

This tree already has `md5_compute()`, `RomDbEntry` and `CART_7800_SGR`, so
nothing else is needed.

**Regenerate later:** `python3 tools/gen_romdb.py <EMU7800>/src/assets/ROMProperties.csv > rom_db.h`

**Verify:** Summer Games and Winter Games should show a diving platform, athlete,
crowd and clouds. Detection should log `7800_SGR`, not `7800_SG`.

---

## Fix 2 — Per-cart WSYNC quirk (fixes Kung Fu Master, Missing in Action)

**Symptom:** KFM has coloured bands across the status bar, a grey sky and a grey
floor. Correct is a grey status bar, blue sky and tan floor.

**Cause:** KFM builds its screen with chained DLI handlers that set BACKGRND and
then burn scanlines with runs of `STA WSYNC` (`85 24`). Honouring those CPU halts
lands every colour band in the wrong place.

**Important:** there is **no general hardware rule** here. Upstream EMU7800
honours WSYNC exactly as we do and renders KFM the same wrong way; ProSystem only
gets it right via a per-cart flag. Disabling WSYNC globally changes **35 of 74**
7800 ROMs, so it must stay per-cart.

**Apply:**

1. Copy `plugin/src/quirk_db.h` into `jni/src/core/quirk_db.h` (8 entries).
2. In `cart.h`, add to the `Cart` struct and after it:

```c
    uint32_t quirk_flags;  /* Per-cart hardware quirks, see CART_QUIRK_* */

#define CART_QUIRK_CYCLE_STEALING 0x01  /* ProSystem-only; we always steal */
#define CART_QUIRK_NO_WSYNC       0x02  /* MARIA WSYNC must not halt the CPU */
```

3. In `cart.c`, before the ROM-properties lookup:

```c
typedef struct {
    uint8_t md5[16];
    uint32_t flags;
} CartQuirkEntry;

#include "quirk_db.h"

#define NUM_CART_QUIRK_ENTRIES (sizeof(cart_quirk_db) / sizeof(cart_quirk_db[0]))

static uint32_t lookup_cart_quirks(const uint8_t *md5)
{
    int lo = 0, hi = (int)NUM_CART_QUIRK_ENTRIES - 1;

    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        int cmp = memcmp(md5, cart_quirk_db[mid].md5, 16);
        if (cmp == 0) return cart_quirk_db[mid].flags;
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}
```

4. In `cart_load()`, **after** any 128-byte A78 header has been stripped (the
   digest must cover the bare ROM image, to match ProSystem's `cartridge_digest`):

```c
    if (machine_type == 1) {
        uint8_t qmd5[16];
        md5_compute(data, (size_t)size, qmd5);
        cart->quirk_flags = lookup_cart_quirks(qmd5);
    }
```

5. In `maria.c`, add a module-level `static int g_wsync_enabled = 1;` plus a
   setter, and gate the WSYNC case:

```c
void maria_set_wsync_enabled(int enabled)
{
    g_wsync_enabled = enabled ? 1 : 0;
}
```

```c
        case WSYNC:
            if (g_wsync_enabled) {
                maria->wsync = 1;
                if (g_cpu_preempt) {
                    g_cpu_preempt();
                }
            }
            break;
```

Declare `maria_set_wsync_enabled()` in `maria.h`.

6. In `machine_reset()`, 7800 branch (after `cart_reset()`):

```c
        maria_set_wsync_enabled((g_cart.quirk_flags & CART_QUIRK_NO_WSYNC) == 0);
```

   and in the 2600 branch add `maria_set_wsync_enabled(1);` so a stale setting
   from a previous 7800 ROM cannot linger.

**Verify:** KFM shows a grey status bar, blue sky, tan floor. Missing in Action's
title screen renders (it was broken too, and is fixed by the same flag).

---

## Fix 3 — 7800 display window (Android-specific; verify, don't copy blindly)

**Symptom:** the 7800 picture sits too high and looks slightly squashed.

**Cause:** showing all 242 scanlines MARIA draws, when a real NTSC set only shows
about 223 of them. ProSystem distinguishes these explicitly:
`displayArea = rasters 16..258` (what MARIA draws) versus
`visibleArea = rasters 26..248` (what a TV shows — **223** lines).

**webOS did:** `START_LINE_7800 12 -> 22`, `VISIBLE_7800 242 -> 223` in `video.c`.

**Android is different and needs checking.** `platform/video.c` here uses:

```c
#define FB_H_7800   242
#define SKIP_7800    11
```

`SKIP_7800 = 11` is one lower than webOS's old `START_LINE_7800 = 12`, so the two
ports do not index the MARIA framebuffer identically — very likely because this
tree's `maria.c` is the older v1.6.2 `output_line_ram()`. **Do not just write 21
and 223.** Determine the correct skip empirically with the harness (below), then
set `FB_H_7800` to `223` and `SKIP_7800` to whatever lines up.

**Also worth questioning:** this port crops horizontally —

```c
#define HCROP_7800   32          /* FB_W_7800 = 320 - 64 = 256 */
```

ProSystem shows the **full width 0..319** with no horizontal crop, so those 64
columns are real picture being thrown away. If the crop was a deliberate choice
for phone screens, keep it; if it was an attempt to hide edge artifacts (e.g. Kung
Fu Master's left-edge strip), note that the strip is drawn by the game itself and
appears in every 7800 emulator.

---

## Fix 4 — 2600 speed, about +22% on ARM

**Cause:** profiling showed **80%** of all 2600 time in
`render_from_start_clock_to()`, which performed ten integer modulo operations per
colour clock.

**Apply** in `tia.c` (this tree has 12 `% 160`; the ten inside the render loop are
the ones that matter):

```c
/*
 * Position counters wrap at 160 but may legitimately be negative: RESPx/RESMx
 * set them to -4, -2 or -((hsync-68)>>1), so they occupy roughly -80..159. C's
 * `%` leaves negative values alone for that range, and so does this.
 */
#define TIA_ADV160(v) do { if (++(v) >= 160) (v) = 0; } while (0)
```

Then, inside `render_from_start_clock_to()` only:

- `tia->hsync = (tia->hsync + 1) % 228;` → `if (++tia->hsync >= 228) tia->hsync = 0;`
- each `tia->p0 = (tia->p0 + 1) % 160;` → `TIA_ADV160(tia->p0);` (p0, p1, m0, m1, bl
  — in both the visible-portion block and the HMOVE block, ten in total)

Leave any `%` **outside** that function alone (e.g. the RESMP
`(tia->p0 - middle) % 160`).

**This must be bit-identical.** On webOS it was verified across all 560 2600 ROMs
with zero differences. Do the same here before accepting it.

Measured on HP TouchPad hardware: 116 → 141 fps on Asteroids.

---

## Fix 5 — EMU7800 palette as a fourth option

Optional, cosmetic. The upstream EMU7800 `MariaTables` palette is noticeably more
saturated than the Trebor variants.

1. Copy the `maria_palette_emu7800[256]` table from webOS `maria.c`.
2. `maria.h`: add `#define MARIA_PALETTE_EMU7800 3` and change
   `MARIA_PALETTE_COUNT` from `3` to `4`.
3. `maria_get_palette()`: add `case MARIA_PALETTE_EMU7800: return maria_palette_emu7800;`
4. Add an `"EMU7800"` label wherever the palette name is produced.
5. **`platform/input.c` line ~1134** — this is the one that will bite:

```c
case 7: video_set_maria_palette((video_get_maria_palette() + 1) % 3); break;
```

   The hardcoded `% 3` must become `% MARIA_PALETTE_COUNT`, or the fourth palette
   is unreachable. Check `platform/input.c` line ~1091 too, where the label array
   is indexed — it needs a fourth string or it will read out of bounds.

---

## Verification harness — build it here too

`plugin/test/headless.c` in the webOS repo links **only core files** and has no
SDL, GL, PDL or Android dependency. It builds against this project's core as-is,
and is the fastest way to check each fix without an emulator or device:

```bash
gcc -O2 -w -I app/src/main/jni/src/core \
    -o headless <path-to-webos>/plugin/test/headless.c \
    app/src/main/jni/src/core/{m6502,tia,tiasound,pia,cart,machine,maria,pokeysound,savestate}.c \
    -lm
```

You will need to provide a `log_msg()` stub and possibly `puff.c`/`zip_load.c`
depending on what this tree's `cart.c` references.

Useful modes:

- `-o out.ppm` — write the visible frame
- `-raw out.raw` — full uncropped 320x262 colour-**index** plane, for diffing
- `-dl` — walk the Display List independently of `maria.c`. This is the highest
  value tool: a garbage display list means the fault is *upstream* of MARIA (cart
  type, banking, missing RAM); a sane one means it is in the rasteriser. That
  single distinction is what identified Fix 1.
- `-press reset:150 -press fire:300` — drive past the title screen

`plugin/test/README.md` has the full differential-testing guide, including how to
tell a real divergence from a boot-frame phase offset.

**Compare colour indices, not RGB** — the two ports may ship different palettes,
and indices isolate emulation from palette choice.

---

## Things not to re-investigate

- **KFM's black strip down the left edge is drawn by the game.** Every zone's
  display list ends with `gfx=$B0CA w=3 hpos=0`. It appears in webOS EMU7800,
  upstream EMU7800 and ProSystem alike. A real TV hides it in overscan.
- **`BCntl` (CTRL bit 3) is dead code** in both this port and upstream EMU7800 —
  parsed, stored, never used. Implementing it will not change anything visible,
  because it governs the region *outside* the 320 active pixels, which nobody
  renders.
- **Ace of Aces' single-scanline title flicker** is a known upstream inaccuracy in
  MARIA DMA clock counting. EMU7800 hides it with a hard-coded per-game
  workaround; webOS deliberately does not.
