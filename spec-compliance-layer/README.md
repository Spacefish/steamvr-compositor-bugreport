# VK_LAYER_steamvr_spec_compliance

A small, cheap Vulkan layer that makes the SteamVR compositor's resource
recycling spec-correct instead of stalling or faulting because of it.

Today it implements one shim:

**Fence reset while in flight** (`VUID-vkResetFences-pFences-01123`).
The compositor resets fences that still have GPU work attached. The layer
tracks which fences were actually submitted and not yet observed signaled, and
before forwarding `vkResetFences` it waits (bounded) on exactly those. Fences
with no pending work are skipped, so the common path is a hash lookup and never
blocks.

## Why tracking matters

`vkGetFenceStatus(f) == VK_NOT_READY` is *not* enough to decide whether to wait:
a fence that was created or reset and never submitted is also unsignaled, and
waiting on it would stall until the timeout. So the layer keeps an "in flight"
set:

- `vkQueueSubmit` / `vkQueueSubmit2` insert the fence when non-null;
- `vkWaitForFences` / `vkGetFenceStatus` erase it once the app observed it
  signaled;
- `vkResetFences` waits only on fences still in the set whose status is
  `VK_NOT_READY`, then erases them.

The wait cap is 2 s (SteamVR's Render thread watchdog aborts at ~5 s). On
timeout the layer logs and forwards the reset anyway.

## Making the reset visible to validation layers

A layer *below* this one (e.g. `VK_LAYER_KHRONOS_validation`) retires a
submitted fence's "in use" state on a helper thread. If the app polls the fence
to signaled and resets it immediately, validation can still see the fence as in
use and report `VUID-vkResetFences-pFences-01123` even though the driver reports
the fence signaled.

So before forwarding `vkResetFences`, the layer issues `vkDeviceWaitIdle` when
any fence in the batch was ever submitted, letting the lower layer's bookkeeping
catch up. `vkDeviceWaitIdle` is deliberate: on the validation side its handling
(`Queue::NotifyAndWait`) drains the helper thread and has no fence-promise stall
hazard, unlike `Fence::NotifyAndWait` (the 120 s timeout path).

Measured: this removes the `01123` reports in the standalone harness, but **not**
in the SteamVR compositor. There, VVL's per-fence state for those fences is
stuck in `kInflight` (their submissions have already left the queue deque), and
a queue drain cannot retire them. The compositor reports were eliminated by
fixing VVL instead (`PostCallRecordGetFenceStatus` calls `Fence::Retire()` when
the driver reports `VK_SUCCESS`; see `../logs/vvl-fix.patch`, 20 -> 0). The drain
is kept as a safety net for other lower layers, but it is not what fixed the
compositor.

This only works if the layer sits **above** `VK_LAYER_KHRONOS_validation` (it
does when listed before it in the loader settings), so validation sees the reset
after the sync.

## Build

```
./build.sh
```

Produces `libVkLayer_steamvr_spec_compliance.so` and `test_reset_fences`.

## Standalone test

```
env -u VK_ADD_LAYER_PATH -u VK_INSTANCE_LAYERS \
    VK_LAYER_PATH=$(pwd) VK_INSTANCE_LAYERS=VK_LAYER_steamvr_spec_compliance \
    ./test_reset_fences
```

Pass means: never-submitted reset is instant, in-flight reset waited (look for
`[SSC] waiting on in-flight fence ... (#1)` on stderr), and the wait/status fast
paths stay sub-5 ms.

## Enabling for the compositor

Environment variables do not reach `vrcompositor` (see AGENTS.md), so add the
layer to the loader settings file instead:

`~/.local/share/vulkan/loader_settings.d/vk_loader_settings.json`

```json
{
  "file_format_version": "1.0.1",
  "settings": {
    "layers": [
      { "control": "unordered_layer_location" },
      {
        "name": "VK_LAYER_steamvr_spec_compliance",
        "path": "/data2/src/steamvr-hotfixlayer/steamvr_spec_compliance/VkLayer_steamvr_spec_compliance.json",
        "control": "on"
      },
      {
        "name": "VK_LAYER_KHRONOS_validation",
        "path": "/data2/src/Vulkan-ValidationLayers/build/layers/VkLayer_khronos_validation.json",
        "control": "on"
      }
    ]
  }
}
```

Note the ordering: put this layer **before** `VK_LAYER_KHRONOS_validation` so it
sits above validation in the chain. Validation then sees the already-waited
reset and `VUID-vkResetFences-pFences-01123` disappears; with the layer below
validation, validation still records the app's invalid reset call.

## Caveats

- This is **not** the fix for the `gfxhub` page fault this repo chases. That is
  the `unlit_vs.spv` UBO read (see `../AGENTS.md`). Full device serialization
  (`../serialize_test/`) did not prevent the fault, so fence reuse is not the
  trigger.
- Blocking inside `vkResetFences` changes app timing. The cap keeps it well
  under the watchdog, but if the device is wedged the layer can still add up to
  2 s per offending fence (once per fence - timed-out fences are not retried).
- The lower-layer drain is a real `vkDeviceWaitIdle`, issued on reset batches
  that contain a previously submitted fence. It is a genuine device sync (one
  per such batch), so it can add a frame of latency. The compositor's resets are
  a startup burst (~140 in a run), not per-frame, so the cost should be
  confined to startup.
- Fence handles are tracked globally; for a multi-device app a spurious
  cross-device match could cause an extra bounded wait. The compositor uses one
  device.
