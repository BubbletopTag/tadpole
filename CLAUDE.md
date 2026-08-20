# Tadpole — notes for Claude

Read `docs/HANDOVER.md` for how the emulator works. This file is only for the
traps that have already cost several sessions each.

## state.bin: one layout, and a longer file is never an error

`$TADPOLE_DIR/state.bin` carries the on-panel rectangle of each framebuffer
layer. **`tadpole/shim/tadpole_state.h` is the only place its layout is
written down.** The shim, the guest's GL library and the viewer all include it;
`tools/fbshot.py` and `tools/burst.py` decode the same bytes by hand and are
pinned by `tools/tests/state_layout_test.py`.

Get this wrong and the symptom is always the same, and never looks like a
layout problem:

> Every Leapster/Didj title renders at the full 480x272 panel instead of the
> 320x240 window its ViewFrame gives it — scaled up, cropped, with the right of
> the picture under the bamboo chrome.

That is a reader deciding it cannot trust `state.bin` and falling back to the
whole panel. It has arrived from four directions now: a field added in one
mirror and not the others, a Windows mmap that rounded the file up to 64 KB, a
`make shim` that left a second shim variant stale, and the android branch
appending a camera block — which reached main because **the worktrees share
`runtime/shimlibs` by symlink**, so the shim a run loads is whichever branch
built last.

Two rules:

* **Grow only at the end.** Everything up to and including `layer[NUM_FB-1]` is
  frozen. A field inserted above the layers moves them for every binary that has
  not been rebuilt, and nothing warns.
* **Only a *shorter* state.bin is unreadable.** A longer one means the writer is
  newer and appended at the end, which rule one makes harmless. Never compare
  the file's length to your own `sizeof` for equality — that is the bug itself.
  Use `tad_state_fault()`; do not open-code another check.

And if a reader does refuse, it must **say so on stderr** (the GL side also
writes `gl-warnings.log`). A silent fall back to the full panel is what made
this expensive: it is indistinguishable from a rendering bug, so every
investigation started in the rasteriser, which was never at fault.

Before touching anything that reads or writes that file:

    cd tadpole && make all                          # the header is a prerequisite
    ./tadpole/viewer/tadpole-view --selftest-state  # a 528-byte state still works
    ./tadpole/viewer/tadpole-view --selftest-layers
    ./tools/tests/state_layout_test.py

To see it end to end, run a title that has a ViewFrame and look at the window
the guest asks for against what gets drawn:

    ./tadpole.sh --app "LST3-0x00180025-000000"     # Clam Prix, 320x240 at 15,17
    ./tadpole.sh --app SNC                          # Sonic (Didj), 320x240 at 18,17
    ./tools/fbshot.py /tmp/shot.png                 # prints each layer's window

## Rebuild every shim variant, not just the one you changed

`shim/tadpole_shim.c` is linked into **two** libraries — `libdl.so.0` for
AppManager and `libz.so.1` for the display tools (`imager-fb` and friends, which
draw the boot logo). `make shim` builds only the first. A stale `libz.so.1` was
still creating `state.bin` at the old size long after the shim itself was fixed,
because the logo runs first and creates the file. Use `make all`.
