/* Presents cleared frames to an X11 window, checks the window contents with
 * XGetImage and reports the frame rate. */
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define W 800
#define H 600
#define FRAMES 300

#define CHECK(x)                                                         \
   do {                                                                  \
      VkResult r_ = (x);                                                 \
      if (r_ != VK_SUCCESS) {                                            \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); \
         exit(1);                                                        \
      }                                                                  \
   } while (0)

static double now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void)
{
   Display *dpy = XOpenDisplay(NULL);
   if (!dpy) {
      fprintf(stderr, "cannot open display\n");
      return 1;
   }
   Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, W, H, 0, 0, 0);
   XStoreName(dpy, win, "vkwsi");
   XMapWindow(dpy, win);
   XSync(dpy, False);

   const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ii = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = 2,
      .ppEnabledExtensionNames = exts,
   };
   VkInstance inst;
   CHECK(vkCreateInstance(&ii, NULL, &inst));
   VkXlibSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, .dpy = dpy, .window = win};
   VkSurfaceKHR surface;
   CHECK(vkCreateXlibSurfaceKHR(inst, &si, NULL, &surface));

   uint32_t count = 1;
   VkPhysicalDevice phys;
   vkEnumeratePhysicalDevices(inst, &count, &phys);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(phys, &props);
   printf("device: %s\n", props.deviceName);

   VkBool32 present = VK_FALSE;
   CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(phys, 0, surface, &present));
   if (!present) {
      fprintf(stderr, "queue family 0 cannot present\n");
      return 1;
   }
   float prio = 1;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   VkDeviceCreateInfo di = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qi,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = dev_exts,
   };
   VkDevice dev;
   CHECK(vkCreateDevice(phys, &di, NULL, &dev));
   VkQueue queue;
   vkGetDeviceQueue(dev, 0, 0, &queue);

   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
   uint32_t nformats = 16;
   VkSurfaceFormatKHR formats[16];
   vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nformats, formats);
   for (uint32_t i = 0; i < nformats; i++)
      if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
         formats[0] = formats[i];
   VkSwapchainCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount,
      .imageFormat = formats[0].format,
      .imageColorSpace = formats[0].colorSpace,
      .imageExtent = {W, H},
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
   };
   VkSwapchainKHR swapchain;
   CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &swapchain));
   uint32_t nimages = 8;
   VkImage images[8];
   CHECK(vkGetSwapchainImagesKHR(dev, swapchain, &nimages, images));
   printf("swapchain: %u images, format %d\n", nimages, formats[0].format);

   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
   VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acquired, rendered;
   CHECK(vkCreateSemaphore(dev, &sem_info, NULL, &acquired));
   CHECK(vkCreateSemaphore(dev, &sem_info, NULL, &rendered));
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(vkCreateFence(dev, &fi, NULL, &fence));

   VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   double start = now();
   VkClearColorValue color = {0};
   for (int i = 0; i < FRAMES; i++) {
      uint32_t index;
      CHECK(vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));
      CHECK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(vkBeginCommandBuffer(cmd, &bi));
      VkImageMemoryBarrier b = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = images[index],
         .subresourceRange = range,
      };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      float t = i / (float)(FRAMES - 1);
      color = (VkClearColorValue){.float32 = {t, 0.5f, 1 - t, 1}};
      vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
      b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.dstAccessMask = 0;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      CHECK(vkEndCommandBuffer(cmd));
      VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &acquired,
         .pWaitDstStageMask = &stage,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &rendered,
      };
      CHECK(vkQueueSubmit(queue, 1, &submit, fence));
      VkPresentInfoKHR pinfo = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &rendered,
         .swapchainCount = 1,
         .pSwapchains = &swapchain,
         .pImageIndices = &index,
      };
      CHECK(vkQueuePresentKHR(queue, &pinfo));
      CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
      CHECK(vkResetFences(dev, 1, &fence));
   }
   vkDeviceWaitIdle(dev);
   double secs = now() - start;
   printf("%d frames at %dx%d in %.2f s: %.1f fps\n", FRAMES, W, H, secs, FRAMES / secs);

   XSync(dpy, False);
   XImage *img = XGetImage(dpy, win, W / 2, H / 2, 1, 1, AllPlanes, ZPixmap);
   unsigned long px = XGetPixel(img, 0, 0);
   unsigned r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
   int ok = r >= 250 && g >= 126 && g <= 129 && b <= 2;
   printf("window center: %u,%u,%u (want 255,127/128,0)\n%s\n", r, g, b, ok ? "PASSED" : "FAILED");
   XDestroyImage(img);

   vkDestroyFence(dev, fence, NULL);
   vkDestroySemaphore(dev, acquired, NULL);
   vkDestroySemaphore(dev, rendered, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroySwapchainKHR(dev, swapchain, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroySurfaceKHR(inst, surface, NULL);
   vkDestroyInstance(inst, NULL);
   XCloseDisplay(dpy);
   return !ok;
}
