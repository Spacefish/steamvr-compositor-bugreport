// Standalone check for VK_LAYER_steamvr_spec_compliance's fence tracking.
//
// Proves the two properties that matter:
//   1. resetting a fence that was never submitted is a fast no-op (we must not
//      block just because an unsignaled fence is VK_NOT_READY);
//   2. resetting a fence that still has in-flight work makes the layer wait for
//      it / forwards the reset only once the work is done.
// It also checks the fast paths where the app already observed the fence
// (vkWaitForFences / vkGetFenceStatus / GPU finished but nobody waited).
//
// Watch stderr for "[SSC] active on device" and the "[SSC] waiting on in-flight
// fence ... (#1)" line - those are the actual proof the layer ran; the timings
// below just show it stayed cheap.

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static bool g_ok = true;
static void check(bool ok, const char* what) {
  printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
  g_ok = g_ok && ok;
}

#define VK_CHECK(x)                                        \
  do {                                                     \
    VkResult r_ = (x);                                     \
    if (r_ != VK_SUCCESS) {                                \
      printf("FAIL %s -> VkResult %d\n", #x, (int)r_);     \
      std::exit(1);                                        \
    }                                                      \
  } while (0)

int main() {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

  uint32_t gpuCount = 0;
  VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr));
  if (gpuCount == 0) {
    printf("FAIL no Vulkan device\n");
    return 1;
  }
  std::vector<VkPhysicalDevice> gpus(gpuCount);
  gpuCount = static_cast<uint32_t>(gpus.size());
  VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()));
  VkPhysicalDevice gpu = gpus[0];
  for (VkPhysicalDevice candidate : gpus) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(candidate, &props);
    if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
      gpu = candidate;
      break;
    }
  }

  uint32_t qfCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qfCount);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, qfs.data());
  uint32_t qf = UINT32_MAX;
  for (uint32_t i = 0; i < qfCount; ++i) {
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      qf = i;
      break;
    }
  }
  if (qf == UINT32_MAX) {
    printf("FAIL no graphics queue family\n");
    return 1;
  }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = qf;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice dev = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &dev));
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, qf, 0, &queue);

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fence));

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = qf;
  VkCommandPool pool = VK_NULL_HANDLE;
  VK_CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
  VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbi.commandPool = pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cbi, &cb));

  auto submit_empty = [&]() {
    VK_CHECK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
  };

  auto timed_reset = [&](double* out_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    VK_CHECK(vkResetFences(dev, 1, &fence));
    *out_ms = ms_since(t0);
  };

  double d1 = 0, d2 = 0, d3 = 0, d4 = 0, d5 = 0;

  // 1. Never-submitted fence: unsignaled, but no work attached. Must not stall.
  timed_reset(&d1);
  check(d1 < 5.0, "reset of never-submitted fence returns immediately (no wait)");
  check(vkGetFenceStatus(dev, fence) == VK_NOT_READY, "fence is unsignaled after reset");

  // 2. In-flight fence: this is the violation the layer fixes.
  submit_empty();
  timed_reset(&d2);
  check(vkGetFenceStatus(dev, fence) == VK_NOT_READY, "fence is unsignaled after in-flight reset");
  check(d2 < 1900.0, "in-flight reset did not hit the 2 s wait cap");
  printf("       -> in-flight reset took %.2f ms (layer should have logged a wait)\n", d2);

  // 3. App already waited: tracking must be cleared, reset must be instant.
  submit_empty();
  VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
  timed_reset(&d3);
  check(d3 < 5.0, "reset after vkWaitForFences is instant (tracking cleared)");

  // 4. App polled to signaled: same.
  submit_empty();
  while (vkGetFenceStatus(dev, fence) == VK_NOT_READY) {
  }
  timed_reset(&d4);
  check(d4 < 5.0, "reset after polling signaled is instant (tracking cleared)");

  // 5. GPU finished but nobody observed it: GetFenceStatus fast path, no block.
  submit_empty();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  timed_reset(&d5);
  check(d5 < 50.0, "reset of a finished-but-unobserved fence uses the status fast path");

  printf("%s\n", g_ok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
  printf("timings: d1=%.2f d2=%.2f d3=%.2f d4=%.2f d5=%.2f (ms)\n", d1, d2, d3, d4, d5);

  vkDestroyFence(dev, fence, nullptr);
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyDevice(dev, nullptr);
  vkDestroyInstance(instance, nullptr);
  return g_ok ? 0 : 1;
}
