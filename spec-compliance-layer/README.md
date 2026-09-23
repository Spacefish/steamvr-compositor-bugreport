# VK_LAYER_steamvr_spec_compliance

A small, cheap Vulkan layer that makes the SteamVR compositor's resource
recycling spec-correct instead of stalling or faulting because of it. It
implements the "recycle a resource while it may still be in use" family: before
forwarding the offending call, wait (bounded, 2 s) for the previous use to
complete.

| Shim | VUIDs | Status |
|---|---|---|
| Fence reset while in flight | `vkResetFences-pFences-01123` | mostly handled in VVL now (retire on `vkGetFenceStatus` success); the layer's wait is the safety net for genuinely pending fences |
| Command buffer reuse | `vkResetCommandBuffer-commandBuffer-00045`, `vkBeginCommandBuffer-commandBuffer-00049`, `vkQueueSubmit-pCommandBuffers-00071` | works - tracked CB -> pending submission fence, waited before reset/begin/submit; verified 0 in the compositor |
| Binary semaphore reuse | `vkAcquireNextImageKHR-semaphore-01286`/`-01779`, `vkQueueSubmit-pSignalSemaphores-00067` | `01779` was a **VVL false positive**, fixed in VVL (polling a fence left the queue's semaphore refcount raised; `Fence::NotifyAndDrainQueue()` now drains it). `01286`/`00067` remain (startup only) - see below |

Image-layout violations (`VkImageMemoryBarrier-oldLayout-01197`,
`vkCmdDraw-None-09600`) are a different family (declared layout vs tracked
layout) and cannot be fixed by any amount of waiting; they stay visible.

## Semaphores: state lag vs. genuinely signaled

Two different things were conflated at first:

- `01779` ("must not have any pending operations") was **validation state lag**,
  the same class as `01123`: VVL's `Semaphore::InUse()` refcount is dropped only
  by the queue thread, so an app that polls a fence to signaled and immediately
  re-acquires still looked like it had a pending operation (uncapped: ~14,000
  reports in 62 s). Instrumentation showed the acquire semaphore *is* waited on in
  submissions (with fences). Fixed in VVL: `Fence::NotifyAndDrainQueue()` drains
  the queue on `vkGetFenceStatus` success, so the submission's semaphore and
  command-buffer refcounts are released before the app recycles them.
- `01286` ("must not be currently signaled") and `00067` (signal while still in
  use by the swapchain) are the genuinely signaled cases: a fence wait cannot
  unsignal a binary semaphore. The only legal remedy is an injected consuming
  submission (empty submit that waits on the semaphore + our fence, then wait for
  it), which changes execution order and masks a real compositor bug. Deliberately
  not done; these occur a couple of times at startup.

## Fence reset while in flight (original shim)

The compositor resets fences that still have GPU work attached. The layer tracks
which fences were actually submitted and not yet observed signaled, and before
forwarding `vkResetFences` it waits (bounded) on exactly those. Fences with no
pending work are skipped, so the common path is a hash lookup and never blocks.
Once the fence is known to be signaled (or the wait succeeded), the command
buffers and semaphores whose last use was tied to it are released - and on a wait
*timeout* nothing is released, so a later reuse still synchronizes instead of
trusting an unverified completion.

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

## The lower-layer device drain was removed

An earlier revision issued `vkDeviceWaitIdle` before forwarding a reset, hoping
to make a lower validation layer retire its fence bookkeeping first. Measured in
the SteamVR compositor it did **not** remove `VUID-vkResetFences-pFences-01123`
(there VVL's fence state can be stuck `kInflight` with its submission already
gone from the queue, which a queue drain cannot retire), and it cost a real
device-wide sync ~2-3 times per second.

It was also masking other validation output: with the drain enabled the
compositor stopped reporting its command-buffer reuse errors
(`VUID-vkResetCommandBuffer-commandBuffer-00045`,
`VUID-vkBeginCommandBuffer-commandBuffer-00049`,
`VUID-vkQueueSubmit-pCommandBuffers-00071`), purely because the device was being
serialized before every reset. With the drain gone those reports are back
(20x each), which is the honest state.

`01123` is fixed where it belongs: in VVL — `PostCallRecordGetFenceStatus` now
calls `Fence::Retire()` when the driver reports `VK_SUCCESS` (see
`../logs/vvl-fix.patch`, 20 -> 0). The layer is inert at runtime in the
compositor (0 fence waits, 0 device waits) and only engages if an application
resets a fence that genuinely still has pending work.

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
