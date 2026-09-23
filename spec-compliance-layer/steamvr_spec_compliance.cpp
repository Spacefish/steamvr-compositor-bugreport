// VK_LAYER_steamvr_spec_compliance
//
// A small, cheap spec-compliance layer for the SteamVR compositor. It fixes the
// "recycle a resource while it may still be in use" family of violations by
// waiting (bounded) for the previous use to complete before forwarding the call:
//   * fences           - VUID-vkResetFences-pFences-01123
//   * command buffers  - VUID-vkResetCommandBuffer-commandBuffer-00045,
//                        VUID-vkBeginCommandBuffer-commandBuffer-00049,
//                        VUID-vkQueueSubmit-pCommandBuffers-00071
//   * binary semaphores- VUID-vkAcquireNextImageKHR-semaphore-01286 / -01779,
//                        VUID-vkQueueSubmit-pSignalSemaphores-00067
// Image-layout violations (VkImageMemoryBarrier-oldLayout-01197,
// vkCmdDraw-None-09600) are NOT this family: they are declared-layout/barrier
// errors that no amount of waiting can fix, and stay visible on purpose.
//
// Strategy (kept cheap on purpose):
//   * vkQueueSubmit / vkQueueSubmit2 records the fence in an "in flight" set
//     when it is non-null (that is the only way a fence can acquire pending
//     work).
//   * vkWaitForFences / vkGetFenceStatus clear it once the app has observed it
//     signaled, so a later reset does not wait again.
//   * vkResetFences only blocks on fences that are still in the in-flight set
//     and that vkGetFenceStatus says are VK_NOT_READY. A fence that was never
//     submitted (or was already waited on) is a single hash lookup + skip, so
//     the common path does not stall.
//
// The wait is bounded (2 s) because the compositor's Render thread watchdog
// aborts at ~5 s; on timeout we log and forward the reset anyway.
//
// NOT a crash fix: the gfxhub page fault this repo chases is the unlit_vs.spv
// UBO read (see AGENTS.md). Full device serialization (serialize_test/) did not
// prevent it, so fence reuse is not the fault trigger. This layer is about spec
// correctness / robustness of the resource recycling, not the page fault.
//
// Enable via loader settings or VK_INSTANCE_LAYERS; see README.md.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

// SteamVR's watchdog aborts a thread after ~5 s, so never block longer.
constexpr uint64_t kWaitTimeoutNs = 2'000'000'000ull;
constexpr unsigned long kLogEvery = 500;

PFN_vkGetInstanceProcAddr g_next_gipa = nullptr;
PFN_vkGetDeviceProcAddr g_next_gdpa = nullptr;

VkDevice g_device = VK_NULL_HANDLE;
PFN_vkGetFenceStatus g_GetFenceStatus = nullptr;
PFN_vkWaitForFences g_WaitForFences = nullptr;
PFN_vkResetFences g_ResetFences = nullptr;
PFN_vkQueueSubmit g_QueueSubmit = nullptr;
PFN_vkQueueSubmit2 g_QueueSubmit2 = nullptr;
PFN_vkDestroyFence g_DestroyFence = nullptr;
PFN_vkDeviceWaitIdle g_DeviceWaitIdle = nullptr;
PFN_vkQueueWaitIdle g_QueueWaitIdle = nullptr;
PFN_vkAllocateCommandBuffers g_AllocateCommandBuffers = nullptr;
PFN_vkFreeCommandBuffers g_FreeCommandBuffers = nullptr;
PFN_vkBeginCommandBuffer g_BeginCommandBuffer = nullptr;
PFN_vkResetCommandBuffer g_ResetCommandBuffer = nullptr;
PFN_vkCreateSemaphore g_CreateSemaphore = nullptr;
PFN_vkDestroySemaphore g_DestroySemaphore = nullptr;
PFN_vkAcquireNextImageKHR g_AcquireNextImageKHR = nullptr;
PFN_vkAcquireNextImage2KHR g_AcquireNextImage2KHR = nullptr;

std::mutex g_lock;
std::unordered_set<VkFence> g_in_flight;      // submitted, not yet seen signaled
std::unordered_set<VkFence> g_ever_submitted;  // diagnostic: every fence seen in a submit
std::unordered_map<VkFence, unsigned> g_observed;  // diagnostic: app observed it signaled

