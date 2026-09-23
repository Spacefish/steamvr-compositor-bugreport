# Follow-up: is `VUID-vkResetFences-pFences-01123` the trigger? (2026-09-23)

## Question

Core validation reports `vkResetFences(): pFences[0] (VkFence …) is in use.` on the
SteamVR compositor (20x per run, always at startup). The hypothesis was that the
compositor recycles fences that still have GPU work attached, and that waiting in
`vkResetFences` before resetting would repair it.

## Method

A small layer was built to instrument and then repair the recycling:

- `spec-compliance-layer/` — `VK_LAYER_steamvr_spec_compliance`.
- It records which fences were submitted (`vkQueueSubmit` / `vkQueueSubmit2`) and
  which the app has observed signaled (`vkWaitForFences` / `vkGetFenceStatus`), so
  it knows which resets target pending work, and can wait only on those (2 s cap,
  under SteamVR's ~5 s watchdog).
- Diagnostic build additionally logs, per reset: `tracked`, `ever_submitted`,
  `observed`, and the driver `vkGetFenceStatus` result.

Enabled through `settings/loader_settings_with_spec_compliance.json` (env vars do
not reach the compositor). The layer sits above `VK_LAYER_KHRONOS_validation`,
which lets it act before validation checks the reset.

## Findings

From a compositor run with the layer (raw excerpt:
`ssc-fence-classification.txt`):

| | |
|---|---|
| reset fences classified | 141 |
| resets that needed a wait | **0** |
| driver status at reset time | `VK_SUCCESS` for **all** resets |
| app had already observed the fence signaled | 62/86 instrumented (up to 596 polls per fence) |
| `VUID-vkResetFences-pFences-01123` reports | 20 (unchanged from runs without the layer) |
| `gfxhub` page fault | none in that run |

So the compositor does **not** reset pending fences at runtime. Every reset was on a
fence the driver reported signaled, and usually one the application had already polled
to signaled itself — a legal reset.

The 20 `01123` reports are validation state that lags the driver. In VVL the check is
`fence_state->State() == kInflight`, and that state is cleared by the queue thread's
`Retire()`. The local VVL patch (`../vvl-fix.patch`, `vkGetFenceStatus` must not block)
replaced the blocking wait with a plain `Notify()`, so a poll-to-signaled no longer
retires the fence synchronously; if the app resets immediately after the poll, VVL still
sees `kInflight` and reports `01123`. Resets of never-submitted (but signaled) fences are
reported too, for the same reason.

This is consistent with the earlier elimination: a serialization layer that called
`vkDeviceWaitIdle` before every `vkResetFences` / `vkResetCommandBuffer` /
`vkBeginCommandBuffer` / `vkQueueSubmit` (12,500+ waits in ~2 s) did **not** prevent the
`gfxhub` page fault. Fence/command-buffer recycling is not the fault trigger; the fault
is the `unlit_vs.spv` UBO read documented in `../README.md`.

## Fix in the layer

`vkResetFences` now drains the lower layer's bookkeeping before forwarding the reset:

```cpp
if (g_DeviceWaitIdle != nullptr && any_ever_submitted(pFences, fenceCount))
    g_DeviceWaitIdle(device);           // let lower layers retire the fence first
return g_ResetFences(device, fenceCount, pFences);
```

`vkDeviceWaitIdle` is deliberate: on the validation side its record phase drains the
queue thread (`Queue::NotifyAndWait`) and has no fence-promise stall hazard — unlike
`Fence::NotifyAndWait` (`vkWaitForFences`), which is the path that timed out for 120 s
and originally killed the compositor via the watchdog.

Verified in the standalone harness (`spec-compliance-layer/test_reset_fences.cpp`):

```
[SSC] vkDeviceWaitIdle before vkResetFences so lower layers retire the fence (#1)
ALL CHECKS PASSED
timings: d1=0.03 d2=0.06 d3=0.00 d4=1.49 d5=0.06 (ms)
```

`VUID-vkResetFences-pFences-01123` no longer appears in the harness (it fired on the
poll-then-reset cases before the change).

## Not verified on the compositor

A compositor run with the fix could not be collected: the Steam client was updated to
the steamrt3/`steamrt64` build (1790121765) during this session, after which SteamVR
2.17.10 cannot launch its compositor at all. Its launcher service is now published only
as `com.steampowered.PressureVessel.LaunchAlongsideSteam.Instance<PID>`, and
`steam-runtime-launch-client --alongside-steam` (used by `vrserver` to spawn
`vrcompositor-launcher.sh`) cannot reach it:

```
steam-runtime-launch-client[...]: E: Unable to connect to any of the specified bus names
        (is steam-runtime-launcher-service running?)
```

`--bus-name=…Instance<PID>` works from a host shell, `--alongside-steam` does not; the
`SRT_LAUNCHER_SERVICE_ALONGSIDE_STEAM` variable reaches `vrserver` inside the sniper
container but the launch still fails. This is a client/runtime incompatibility, not a
layer problem. Once SteamVR can start again, the check is: `01123` count 0 and
`[SSC] vkDeviceWaitIdle before vkResetFences` lines at startup.

## Files

| Path | Description |
|---|---|
| `ssc-fence-classification.txt` | Raw reset classifications + counts from the compositor run |
| `../spec-compliance-layer/` | Layer source (`steamvr_spec_compliance.cpp`), manifest, `build.sh`, README, standalone test |
| `../settings/loader_settings_with_spec_compliance.json` | Loader settings that enable the layer above VVL |
