# Reproduction on SteamVR 2.18.1 (build 25466322)

SteamVR was updated on 2026-09-24 (`buildid 25466322`, `LastUpdated 1790208373`,
version notice 2.18.1). To check whether the update fixed this bug, the shader
workaround was removed (`../workaround/restore.sh` plus Steam "verify integrity",
which re-downloaded the stock `unlit_vs.spv`, md5
`cc85c34c275ffe34462753a36abb47a3`). **It did not** — the crash is identical.

Signature (2026-09-24 02:19 local):

```
amdgpu 0000:0d:00.0: [gfxhub] page fault (src_id:0 ring:24 vmid:5 pasid:1305)
amdgpu 0000:0d:00.0:   in page starting at address 0x00000080013a0000 from client 10
amdgpu 0000:0d:00.0:   Faulty UTCL2 client ID: SQC (data) (0xa)
amdgpu 0000:0d:00.0:   PERMISSION_FAULTS: 0x3   MAPPING_ERROR: 0x0
amdgpu 0000:0d:00.0: ring gfx_0.0.0 timeout, signaled seq=9931119, emitted seq=9931121
amdgpu 0000:0d:00.0:  Process vrcompositor pid 287087 thread RenderThread pid 287145
amdgpu 0000:0d:00.0: [drm] device wedged, but no recovery needed
```

```
vrcompositor.txt:
  WaitForPendingPresent: vkWaitForPresent failed, disabling wait-for-present mode. VkResult = -4
  Failed WaitEvent(CompositorPresent)! event=1 vkerror=-4
  Failed GetDeltas(CompositorPresent): vkerror=-4
  Failed Watchdog timeout in thread Render in RenderDistort after 5.532892 seconds. Aborting.
```

The faulting VA is in the same range as every earlier run (`0x80013xxxxx`) and
the `SQC (data)` client matches the `unlit_vs.spv` `Set 0 / Binding 0` UBO read
documented in `../README.md`. Re-applying the workaround (`../workaround/apply.sh`,
`4feb612c`) makes 2.18.1 run again — so the files here are the "before" state.

## Files

| Path | Description |
|---|---|
| `crash-stock-shader.txt` | Manifest/build, the dmesg fault + ring-reset lines and the compositor abort lines from this crash |
| `devcoredump.txt` | amdgpu devcoredump for this crash (`/sys/class/drm/card1/device/devcoredump/data` is a text coredump; 36k lines: fault VA/protection status, faulting IB, IP register dump, ring contents) |
