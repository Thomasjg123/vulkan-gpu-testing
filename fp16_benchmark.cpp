#include <vulkan/vulkan.h>

#include <iostream>
#include <vector>
#include <fstream>
#include <iomanip>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>
#include <string>

// Benchmark settings
constexpr uint32_t WORKGROUP_SIZE = 256;
constexpr uint32_t WORKGROUP_COUNT = 4096;
constexpr uint32_t ITERATIONS = 4096;

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(
            std::string(operation) + " failed: " +
            std::to_string(result));
}

std::vector<uint32_t> loadShader(const char* filename)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);

    if (!file)
        throw std::runtime_error("Could not open fp16.comp.spv");

    size_t size = static_cast<size_t>(file.tellg());

    if (size == 0 || size % 4 != 0)
        throw std::runtime_error("Invalid SPIR-V file size");

    std::vector<uint32_t> code(size / 4);

    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);

    return code;
}

uint32_t findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeBits,
    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory{};

    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
    {
        if ((typeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    throw std::runtime_error("No suitable Vulkan memory type found");
}

int main(int argc, char** argv)
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    try
    {
        // Select GPU from command line. Default is GPU #3.
        uint32_t gpuIndex = 3;

        if (argc > 1)
            gpuIndex = static_cast<uint32_t>(std::stoi(argv[1]));

        // Create Vulkan instance.
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "FP16 Benchmark";
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

        check(vkCreateInstance(&instanceInfo, nullptr, &instance),
              "vkCreateInstance");

        // Enumerate GPUs.
        uint32_t gpuCount = 0;
        check(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr),
              "vkEnumeratePhysicalDevices");

        if (gpuCount == 0 || gpuIndex >= gpuCount)
            throw std::runtime_error("GPU index does not exist");

        std::vector<VkPhysicalDevice> gpus(gpuCount);

        check(vkEnumeratePhysicalDevices(
                  instance, &gpuCount, gpus.data()),
              "vkEnumeratePhysicalDevices");

        VkPhysicalDevice physicalDevice = gpus[gpuIndex];

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);

        std::cout << "\nSelected GPU #" << gpuIndex << "\n";
        std::cout << "Name: " << properties.deviceName << "\n";

        // Check FP16 support.
        VkPhysicalDeviceShaderFloat16Int8Features supported{};
        supported.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;

        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &supported;

        vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

        if (!supported.shaderFloat16)
            throw std::runtime_error("This GPU does not support shader FP16");

        std::cout << "FP16 arithmetic: Supported\n";

        // Find a compute queue that supports timestamps.
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);

        vkGetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &queueFamilyCount, queueFamilies.data());

        uint32_t queueFamily = UINT32_MAX;

        for (uint32_t i = 0; i < queueFamilyCount; i++)
        {
            if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                queueFamilies[i].queueCount > 0 &&
                queueFamilies[i].timestampValidBits > 0)
            {
                queueFamily = i;
                break;
            }
        }

        if (queueFamily == UINT32_MAX)
            throw std::runtime_error("No timestamp-capable compute queue found");

        uint32_t timestampBits =
            queueFamilies[queueFamily].timestampValidBits;

        // Create logical device with FP16 enabled.
        float priority = 1.0f;

        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceShaderFloat16Int8Features enableFp16{};
        enableFp16.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
        enableFp16.shaderFloat16 = VK_TRUE;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &enableFp16;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;

        check(vkCreateDevice(
                  physicalDevice, &deviceInfo, nullptr, &device),
              "vkCreateDevice");

        VkQueue queue;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        // Allocate result buffer.
        const uint32_t invocationCount =
            WORKGROUP_SIZE * WORKGROUP_COUNT;

        const VkDeviceSize bufferSize =
            static_cast<VkDeviceSize>(invocationCount) * sizeof(float);

        if (bufferSize > properties.limits.maxStorageBufferRange)
            throw std::runtime_error("Result buffer exceeds GPU storage limit");

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer),
              "vkCreateBuffer");

        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(device, buffer, &memoryRequirements);

        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = memoryRequirements.size;
        allocation.memoryTypeIndex = findMemoryType(
            physicalDevice,
            memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        check(vkAllocateMemory(device, &allocation, nullptr, &bufferMemory),
              "vkAllocateMemory");

        check(vkBindBufferMemory(device, buffer, bufferMemory, 0),
              "vkBindBufferMemory");

        // Descriptor set layout: one storage buffer.
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo descriptorInfo{};
        descriptorInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorInfo.bindingCount = 1;
        descriptorInfo.pBindings = &binding;

        check(vkCreateDescriptorSetLayout(
                  device, &descriptorInfo, nullptr, &descriptorLayout),
              "vkCreateDescriptorSetLayout");

        // Pipeline layout: descriptor set + 4-byte push constant.
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(uint32_t);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;

        check(vkCreatePipelineLayout(
                  device, &pipelineLayoutInfo, nullptr, &pipelineLayout),
              "vkCreatePipelineLayout");

        // Load compiled compute shader.
        auto shaderCode = loadShader("fp16.comp.spv");

        VkShaderModuleCreateInfo shaderInfo{};
        shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderInfo.codeSize = shaderCode.size() * sizeof(uint32_t);
        shaderInfo.pCode = shaderCode.data();

        check(vkCreateShaderModule(
                  device, &shaderInfo, nullptr, &shaderModule),
              "vkCreateShaderModule");

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shaderModule;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout;

        check(vkCreateComputePipelines(
                  device, VK_NULL_HANDLE, 1, &pipelineInfo,
                  nullptr, &pipeline),
              "vkCreateComputePipelines");

        // Descriptor pool and descriptor set.
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        check(vkCreateDescriptorPool(
                  device, &poolInfo, nullptr, &descriptorPool),
              "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo setAllocation{};
        setAllocation.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocation.descriptorPool = descriptorPool;
        setAllocation.descriptorSetCount = 1;
        setAllocation.pSetLayouts = &descriptorLayout;

        VkDescriptorSet descriptorSet;
        check(vkAllocateDescriptorSets(
                  device, &setAllocation, &descriptorSet),
              "vkAllocateDescriptorSets");

        VkDescriptorBufferInfo descriptorBuffer{};
        descriptorBuffer.buffer = buffer;
        descriptorBuffer.offset = 0;
        descriptorBuffer.range = bufferSize;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &descriptorBuffer;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        // Timestamp query pool.
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;

        check(vkCreateQueryPool(device, &queryInfo, nullptr, &queryPool),
              "vkCreateQueryPool");

        // Command pool and command buffer.
        VkCommandPoolCreateInfo commandPoolInfo{};
        commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        commandPoolInfo.queueFamilyIndex = queueFamily;

        check(vkCreateCommandPool(
                  device, &commandPoolInfo, nullptr, &commandPool),
              "vkCreateCommandPool");

        VkCommandBufferAllocateInfo commandAllocation{};
        commandAllocation.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandAllocation.commandPool = commandPool;
        commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocation.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        check(vkAllocateCommandBuffers(
                  device, &commandAllocation, &commandBuffer),
              "vkAllocateCommandBuffers");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        check(vkBeginCommandBuffer(commandBuffer, &beginInfo),
              "vkBeginCommandBuffer");

        vkCmdResetQueryPool(commandBuffer, queryPool, 0, 2);

        vkCmdWriteTimestamp(
            commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            queryPool, 0);

        vkCmdBindPipeline(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

        vkCmdPushConstants(
            commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(ITERATIONS), &ITERATIONS);

        vkCmdDispatch(
            commandBuffer, WORKGROUP_COUNT, 1, 1);

        vkCmdWriteTimestamp(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            queryPool, 1);

        check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        check(vkCreateFence(device, &fenceInfo, nullptr, &fence),
              "vkCreateFence");

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;

        check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");

        check(vkWaitForFences(
                  device, 1, &fence, VK_TRUE, UINT64_MAX),
              "vkWaitForFences");

        // Read GPU timestamps.
        uint64_t timestamps[2]{};

        check(vkGetQueryPoolResults(
                  device, queryPool, 0, 2,
                  sizeof(timestamps), timestamps,
                  sizeof(uint64_t),
                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "vkGetQueryPoolResults");

        uint64_t ticks = timestamps[1] - timestamps[0];

        if (timestampBits < 64)
            ticks &= (1ULL << timestampBits) - 1;

        double elapsedSeconds =
            static_cast<double>(ticks) *
            properties.limits.timestampPeriod / 1.0e9;

        // Eight FP16 FMA operations per invocation per iteration.
        // Each FMA counts as two floating-point operations.
        double totalFlops =
            static_cast<double>(invocationCount) *
            ITERATIONS * 8.0 * 2.0;

        double tflops =
            totalFlops / elapsedSeconds / 1.0e12;

        std::cout << "\n========== FP16 BENCHMARK ==========\n";
        std::cout << "GPU: " << properties.deviceName << "\n";
        std::cout << "Invocations: " << invocationCount << "\n";
        std::cout << "Iterations: " << ITERATIONS << "\n";
        std::cout << "Elapsed GPU time: "
                  << std::fixed << std::setprecision(3)
                  << elapsedSeconds * 1000.0 << " ms\n";
        std::cout << "Total operations: "
                  << std::setprecision(0) << totalFlops << "\n";
        std::cout << "FP16 performance: "
                  << std::setprecision(3) << tflops << " TFLOPS\n";
        std::cout << "====================================\n";

        vkDeviceWaitIdle(device);

        // Clean up Vulkan resources.
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyQueryPool(device, queryPool, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, shaderModule, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, bufferMemory, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "\nERROR: " << error.what() << "\n";

        if (device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device);

            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            if (queryPool) vkDestroyQueryPool(device, queryPool, nullptr);
            if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (shaderModule) vkDestroyShaderModule(device, shaderModule, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
            if (buffer) vkDestroyBuffer(device, buffer, nullptr);
            if (bufferMemory) vkFreeMemory(device, bufferMemory, nullptr);

            vkDestroyDevice(device, nullptr);
        }

        if (instance != VK_NULL_HANDLE)
            vkDestroyInstance(instance, nullptr);

        return 1;
    }
}

