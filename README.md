# SteamVR compositor GPU fault — evidence bundle

Collected 2026-09-17 for a SteamVR / RADV bug report.

## Environment

| | |
|---|---|
| OS | Ubuntu 26.04.1 LTS |
| Kernel | `7.3.0-rc1-spacy2026090103` (custom) |
| GPU | AMD Radeon RX 7800 XT (Navi 32), `radeon_icd` |
| Driver | RADV, Mesa 26.0.8-1ubuntu0.3 |
| SteamVR | 2.17.10 (build 1789498647) |
| Validation | Khronos VVL built from `vulkan-sdk-1.4.341.0` + commit `3786adc` |
| Headset | Valve Index |

## Summary

`vrcompositor`'s `RenderThread` takes an AMD GPU page fault and the GPU wedges;
SteamVR's watchdog then aborts the compositor:

```
amdgpu ...: [gfxhub] page fault (src_id:0 ring:24 vmid:N pasid:NNN)
amdgpu ...:  Process vrcompositor pid NNN thread RenderThread pid NNN
amdgpu ...:   in page starting at address 0x00000080012f6000 from client 10
amdgpu ...: GCVM_L2_PROTECTION_FAULT_STATUS:0x00601430
amdgpu ...:   Faulty UTCL2 client ID: SQC (data) (0xa)
amdgpu ...:   PERMISSION_FAULTS:0x3   MAPPING_ERROR:0x0
amdgpu ...: ring gfx_0.0.0 timeout ... -> ring reset -> device wedged
```
```
Failed Watchdog timeout in thread Render in Present after ~6 s. Aborting.
```

Repro: start SteamVR (`SteamVR/bin/vrstartup.sh`); with a headset connected it
faults within ~10-40 s (entering Theater mode makes it deterministic).

## Leading evidence (RADV GPU hang dump)

`RADV_DEBUG=hang,umr_ring,bo_history` in
`SteamVR/bin/linux64/vrcompositor-launcher.sh` makes RADV write
`~/radv_dumps_<pid>_<timestamp>/` on the hang (captured here in
`radv-hang-report/`):

- `vm_fault.log`: failing page `0x80012f6000`, status `0x601430`, client
  `SQC (data)`, and RADV's own address-binding-report check reports
  **`VA not found!`** — the faulting address is **not any RADV BO** (live or
  destroyed, including imported BOs).
- Guilty pipeline shaders: `VS_MAIN` (vertex, writes `gl_Layer`) and `PS_MAIN`
  (fragment, reads `gl_Layer`, samples `Set 0 / Binding 9` and
  `Set 0 / Binding 25` with `OpImageSampleExplicitLod`). No
  `PhysicalStorageBuffer`/BDA in the application shader.
- `trace.log`: full PM4 decode; RADV marks trace point 103 as the last one
  reached by the CP, and the faulting work is a `DRAW_INDEX_AUTO` (4-index
  fullscreen draw) right after it.
- `bo_history.log` / `addr_binding_report.log`: RADV BO GPU-VA map, to check a
  VA against real mappings.

Conclusion: the texture unit dereferenced an address that is not a valid
mapping — i.e. a **bogus address reached from the draw**, not a normal
out-of-bounds access.

## Root cause: vertex shader UBO read (`Set 0 / Binding 0`)

Hand-patching `unlit_vs.spv` isolated the faulting dereference. Three variants
(all confirmed by running SteamVR and checking dmesg/RADV hang dumps):

| Variant | UBO read (`Set 0/Binding 0`) | `gl_Layer` stored | Result |
|---|---|---|---|
| original | yes (`%108 = OpLoad %uint %107`) | value from UBO | crash |
| A: `%108 = OpCopyObject %uint %uint_0` | removed | 0 | **no crash** |
| B: `%108 = OpLoad` kept, store `%108 >> 31` | kept | ~0 | **crash** |
| C: `%108 = OpCopyObject %uint %uint_1` | removed | 1 | **no crash** |

So the fault is the **vertex shader's UBO access itself** — the descriptor /
buffer address behind `Set 0 / Binding 0` — and *not* the `gl_Layer` value:
`gl_Layer = 0` and `gl_Layer = 1` both survive, and only keeping the UBO load
brings the fault back. It is also not the fragment texture fetch: patching
`unlit_gaussian_blur_u_ps.spv` to drop all three `OpImageSampleExplicitLod` made
no difference (same fault, same pass, patched module confirmed in the new RADV
hang dump).

Workaround: patch `unlit_vs.spv` so it no longer reads `Set 0 / Binding 0`
(`unlit_vs.noubo.spv`, stores a constant 0 to `gl_Layer`). With it installed the
compositor runs with no `gfxhub` fault and no watchdog abort.

- original `unlit_vs.spv`: md5 `cc85c34c275ffe34462753a36abb47a3`
- patched (workaround): md5 `4feb612cac1a8b052ec6e0d44f47070e`

