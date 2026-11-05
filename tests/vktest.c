/* Runs copies, an image clear and a compute shader, and checks the results on the CPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "compute.h"

#define N 4096
#define IMG 32

#define CHECK(x)                                                         \
   do {                                                                  \
      VkResult r_ = (x);                                                 \
      if (r_ != VK_SUCCESS) {                                            \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); \
         exit(1);                                                        \
      }                                                                  \
   } while (0)

static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static uint32_t family;

struct buffer {
   VkBuffer buf;
   VkDeviceMemory mem;
   uint32_t *map;
};

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties p;
   vkGetPhysicalDeviceMemoryProperties(phys, &p);
   for (uint32_t i = 0; i < p.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type %#x\n", want);
   exit(1);
}

static struct buffer buffer(VkDeviceSize size, VkMemoryPropertyFlags props)
{
   struct buffer b = {0};
   VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
   };
   CHECK(vkCreateBuffer(dev, &info, NULL, &b.buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, b.buf, &req);
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memory_type(req.memoryTypeBits, props),
   };
   CHECK(vkAllocateMemory(dev, &alloc, NULL, &b.mem));
   CHECK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
   if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      CHECK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, (void **)&b.map));
   return b;
}

static VkCommandBuffer begin(void)
{
   VkCommandBufferAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &info, &cmd));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   return cmd;
}

static void barrier(VkCommandBuffer cmd)
{
   VkMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}

static void submit(VkCommandBuffer cmd)
{
   barrier(cmd);
   VkMemoryBarrier host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
   CHECK(vkEndCommandBuffer(cmd));
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(vkCreateFence(dev, &fi, NULL, &fence));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
   vkDestroyFence(dev, fence, NULL);
   vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

static int expect(const char *what, const uint32_t *got, uint32_t (*want)(uint32_t), uint32_t n)
{
   for (uint32_t i = 0; i < n; i++) {
      if (got[i] != want(i)) {
         printf("%s: FAIL at %u: got %#x want %#x\n", what, i, got[i], want(i));
         return 1;
      }
   }
   printf("%s: ok\n", what);
   return 0;
}

static uint32_t fill_value(uint32_t i) { return 0xdeadbeef; }
static uint32_t pattern(uint32_t i) { return i * 2654435761u; }
static uint32_t computed(uint32_t i) { return pattern(i) * 3 + i; }
static uint32_t clear_value(uint32_t i) { return 0xff20c040; }

int main(void)
{
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
   VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   CHECK(vkCreateInstance(&ii, NULL, &inst));
   uint32_t count = 1;
   VkResult res = vkEnumeratePhysicalDevices(inst, &count, &phys);
   if (count == 0 || (res != VK_SUCCESS && res != VK_INCOMPLETE)) {
      fprintf(stderr, "no physical device\n");
      return 1;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(phys, &props);
   printf("device: %s (api %u.%u)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));

   uint32_t nfam = 8;
   VkQueueFamilyProperties fams[8];
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, fams);
   for (family = 0; family < nfam && !(fams[family].queueFlags & VK_QUEUE_COMPUTE_BIT); family++)
      ;
   float prio = 1;
   VkDeviceQueueCreateInfo qi = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   CHECK(vkCreateDevice(phys, &di, NULL, &dev));
   vkGetDeviceQueue(dev, family, 0, &queue);
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
   CHECK(vkCreateCommandPool(dev, &pi, NULL, &pool));

   const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   const VkDeviceSize size = N * 4;
   struct buffer local = buffer(size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   struct buffer src = buffer(size, host);
   struct buffer dst = buffer(size, host);
   int fails = 0;

   VkCommandBuffer cmd = begin();
   vkCmdFillBuffer(cmd, local.buf, 0, size, 0xdeadbeef);
   barrier(cmd);
   vkCmdCopyBuffer(cmd, local.buf, dst.buf, 1, &(VkBufferCopy){0, 0, size});
   submit(cmd);
   fails += expect("fill and copy to host", dst.map, fill_value, N);

   for (int round = 0; round < 2; round++) {
      for (uint32_t i = 0; i < N; i++)
         src.map[i] = pattern(i) + round;
      cmd = begin();
      vkCmdCopyBuffer(cmd, src.buf, local.buf, 1, &(VkBufferCopy){0, 0, size});
      barrier(cmd);
      vkCmdCopyBuffer(cmd, local.buf, dst.buf, 1, &(VkBufferCopy){0, 0, size});
      submit(cmd);
      char name[64];
      snprintf(name, sizeof(name), "host to device to host, round %d", round);
      for (uint32_t i = 0; i < N; i++)
         dst.map[i] -= round;
      fails += expect(name, dst.map, pattern, N);
   }

   /* Image clear, then copy to a host buffer. */
   VkImageCreateInfo imi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {IMG, IMG, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   };
   VkImage image;
   CHECK(vkCreateImage(dev, &imi, NULL, &image));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, image, &req);
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory image_mem;
   CHECK(vkAllocateMemory(dev, &alloc, NULL, &image_mem));
   CHECK(vkBindImageMemory(dev, image, image_mem, 0));
   VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   cmd = begin();
   VkImageMemoryBarrier ib = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = range,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
   VkClearColorValue color = {.float32 = {64 / 255.0f, 192 / 255.0f, 32 / 255.0f, 1}};
   vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
   ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
   VkBufferImageCopy copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {IMG, IMG, 1},
   };
   vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buf, 1, &copy);
   submit(cmd);
   fails += expect("image clear and copy", dst.map, clear_value, IMG * IMG);

   /* Compute shader: dst[i] = src[i] * 3 + i */
   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(compute_spv), .pCode = compute_spv};
   VkShaderModule module;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &module));
   VkDescriptorSetLayoutBinding bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
   };
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
   VkDescriptorSetLayout set_layout;
   CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &set_layout));
   VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
   VkPipelineLayoutCreateInfo pli = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push,
   };
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &layout));
   VkComputePipelineCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
      .layout = layout,
   };
   VkPipeline pipeline;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dpool;
   CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
   VkDescriptorSetAllocateInfo dai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet set;
   CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
   VkDescriptorBufferInfo infos[2] = {{src.buf, 0, size}, {dst.buf, 0, size}};
   VkWriteDescriptorSet writes[2];
   for (int i = 0; i < 2; i++)
      writes[i] = (VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = i,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &infos[i],
      };
   vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);
   for (uint32_t i = 0; i < N; i++)
      src.map[i] = pattern(i);
   memset(dst.map, 0, size);
   cmd = begin();
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
   uint32_t mul = 3;
   vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &mul);
   vkCmdDispatch(cmd, N / 64, 1, 1);
   submit(cmd);
   fails += expect("compute shader", dst.map, computed, N);

   vkDeviceWaitIdle(dev);
   vkDestroyPipeline(dev, pipeline, NULL);
   vkDestroyDescriptorPool(dev, dpool, NULL);
   vkDestroyPipelineLayout(dev, layout, NULL);
   vkDestroyDescriptorSetLayout(dev, set_layout, NULL);
   vkDestroyShaderModule(dev, module, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   printf(fails ? "FAILED\n" : "PASSED\n");
   return fails != 0;
}