// Resources whose last use may still be executing. `fence` is the fence of the
// submission that used the resource (VK_NULL_HANDLE if that submit had none, in
// which case a queue-idle fallback is used); queue is used for that fallback.
struct PendingUse {
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
};
std::unordered_map<VkCommandBuffer, PendingUse> g_cbs;
std::unordered_map<VkSemaphore, PendingUse> g_semaphores;  // binary semaphores only
std::unordered_map<VkSemaphore, unsigned> g_sem_diag;      // diagnostic: events per semaphore

std::atomic<unsigned long> g_submitted{0};   // submits that carried a fence
std::atomic<unsigned long> g_waited{0};      // blocking waits we performed
std::atomic<unsigned long> g_timeouts{0};    // waits that hit the cap
std::atomic<unsigned long> g_no_pending{0};  // resets of fences with no pending work
std::atomic<unsigned long> g_reset_fences{0};  // fences passed to vkResetFences
std::atomic<unsigned long> g_drains{0};        // device waits issued to sync lower-layer state
std::atomic<unsigned long> g_cb_waits{0};      // waits before reusing a command buffer
std::atomic<unsigned long> g_sem_waits{0};     // waits before reusing a binary semaphore
std::atomic<unsigned long> g_unknown_sync{0};  // reuses with nothing observable to wait on

void forget_fence(VkFence fence);  // defined with the resource-reuse shims below
void release_fence_resources(VkFence fence);
void cb_wait_for_reuse(VkCommandBuffer cb);
void sem_diag(VkSemaphore semaphore, const char* event, VkQueue queue, VkFence fence);
void sem_wait_for_reuse(VkSemaphore semaphore);
void cb_mark_pending(VkCommandBuffer cb, VkQueue queue, VkFence fence);
void sem_mark_pending(VkSemaphore semaphore, VkQueue queue, VkFence fence);

void note_in_flight(VkFence fence) {
  if (fence == VK_NULL_HANDLE) return;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    g_in_flight.insert(fence);
    g_ever_submitted.insert(fence);
  }
  const unsigned long n = ++g_submitted;
  if (n == 1 || n % kLogEvery == 0) {
    std::lock_guard<std::mutex> guard(g_lock);
    fprintf(stderr, "[SSC] %lu submitted fences, %zu in flight\n", n, g_in_flight.size());
    fflush(stderr);
  }
}