Artifacts: `spirv-dumps/unlit_vs.noubo.spv` (+`.dis`), `unlit_vs.shift0.spv`,
`unlit_vs.layer1.spv`, and `unlit_vs.spv.orig` for restoring the shipped shader.

## Earlier shader-patch experiments

- **Binding 9 descriptor index 2 (GPU-AV `10068`) is not the crash.** Patching
  the selected shader so every `Binding 9` access uses index 0 removed `10068`
  (20 -> 0) but the fault persisted.
- **DCC / compression state is not the crash.** `RADV_DEBUG=nodcc` made no
  difference.
- **Synchronization hazards are not the cause.** Sync validation reports no
  image hazards (only `SYNC-HAZARD-WRITE-AFTER-WRITE` on a `vkCmdUpdateBuffer`
  uniform buffer).
- **Resource recycling is not a race.** A layer that forces `vkDeviceWaitIdle`
  before every `vkResetFences` / `vkResetCommandBuffer` / `vkBeginCommandBuffer`
  / `vkQueueSubmit` (12,500+ serializations) did not prevent the fault, so it is
  deterministic inside a single correctly-ordered submission.
- **Descriptor use-after-free of destroyed objects is not it.** A lifetime layer
  showed the compositor destroys an imageView (binding 9), a buffer (0/4) and a
  sampler (25) while a set still references them, but that set is never
  submitted (`USED-AFTER-DESTROY` = 0).
- **The GPU-AV vertex-fetch OOB (`02721`) is unrelated** — it is in a different
  pass; the faulting shader is the fragment texture-sampling pass, and the fault
  VA is not any BO at all (a 4-byte buffer over-read would stay in a mapping).

## Contents

| Path | Description |
|---|---|
| `radv-hang-report/` | **Clean** RADV hang dump (core validation only): `vm_fault.log`, `trace.log`, `pipeline.log`, `registers.log`, `bo_history.log`, `addr_binding_report.log`, `gpu_info.log`, app `.spv` shaders |
| `crashdumps/` | breakpad minidumps, `amdgpu-devcoredump.txt` (text coredump: fault VA/status, faulting IB, IP register dump, ring contents), and files from the first (GPU-AV) hang dump |
| `radv-bo-history.log` | `/tmp/radv_bo_history.log` from `RADV_DEBUG=bo_history` (every BO GPU-VA range) |
| `spirv-dumps/` | `VS_DISTORT`/`PS_DISTORT` dumps + disassembly; `distort_ps_layered.spv.orig` / `.patched.*` (the index-2 experiment) |
| `spirv-dumps/radv-hang/` | Guilty shaders from the GPU-AV-instrumented hang dump (`inst_post_process_descriptor_index`, BDA pointer array) |
| `spirv-dumps/radv-hang-clean/` | Guilty shaders from the clean hang dump (application `VS_MAIN` / `PS_MAIN`) |
| `spirv-dumps/unlit_vs.*` | `unlit_vs.spv` original (`cc85c34c…`), patched `.noubo.spv`/`.dis` (crash goes away), md5 |
| `spirv-dumps/unlit_gaussian_blur_u_ps.*` | `unlit_gaussian_blur_u_ps.spv` original (`d7456caa…`) and the no-sample patch that did *not* help |
| `vrcompositor-linux.txt` | Compositor stdout/stderr with validation interleaved (one crashing run) |
| `vrcompositor-linux_no_sync_layers.txt`, `vrcompositor-linux_no_shadersdump.txt` | Earlier runs |
| `vvl-compositor-core.log`, `vvl-compositor.log` | Earlier runs with VVL `log_filename` redirection |
| `vrcompositor.txt`, `vrserver.txt`, `vrstartup-linux.txt` | Compositor's own log (watchdog abort) and SteamVR logs |
| `dmesg-full.txt`, `dmesg-amdgpu.txt` | Kernel log / amdgpu fault lines |
| `vulkaninfo-summary.txt`, `versions.txt` | GPU, driver, loader, versions |
| `steamvr-systemreport.txt` | SteamVR system report |
| `settings/loader_settings.json`, `settings/vk_layer_settings.txt` | How VVL was enabled / configured (no env reaches the compositor) |
| `vrcompositor-launcher.sh.orig` | Backup of the original compositor launcher (hash `f210c50b…`) |
| `vvl-fix.patch` | Local VVL patch (non-blocking `vkGetFenceStatus`) needed so validation doesn't stall the compositor |

## Notes on running validation / debug options

Env vars do **not** reach `vrcompositor`: it is spawned via
`steam-runtime-launch-client --alongside-steam --pass-env=LD_LIBRARY_PATH`, so
only `LD_LIBRARY_PATH` is forwarded. Validation is enabled through the loader
settings file; VVL settings use the flat `khronos_validation.<key> = <value>`
form. To pass `RADV_DEBUG` (or `STEAMVR_*`) to the compositor you must export it
inside `SteamVR/bin/linux64/vrcompositor-launcher.sh`.
