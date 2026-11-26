# Handoff (2026-11-26)

Where Flamingo stands, what was found, and what to look at next. Measured on one PC: Windows 11,
NVIDIA RTX 4070 12 GB (32-lane waves, a capped 1,000,000-slot view heap), Ryzen 7 7800X3D,
32 GB. All frame rates are medians over a scripted run through the first mission's boat
cutscene (Series S profile unless said otherwise).

## Playable settings

About **9-10 fps** in the boat cutscene (run 193: 9.8 fps, 102 ms a frame: 46 ms preparing,
29 ms on the GPU):

```
set VICTORY_NO_WATCHDOG=1
set VICTORY_PIPELINE_WAIT_MS=24
set VICTORY_RELAY=1
set VICTORY_LATE_WAIT=3
set VICTORY_CARD_BUFFERS_MIN_MB=1
set VICTORY_CARD_BUFFERS_MB=1024
set VICTORY_INDIRECT_WINDOW=0
set VICTORY_SHADER_OPT=0
set VICTORY_WIDE_MERGE=1
set VICTORY_WIDE_TEXTURES=1
set VICTORY_VIEW_GUARD=0
set FLAMINGO_BVH_MISS=1
set VICTORY_HEAP_WORKER=1
set VICTORY_AMBIENT=0.6
set VICTORY_SKIP_PIPELINES=7115e46a79c78f98,59b1ba494f18c894,b8ca32a6899b87fb,659fe9cae59dbd88
target\diag\victory.exe run <your dump>\<the title's executable> --execute --console series-s
```

What each step gave, in order (same scenes):

| Step | fps | |
|------|-----|-|
| Series X profile (2560x1440 targets) | 5.4 | |
| Series S profile (1280x720, the title upscales) | 7.1 | GPU 65 to 35 ms a frame |
| `VICTORY_HEAP_WORKER=1` | 9.1 | textures picked by index uploaded by a thread of their own: the submitting thread's texture pass went from 16 ms to 0 |
| `VICTORY_AMBIENT=0.6` and the impostors left out | 9.8 | |

`VICTORY_TEXTURE_NO_WAIT=1`, `VICTORY_RELAY=2`, `VICTORY_WIDE_BACK=1`,
`VICTORY_CARD_REHASH_RUNS=1`, `VICTORY_JOIN_COPIES=1`, `VICTORY_JOIN_PARTS=1` and
`VICTORY_LEVELS_ON_DEMAND=1` together gave nothing more here (9.0 against 9.1).

The submitting thread is now the limit (about 45 ms of preparing a frame). The faster forks
(avatarmanz/VIctory-7900XTX's speed work, victory-fork main's later commits) target exactly that.
victory-fork main is 79 commits ahead of this tree; a trial merge conflicts in 22 files.

## Fixed this round

- **Pipeline syncs order shader writes.** The console's pipeline syncs (list slots 0x408 and
  0x410; 0x418 and 0x420 in newer titles) wait for earlier work. They were no-ops ("host lists
  order their own work"), which is wrong for D3D12: a host list does not order its own
  dispatches. They are now a barrier on every shader write (`FLAMINGO_SYNC_BARRIER=0` restores
  the old behavior). Since then a character whose clothes came and went from run to run has
  been drawn whole in every run.
- **Faces move.** The title's face network keeps half floats in `R16G16B16A16_UINT` and
  `R16_UINT` views, in descriptor tables, and moves them with `_d16` typed accesses. They were
  converted as halves through a float handle, so its output was garbage. The view's format kind
  now rides in the table of strides beside the stride, also for views in tables (VIctory
  4243a96a by way of avatarmanz/VIctory-7900XTX 9a6c829d and that fork's #114).
- **The impostors are left out** (see below), which removes the biggest artifacts.

## Open: artifacts

### Impostors (distant trees and buildings)

The title's octahedral impostors draw as huge flat panels on this layer (olive or green, covering
much of the screen in some boat shots), and the water reflects them as dark rectangular blocks.
Leaving out their four pixel programs (the `VICTORY_SKIP_PIPELINES` list above) removes the
panels, most of the dark blocks on the water, and the dark boxes that floated over the beach; it
also brought back a character's body in several shots. Far trees and some far buildings are then
missing.

| Programs | What they draw | Left out |
|----------|----------------|----------|
| `MS_VS_OctaSLODWind` (`c89489afd89ee7f0`) + `PS_OctaSLODReZ` (`59b1ba494f18c894`), `PS_OctaSLODFireReZ` (`b8ca32a6899b87fb`) | the big flat panels | runs 190, 191 |
| `VS_OctaWind` (`b3ba0ef9ee352720`) + `PS_Octa` (`7115e46a79c78f98`), `PS_OctaVsplit` (`659fe9cae59dbd88`) | the dark blocks on the water, a body hidden | run 190 |

Suspect: the passes that build the impostors' instance lists. `CS_Tree_BatchCull_ImposterOnly`
(`89d08e561897d4e5`), `CS_Tree_BatchCull` (`55679a0547649d0b`) and `CS_Prop_BatchCull`
(`17af15c4015cb4b6`) are wave64 programs that, on 32-lane hosts, take the "local waves (idiom)"
form, whose report says "an LDS read and write in one lane-branch region": the two host halves
of a console wave can race on group-shared memory. `VICTORY_WAVE_LOCAL=0` puts all three in the
exact paired form: a hidden body came back, but the dark water blocks stayed and a long spike
polygon appeared, and the GPU time went up (run 192). So that is part of it at most. The
impostor programs themselves translate in their original form (the mesh one has ten
`s_andn2_wrexec_b64` waterfall loops and 22 `v_readfirstlane_b32`).

### Characters

One character's body is still missing in some shots of the boat cutscene (head, neck and
necklace drawn). What was ruled out, with the data:

