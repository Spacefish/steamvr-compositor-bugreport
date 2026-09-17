# SteamVR compositor GPU fault — evidence bundle

Collected 2026-09-17 for a SteamVR / Vulkan bug report.

## Environment

| | |
|---|---|
| OS | Ubuntu 26.04.1 LTS |
| Kernel | `7.3.0-rc1-spacy2026090103` (custom) |
| GPU | AMD Radeon RX 7800 XT (Navi 32), `radeon_icd` |
| Driver | RADV, Mesa 26.0.8-1ubuntu0.3 |
| SteamVR | 2.17.10 (build 1789498647) |
| Validation | Khronos VVL built from `vulkan-sdk-1.4.341.0` + commit `3786adc` |
| Feature | headset: Valve Index |

## Summary

`vrcompositor` crashes with an AMD GPU page fault while rendering the Lens
Distortion pass. `dmesg`:

```
amdgpu ...: [gfxhub] page fault (src_id:0 ring:24 vmid:5 pasid:NNN)
amdgpu ...:  Process vrcompositor pid NNN thread RenderThread pid NNN
amdgpu ...:   in page starting at address 0x0000008001391000 from client 10
amdgpu ...: GCVM_L2_PROTECTION_FAULT_STATUS:0x00501430
amdgpu ...:   Faulty UTCL2 client ID: SQC (data) (0xa)
amdgpu ...:   PERMISSION_FAULTS:0x3   MAPPING_ERROR:0x0
amdgpu ...: ring gfx_0.0.0 timeout ... -> ring reset -> device wedged
```

then `vrcompositor.txt`:

```
Failed Watchdog timeout in thread Render in Present after 6.5 seconds. Aborting.
```

Repro: start SteamVR and enter Theater mode (`bin/vrstartup.sh`).

## Root cause chain (from validation + SPIR-V dumps)

1. `VS_DISTORT` (vertex) writes `gl_Layer`, but the framebuffer is created with
   `VkFramebufferCreateInfo::layer = 1`, so the value is undefined
   (`Undefined-Value-Layer-Written`).
2. `PS_DISTORT` (fragment) loads the layer value into a flat uint varying and
   `OpSwitch`es on it to pick `Set 0, Binding 9` index 0 / 1 / 2
   (`OpAccessChain ... %int_0|%int_1|%int_2`), then `OpImageSampleImplicitLod`.
   The SPIR-V declares binding 9 as `OpTypeArray ... %uint_3` (3 images).
3. The pipeline's `VkDescriptorSetLayoutBinding::descriptorCount` for binding 9
   is **1** (`VUID-VkGraphicsPipelineCreateInfo-layout-07991`).
4. Index 1 was never written with `vkUpdateDescriptorSets`
   (`VUID-vkCmdDrawIndexed-None-08114`) and GPU-AV catches index 2 in use
   (`VUID-vkCmdDrawIndexed-None-10068`, region
   `Warp: L::Scene::Scene / Lens Distortion`).
5. Sampling the resulting garbage image descriptor makes the texture unit
   (`SQC (data)`) read a bad address → the `gfxhub` page fault.

Also reported: `VUID-vkCmdDrawIndexed-None-02721` (GPU-AV vertex fetch OOB,
index 1680 with 1680 vertices), `VUID-vkCmdDraw-imageLayout-00344` (layout
tracking; sync validation found no matching memory hazard),
`SYNC-HAZARD-WRITE-AFTER-WRITE` on a `vkCmdUpdateBuffer` uniform buffer, and
fence/command-buffer reuse violations (`vkResetFences-pFences-01123`,
`vkResetCommandBuffer-00045`, `vkBeginCommandBuffer-00049`,
`vkQueueSubmit-pCommandBuffers-00071`, `vkAcquireNextImageKHR-semaphore-01779`).

## Contents

| File | Description |
|---|---|
| `vrcompositor-linux.txt` | Compositor stdout/stderr with core + sync + GPU-AV validation interleaved (crashing run) |
| `vvl-compositor-core.log` | Earlier run, core validation only, redirected via `log_filename` |
| `vvl-compositor.log` | Earlier run, core + GPU-AV, redirected via `log_filename` |
| `vrcompositor.txt` | Compositor's own log (watchdog abort) |
| `vrserver.txt` | `vrserver` log |
| `vrstartup-linux.txt` | SteamVR startup log |
| `dmesg-full.txt` | Full `dmesg -T` (must run as root on this box) |
| `dmesg-amdgpu.txt` | `dmesg` filtered to amdgpu / page fault / ring / SQC lines |
| `vulkaninfo-summary.txt` | Vulkan instance/device info (GPU, driver, versions) |
| `versions.txt` | OS / kernel / GPU / Mesa / SteamVR / VVL versions |
| `steamvr-systemreport.txt` | SteamVR system report (`~/SteamVR-2026-09-17-AM_02_40_44.txt`) |
| `settings/loader_settings.json` | `~/.local/share/vulkan/loader_settings.d/vk_loader_settings.json` (enables VVL without env) |
| `settings/vk_layer_settings.txt` | `~/.local/share/vulkan/settings.d/vk_layer_settings.txt` (core + sync + GPU-AV) |
| `spirv-dumps/` | `dump_N_before/after.spv` for `VS_DISTORT` / `PS_DISTORT` + `spirv-dis` disassembly |
| `crashdumps/` | breakpad minidumps for the last two crashes |
| `vvl-fix.patch` | Local VVL patch (non-blocking `vkGetFenceStatus`) used for these runs |

## Notes on enabling validation

Env vars do **not** reach `vrcompositor`: it is spawned via
`steam-runtime-launch-client --alongside-steam --pass-env=LD_LIBRARY_PATH`, so
only `LD_LIBRARY_PATH` is forwarded. Validation is enabled through the loader
settings file, and configured through the VVL settings file (flat
`khronos_validation.<key> = <value>` form; a `[section]` header is ignored).
See the settings/ files above.