void note_signaled(VkFence fence, bool observed = false) {
  if (fence == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> guard(g_lock);
  g_in_flight.erase(fence);
  if (observed) ++g_observed[fence];
}

bool is_in_flight(VkFence fence) {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_in_flight.count(fence) != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL SscGetFenceStatus(VkDevice device, VkFence fence) {
  const VkResult result = g_GetFenceStatus(device, fence);
  if (result == VK_SUCCESS) note_signaled(fence, true);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL SscWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences,
                                                VkBool32 waitAll, uint64_t timeout) {
  const VkResult result = g_WaitForFences(device, fenceCount, pFences, waitAll, timeout);
  if (result == VK_SUCCESS) {
    for (uint32_t i = 0; i < fenceCount; ++i) note_signaled(pFences[i], true);
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL SscResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences) {
  // NOTE: this used to issue vkDeviceWaitIdle here to make a lower validation layer retire its
  // fence bookkeeping before the reset. That did not work in the SteamVR compositor (the layer's
  // fence state can be stuck kInflight with its submission already gone from the queue dequeue,
  // which a queue drain cannot retire) and it cost a real device-wide sync a few times per
  // second. The correct fix lives in VVL: PostCallRecordGetFenceStatus retires the fence when
  // the driver reports VK_SUCCESS, which removes the false
  // VUID-vkResetFences-pFences-01123 reports.
  for (uint32_t i = 0; i < fenceCount; ++i) {
    const VkFence fence = pFences[i];
    const bool tracked = is_in_flight(fence);
    const VkResult status = g_GetFenceStatus(device, fence);
    const unsigned long idx = ++g_reset_fences;
    if (idx <= 64 || idx % 256 == 0) {
      std::lock_guard<std::mutex> guard(g_lock);
      const bool ever = g_ever_submitted.count(fence) != 0;
      const unsigned observed = g_observed.count(fence) ? g_observed[fence] : 0u;
      fprintf(stderr, "[SSC] vkResetFences fence %p: tracked=%d ever_submitted=%d observed=%u status=%d\n", (void*)fence,
              (int)tracked, (int)ever, observed, (int)status);
      fflush(stderr);
    }
    if (!tracked) {
      // Not in flight: either never submitted, or the app already observed it
      // signaled (our wrappers removed it). Only claim its work is done when the
      // driver agrees.
      if (status == VK_SUCCESS) release_fence_resources(fence);
      ++g_no_pending;
      continue;
    }
    // Submitted earlier, but it may already be signaled - don't block for that.
    if (status != VK_NOT_READY) {
      note_signaled(fence);
      if (status == VK_SUCCESS) release_fence_resources(fence);
      continue;
    }
    const unsigned long n = ++g_waited;
    if (n <= 5 || n % kLogEvery == 0) {
      fprintf(stderr, "[SSC] waiting on in-flight fence %p before vkResetFences (#%lu)\n", (void*)fence, n);
      fflush(stderr);
    }
    const VkResult wait_result = g_WaitForFences(device, 1, &fence, VK_TRUE, kWaitTimeoutNs);
    if (wait_result != VK_SUCCESS) {
      ++g_timeouts;
      fprintf(stderr, "[SSC] WARNING: fence %p not signaled in %llu ms (VkResult %d), resetting anyway\n", (void*)fence,
              (unsigned long long)(kWaitTimeoutNs / 1000000ull), (int)wait_result);
      fflush(stderr);
      // Do NOT claim completion here: the work may still be running, so leave the
      // resources that were tied to this fence marked pending.
    } else {
      // The wait succeeded, so this fence's work is done and so are the command
      // buffers / semaphores whose last use was tied to it.
      release_fence_resources(fence);
    }
    note_signaled(fence);  // never block on this fence again
  }
  return g_ResetFences(device, fenceCount, pFences);
}

VKAPI_ATTR VkResult VKAPI_CALL SscQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                              VkFence fence) {
  // Make the submit legal: wait for command buffers being resubmitted and binary
  // semaphores being signalled whose previous use may still be pending.
  for (uint32_t i = 0; i < submitCount; ++i) {
    const VkSubmitInfo& submit = pSubmits[i];
    for (uint32_t j = 0; j < submit.commandBufferCount; ++j) cb_wait_for_reuse(submit.pCommandBuffers[j]);
    for (uint32_t j = 0; j < submit.signalSemaphoreCount; ++j) sem_wait_for_reuse(submit.pSignalSemaphores[j]);
  }
  note_in_flight(fence);
  const VkResult result = g_QueueSubmit(queue, submitCount, pSubmits, fence);
  if (result != VK_SUCCESS) {
    note_signaled(fence);
    return result;
  }
  for (uint32_t i = 0; i < submitCount; ++i) {
    const VkSubmitInfo& submit = pSubmits[i];
    for (uint32_t j = 0; j < submit.commandBufferCount; ++j) cb_mark_pending(submit.pCommandBuffers[j], queue, fence);
    for (uint32_t j = 0; j < submit.signalSemaphoreCount; ++j) {
      sem_diag(submit.pSignalSemaphores[j], "submit-signal", queue, fence);
      sem_mark_pending(submit.pSignalSemaphores[j], queue, fence);
    }
    for (uint32_t j = 0; j < submit.waitSemaphoreCount; ++j) {
      sem_diag(submit.pWaitSemaphores[j], "submit-wait", queue, fence);
      sem_mark_pending(submit.pWaitSemaphores[j], queue, fence);
    }
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL SscQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                               VkFence fence) {
  if (g_QueueSubmit2 == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
  for (uint32_t i = 0; i < submitCount; ++i) {
    const VkSubmitInfo2& submit = pSubmits[i];
    for (uint32_t j = 0; j < submit.commandBufferInfoCount; ++j) {
      cb_wait_for_reuse(submit.pCommandBufferInfos[j].commandBuffer);
    }
    for (uint32_t j = 0; j < submit.signalSemaphoreInfoCount; ++j) {
      sem_wait_for_reuse(submit.pSignalSemaphoreInfos[j].semaphore);
    }
  }
  note_in_flight(fence);
  const VkResult result = g_QueueSubmit2(queue, submitCount, pSubmits, fence);
  if (result != VK_SUCCESS) {
    note_signaled(fence);
    return result;
  }
  for (uint32_t i = 0; i < submitCount; ++i) {
    const VkSubmitInfo2& submit = pSubmits[i];
    for (uint32_t j = 0; j < submit.commandBufferInfoCount; ++j) {
      cb_mark_pending(submit.pCommandBufferInfos[j].commandBuffer, queue, fence);
    }
    for (uint32_t j = 0; j < submit.signalSemaphoreInfoCount; ++j) {
      sem_diag(submit.pSignalSemaphoreInfos[j].semaphore, "submit-signal", queue, fence);
      sem_mark_pending(submit.pSignalSemaphoreInfos[j].semaphore, queue, fence);
    }
    for (uint32_t j = 0; j < submit.waitSemaphoreInfoCount; ++j) {
      sem_diag(submit.pWaitSemaphoreInfos[j].semaphore, "submit-wait", queue, fence);
      sem_mark_pending(submit.pWaitSemaphoreInfos[j].semaphore, queue, fence);
    }
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL SscDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* pAllocator) {
  forget_fence(fence);
  note_signaled(fence);
  g_DestroyFence(device, fence, pAllocator);
}

// ---- resource-reuse shims ---------------------------------------------------
//
// See the file header. Before an application recycles a command buffer or a
// binary semaphore, wait (bounded) for the previous use to complete. Resources
// that were never submitted, or whose previous use already completed, are a hash
// lookup and a non-blocking status check - no stall.

// Must not be called with g_lock held.
void wait_for_previous_use(VkDevice device, VkQueue queue, VkFence fence, const char* what, const char* who,
                           std::atomic<unsigned long>& counter) {
  const unsigned long n = ++counter;
  if (n <= 5 || n % kLogEvery == 0) {
    fprintf(stderr, "[SSC] waiting before reusing a %s in %s (#%lu)\n", what, who, n);
    fflush(stderr);
  }
  if (fence != VK_NULL_HANDLE && g_GetFenceStatus != nullptr && g_WaitForFences != nullptr) {
    if (g_GetFenceStatus(device, fence) != VK_NOT_READY) return;  // previous use already done
    const VkResult wait_result = g_WaitForFences(device, 1, &fence, VK_TRUE, kWaitTimeoutNs);
    if (wait_result != VK_SUCCESS) {
      ++g_timeouts;
      fprintf(stderr, "[SSC] WARNING: %s fence %p not signaled in %llu ms (VkResult %d), continuing\n", what,
              (void*)fence, (unsigned long long)(kWaitTimeoutNs / 1000000ull), (int)wait_result);
      fflush(stderr);
    }
    return;
  }
  if (queue != VK_NULL_HANDLE && g_QueueWaitIdle != nullptr) {
    g_QueueWaitIdle(queue);  // the submission carried no fence; drain its queue instead
    return;
  }
  ++g_unknown_sync;  // nothing observable to wait on; leave the violation visible
}

void cb_wait_for_reuse(VkCommandBuffer cb) {
  PendingUse use;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_cbs.find(cb);
    if (it == g_cbs.end() || !it->second.pending) return;
    use = it->second;
    it->second.pending = false;
    it->second.fence = VK_NULL_HANDLE;
    it->second.queue = VK_NULL_HANDLE;
  }
  wait_for_previous_use(use.device, use.queue, use.fence, "command buffer",
                        "vkResetCommandBuffer/vkBeginCommandBuffer/vkQueueSubmit", g_cb_waits);
}

// Diagnostic only: log semaphore usage so we can see whether an acquire semaphore
// was ever waited on in a submission (and with which fence). Throttled per handle.
void sem_diag(VkSemaphore semaphore, const char* event, VkQueue queue, VkFence fence) {
  if (semaphore == VK_NULL_HANDLE) return;
  unsigned count;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    count = ++g_sem_diag[semaphore];
  }
  if (count <= 8 || count % 2000 == 0) {
    fprintf(stderr, "[SSC] sem %p %s queue=%p fence=%p (#%u)\n", (void*)semaphore, event, (void*)queue, (void*)fence,
            count);
    fflush(stderr);
  }
}

void sem_wait_for_reuse(VkSemaphore semaphore) {
  if (semaphore == VK_NULL_HANDLE) return;
  PendingUse use;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_semaphores.find(semaphore);
    if (it == g_semaphores.end() || !it->second.pending) return;
    use = it->second;
    it->second.pending = false;
    it->second.fence = VK_NULL_HANDLE;
    it->second.queue = VK_NULL_HANDLE;
  }
  wait_for_previous_use(use.device, use.queue, use.fence, "binary semaphore",
                        "vkAcquireNextImageKHR/vkQueueSubmit", g_sem_waits);
}

void cb_mark_pending(VkCommandBuffer cb, VkQueue queue, VkFence fence) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_cbs.find(cb);
  if (it == g_cbs.end()) return;
  it->second.pending = true;
  it->second.queue = queue;
  it->second.fence = fence;
}

void sem_mark_pending(VkSemaphore semaphore, VkQueue queue, VkFence fence) {
  if (semaphore == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_semaphores.find(semaphore);
  if (it == g_semaphores.end()) return;  // not tracked (timeline semaphore or none)
  it->second.pending = true;
  it->second.queue = queue;
  it->second.fence = fence;
}

// Drop references to a fence that is going away, so no later wait uses a stale
// handle (those resources fall back to the queue-idle path).
void forget_fence(VkFence fence) {
  if (fence == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> guard(g_lock);
  for (auto& entry : g_cbs) {
    if (entry.second.fence == fence) entry.second.fence = VK_NULL_HANDLE;
  }
  for (auto& entry : g_semaphores) {
    if (entry.second.fence == fence) entry.second.fence = VK_NULL_HANDLE;
  }
}

// A fence is being reset, so its work is complete from the application's point
// of view; release every resource whose last use was tied to it. Without this,
// a reset (now unsignaled) fence would make a later reuse look pending and wait
// for the full timeout.
void release_fence_resources(VkFence fence) {
  if (fence == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> guard(g_lock);
  for (auto& entry : g_cbs) {
    if (entry.second.fence == fence) {
      entry.second.pending = false;
      entry.second.fence = VK_NULL_HANDLE;
      entry.second.queue = VK_NULL_HANDLE;
    }
  }
  for (auto& entry : g_semaphores) {
    if (entry.second.fence == fence) {
      entry.second.pending = false;
      entry.second.fence = VK_NULL_HANDLE;
      entry.second.queue = VK_NULL_HANDLE;
    }
  }
}

VKAPI_ATTR VkResult VKAPI_CALL SscAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
                                                         VkCommandBuffer* pCommandBuffers) {
  const VkResult result = g_AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
  if (result == VK_SUCCESS) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
      PendingUse use;
      use.device = device;
      g_cbs[pCommandBuffers[i]] = use;
    }
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL SscFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
                                                 const VkCommandBuffer* pCommandBuffers) {
  for (uint32_t i = 0; i < commandBufferCount; ++i) cb_wait_for_reuse(pCommandBuffers[i]);
  {
    std::lock_guard<std::mutex> guard(g_lock);
    for (uint32_t i = 0; i < commandBufferCount; ++i) g_cbs.erase(pCommandBuffers[i]);
  }
  g_FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL SscBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                     const VkCommandBufferBeginInfo* pBeginInfo) {
  cb_wait_for_reuse(commandBuffer);
  return g_BeginCommandBuffer(commandBuffer, pBeginInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL SscResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
  cb_wait_for_reuse(commandBuffer);
  return g_ResetCommandBuffer(commandBuffer, flags);
}

VKAPI_ATTR VkResult VKAPI_CALL SscCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator, VkSemaphore* pSemaphore) {
  const VkResult result = g_CreateSemaphore(device, pCreateInfo, pAllocator, pSemaphore);
  if (result != VK_SUCCESS) return result;
  bool timeline = false;
  for (const VkBaseInStructure* chain = (const VkBaseInStructure*)pCreateInfo->pNext; chain != nullptr;
       chain = chain->pNext) {
    if (chain->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
      timeline = ((const VkSemaphoreTypeCreateInfo*)chain)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
      break;
    }
  }
  if (!timeline) {  // timeline semaphores have their own (payload-based) sync rules
    std::lock_guard<std::mutex> guard(g_lock);
    PendingUse use;
    use.device = device;
    g_semaphores[*pSemaphore] = use;
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL SscDestroySemaphore(VkDevice device, VkSemaphore semaphore,
                                               const VkAllocationCallbacks* pAllocator) {
  {
    std::lock_guard<std::mutex> guard(g_lock);
    g_semaphores.erase(semaphore);
  }
  g_DestroySemaphore(device, semaphore, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL SscAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                      VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
  sem_wait_for_reuse(semaphore);
  const VkResult result = g_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
  if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) sem_mark_pending(semaphore, VK_NULL_HANDLE, fence);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL SscAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo,
                                                       uint32_t* pImageIndex) {
  if (g_AcquireNextImage2KHR == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
  sem_wait_for_reuse(pAcquireInfo->semaphore);
  const VkResult result = g_AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
  if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
    sem_mark_pending(pAcquireInfo->semaphore, VK_NULL_HANDLE, pAcquireInfo->fence);
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL SscCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
  VkLayerInstanceCreateInfo* chain = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
  while (chain != nullptr &&
         !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) {
    chain = (VkLayerInstanceCreateInfo*)chain->pNext;
  }
  if (chain == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  g_next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkCreateInstance next = (PFN_vkCreateInstance)g_next_gipa(nullptr, "vkCreateInstance");
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
  return next(pCreateInfo, pAllocator, pInstance);
}

VKAPI_ATTR VkResult VKAPI_CALL SscCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
  VkLayerDeviceCreateInfo* chain = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
  while (chain != nullptr &&
         !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) {
    chain = (VkLayerDeviceCreateInfo*)chain->pNext;
  }
  if (chain == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  PFN_vkCreateDevice next = (PFN_vkCreateDevice)next_gipa(nullptr, "vkCreateDevice");
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

  const VkResult result = next(physicalDevice, pCreateInfo, pAllocator, pDevice);
  if (result == VK_SUCCESS) {
    g_next_gdpa = next_gdpa;
    g_device = *pDevice;
    g_GetFenceStatus = (PFN_vkGetFenceStatus)next_gdpa(*pDevice, "vkGetFenceStatus");
    g_WaitForFences = (PFN_vkWaitForFences)next_gdpa(*pDevice, "vkWaitForFences");
    g_ResetFences = (PFN_vkResetFences)next_gdpa(*pDevice, "vkResetFences");
    g_QueueSubmit = (PFN_vkQueueSubmit)next_gdpa(*pDevice, "vkQueueSubmit");
    g_QueueSubmit2 = (PFN_vkQueueSubmit2)next_gdpa(*pDevice, "vkQueueSubmit2");
    g_DestroyFence = (PFN_vkDestroyFence)next_gdpa(*pDevice, "vkDestroyFence");
    g_DeviceWaitIdle = (PFN_vkDeviceWaitIdle)next_gdpa(*pDevice, "vkDeviceWaitIdle");
    g_QueueWaitIdle = (PFN_vkQueueWaitIdle)next_gdpa(*pDevice, "vkQueueWaitIdle");
    g_AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)next_gdpa(*pDevice, "vkAllocateCommandBuffers");
    g_FreeCommandBuffers = (PFN_vkFreeCommandBuffers)next_gdpa(*pDevice, "vkFreeCommandBuffers");
    g_BeginCommandBuffer = (PFN_vkBeginCommandBuffer)next_gdpa(*pDevice, "vkBeginCommandBuffer");
    g_ResetCommandBuffer = (PFN_vkResetCommandBuffer)next_gdpa(*pDevice, "vkResetCommandBuffer");
    g_CreateSemaphore = (PFN_vkCreateSemaphore)next_gdpa(*pDevice, "vkCreateSemaphore");
    g_DestroySemaphore = (PFN_vkDestroySemaphore)next_gdpa(*pDevice, "vkDestroySemaphore");
    g_AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)next_gdpa(*pDevice, "vkAcquireNextImageKHR");
    g_AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR)next_gdpa(*pDevice, "vkAcquireNextImage2KHR");
    fprintf(stderr, "[SSC] active on device %p (wait cap %llu ms)\n", (void*)*pDevice,
            (unsigned long long)(kWaitTimeoutNs / 1000000ull));
    fflush(stderr);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL SscDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
  PFN_vkDestroyDevice next = g_next_gdpa ? (PFN_vkDestroyDevice)g_next_gdpa(device, "vkDestroyDevice") : nullptr;
  if (next) next(device, pAllocator);
  {
    std::lock_guard<std::mutex> guard(g_lock);
    g_in_flight.clear();
    g_ever_submitted.clear();
    g_observed.clear();
    g_cbs.clear();
    g_semaphores.clear();
  }
  if (g_reset_fences > 0 || g_cb_waits > 0 || g_sem_waits > 0) {
    fprintf(stderr,
            "[SSC] summary: %lu submits w/ fence, %lu reset fences (%lu waited, %lu timed out, %lu untracked); "
            "%lu command-buffer waits, %lu semaphore waits, %lu reuses without a waitable signal\n",
            g_submitted.load(), g_reset_fences.load(), g_waited.load(), g_timeouts.load(), g_no_pending.load(),
            g_cb_waits.load(), g_sem_waits.load(), g_unknown_sync.load());
    fflush(stderr);
  }
  g_device = VK_NULL_HANDLE;
  g_next_gdpa = nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ssc_GetInstanceProcAddr(VkInstance instance, const char* name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ssc_GetDeviceProcAddr(VkDevice device, const char* name);

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ssc_GetInstanceProcAddr(VkInstance instance, const char* name) {
  if (name == nullptr) return nullptr;
  if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)ssc_GetInstanceProcAddr;
  if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)ssc_GetDeviceProcAddr;
  if (strcmp(name, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)SscCreateInstance;
  if (strcmp(name, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)SscCreateDevice;
  if (strcmp(name, "vkDestroyDevice") == 0) return (PFN_vkVoidFunction)SscDestroyDevice;
  return g_next_gipa ? g_next_gipa(instance, name) : nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ssc_GetDeviceProcAddr(VkDevice device, const char* name) {
  if (name == nullptr) return nullptr;
  if (strcmp(name, "vkResetFences") == 0) return (PFN_vkVoidFunction)SscResetFences;
  if (strcmp(name, "vkQueueSubmit") == 0) return (PFN_vkVoidFunction)SscQueueSubmit;
  if (strcmp(name, "vkQueueSubmit2") == 0) return (PFN_vkVoidFunction)SscQueueSubmit2;
  if (strcmp(name, "vkWaitForFences") == 0) return (PFN_vkVoidFunction)SscWaitForFences;
  if (strcmp(name, "vkGetFenceStatus") == 0) return (PFN_vkVoidFunction)SscGetFenceStatus;
  if (strcmp(name, "vkDestroyFence") == 0) return (PFN_vkVoidFunction)SscDestroyFence;
  if (strcmp(name, "vkAllocateCommandBuffers") == 0) return (PFN_vkVoidFunction)SscAllocateCommandBuffers;
  if (strcmp(name, "vkFreeCommandBuffers") == 0) return (PFN_vkVoidFunction)SscFreeCommandBuffers;
  if (strcmp(name, "vkBeginCommandBuffer") == 0) return (PFN_vkVoidFunction)SscBeginCommandBuffer;
  if (strcmp(name, "vkResetCommandBuffer") == 0) return (PFN_vkVoidFunction)SscResetCommandBuffer;
  if (strcmp(name, "vkCreateSemaphore") == 0) return (PFN_vkVoidFunction)SscCreateSemaphore;
  if (strcmp(name, "vkDestroySemaphore") == 0) return (PFN_vkVoidFunction)SscDestroySemaphore;
  if (strcmp(name, "vkAcquireNextImageKHR") == 0) return (PFN_vkVoidFunction)SscAcquireNextImageKHR;
  if (strcmp(name, "vkAcquireNextImage2KHR") == 0) return (PFN_vkVoidFunction)SscAcquireNextImage2KHR;
  return g_next_gdpa ? g_next_gdpa(device, name) : nullptr;
}

}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
  if (version == nullptr || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
  if (version->loaderLayerInterfaceVersion > 2) version->loaderLayerInterfaceVersion = 2;
  version->pfnGetInstanceProcAddr = ssc_GetInstanceProcAddr;
  version->pfnGetDeviceProcAddr = ssc_GetDeviceProcAddr;
  version->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}