- The skinning output is consistent. With `FLAMINGO_DUMP_RANGE` (below) the two skinning
  buffers were dumped at frames 2850 and 3000: everything the frame's skinning dispatches write
  (output word: dword 13 of their 96-byte constant buffer, offset in 16-byte units in its low 24
  bits) changes between frames, the rest is old data no mesh points at.
- Every mesh the pre-skinned culls dispatch (from the per-group dwords at `t77[2]`) points at
  skinned data written that frame; the culls' mesh records (`t77[0] + 24 * mesh`, the skinned
  word at +12) and the instance matrices (`t77[1] + 48 * instance`) on the card match title
  memory at the frame's present.
- Not occlusion culling (`FLAMINGO_GATHER_ZERO=1`: the pyramid reads 0), not overlap between
  command lists (`FLAMINGO_LIST_BARRIER=1`), not the relay (`VICTORY_RELAY=0`), no dropped draws.
- About 55-60% of the character mesh draws run with no meshlet groups; many of those are
  probably empty legitimately (level-of-detail variants), so that count is not the measure.

What is left: the cull's per-meshlet decision for that character's meshes (frustum test on the
skinned vertices, the optional back-face test), or the draws themselves. Next step: map each
colour draw's record to its mesh and character, and test that character's meshlets on the CPU
from the dumps.

### Other

- Dark blocks on the water and the horizon remain in some shots even with the impostors left
  out; a large dark triangle sticks up from the boat in one shot; one shot shows broken shards.
- `VICTORY_RELAY=0` turns skin grey and makes eyes glow: do not use it for picture checks.
- The water's GPU-written textures are copied back by default (`texback.rs`); with
  `VICTORY_TEXTURE_BACK=0` the boat sits on the seabed. Keep it on.

## Tools added this round

- `FLAMINGO_DUMP_RANGE=<hex address>+<hex bytes>[,...]`, or `size:<hex bytes>` for the title
  buffer of that size wherever it lies (diagnostics builds, with `VICTORY_CAPTURE` and
  `VICTORY_CAPTURE_FRAMES`): `frame-N-range-<address>.bin` is the range as its host copy held it
  at the frame's first character mesh draw, `frame-N-title-<address>.bin` as title memory held it
  at the frame's present.
- The capture's op lines give a constant buffer of up to 128 bytes in full, and mesh pipelines'
  depth, stencil, cull and blend state; `frame-N-drawn.txt` gives each indirect record as the
  card ran it and as title memory held it while the command was recorded.
- `shader-info --disasm` limited to `FLAMINGO_DISASM_HASHES=<hash prefixes>`.
- Test switches (off by default): `FLAMINGO_GATHER_ZERO=1` (gathers at a named level read 0:
  occlusion off), `FLAMINGO_LIST_BARRIER=1` (a barrier at the start of every list).
- `VICTORY_SKIP_PIPELINES` works in every build.
- `translate --warm` of one hash with `VICTORY_WAVE_WHY=1` and a scratch `VICTORY_CACHE_DIR`
  prints which wave64 form a program takes on a 32-lane host.
