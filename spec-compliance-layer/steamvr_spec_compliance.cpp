// VK_LAYER_steamvr_spec_compliance
//
// A small, cheap spec-compliance layer for the SteamVR compositor. Right now it
// fixes one class of violation: resetting a fence whose GPU work has not
// completed yet (VUID-vkResetFences-pFences-01123), which is one of a set of
// "recycle while still in flight" bugs in the compositor.
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

std::mutex g_lock;
std::unordered_set<VkFence> g_in_flight;      // submitted, not yet seen signaled
std::unordered_set<VkFence> g_ever_submitted;  // diagnostic: every fence seen in a submit
std::unordered_map<VkFence, unsigned> g_observed;  // diagnostic: app observed it signaled

std::atomic<unsigned long> g_submitted{0};   // submits that carried a fence
std::atomic<unsigned long> g_waited{0};      // blocking waits we performed
std::atomic<unsigned long> g_timeouts{0};    // waits that hit the cap
std::atomic<unsigned long> g_no_pending{0};  // resets of fences with no pending work
std::atomic<unsigned long> g_reset_fences{0};  // fences passed to vkResetFences
std::atomic<unsigned long> g_drains{0};        // device waits issued to sync lower-layer state

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

bool any_ever_submitted(const VkFence* pFences, uint32_t fenceCount) {
  std::lock_guard<std::mutex> guard(g_lock);
  for (uint32_t i = 0; i < fenceCount; ++i) {
    if (g_ever_submitted.count(pFences[i]) != 0) return true;
  }
  return false;
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
  // A layer below us (Vulkan Validation Layers) retires a submitted fence's
  // "in use" state on a helper thread. If the app polls the fence to signaled
  // and then immediately resets it, validation can still see the fence as in
  // use and report VUID-vkResetFences-pFences-01123 even though the driver says
  // the fence is signaled. Drain the lower layers' queue bookkeeping before we
  // forward the reset, so they observe a legal reset.
  // vkDeviceWaitIdle is deliberate: on the validation side its handling
  // (Queue::NotifyAndWait) drains the helper thread and has no fence-promise
  // stall hazard, unlike Fence::NotifyAndWait.
  if (g_DeviceWaitIdle != nullptr && any_ever_submitted(pFences, fenceCount)) {
    const unsigned long n = ++g_drains;
    if (n <= 5 || n % 100 == 0) {
      fprintf(stderr, "[SSC] vkDeviceWaitIdle before vkResetFences so lower layers retire the fence (#%lu)\n", n);
      fflush(stderr);
    }
    g_DeviceWaitIdle(device);
  }

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
      ++g_no_pending;
      continue;
    }
    // Submitted earlier, but it may already be signaled - don't block for that.
    if (status != VK_NOT_READY) {
      note_signaled(fence);
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
    }
    note_signaled(fence);  // never block on this fence again
  }
  return g_ResetFences(device, fenceCount, pFences);
}

VKAPI_ATTR VkResult VKAPI_CALL SscQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                              VkFence fence) {
  note_in_flight(fence);
  const VkResult result = g_QueueSubmit(queue, submitCount, pSubmits, fence);
  if (result != VK_SUCCESS) note_signaled(fence);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL SscQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                               VkFence fence) {
  if (g_QueueSubmit2 == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
  note_in_flight(fence);
  const VkResult result = g_QueueSubmit2(queue, submitCount, pSubmits, fence);
  if (result != VK_SUCCESS) note_signaled(fence);
  return result;
}

VKAPI_ATTR void VKAPI_CALL SscDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* pAllocator) {
  note_signaled(fence);
  g_DestroyFence(device, fence, pAllocator);
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
  }
  if (g_reset_fences > 0) {
    fprintf(stderr,
            "[SSC] summary: %lu submits w/ fence, %lu reset fences (%lu waited, %lu timed out, %lu untracked), %lu "
            "lower-layer drains\n",
            g_submitted.load(), g_reset_fences.load(), g_waited.load(), g_timeouts.load(), g_no_pending.load(),
            g_drains.load());
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
