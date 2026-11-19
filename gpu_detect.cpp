#include <vulkan/vulkan.h>

#include <iostream>
#include <vector>
#include <iomanip>
#include <string>
#include <cstdint>


const char* getVendor(uint32_t id)
{
	switch (id)
	{
		case 0x1002: return "AMD";
		case 0x10DE: return "NVIDIA";
                case 0x8086: return "INTEL";
                case 0x13B5: return "ARM";
                case 0x5143: return "Qualcom";
		default: 	return "unknown???";
	}
}

const char* getDeviceType(VkPhysicalDeviceType type)
{
	switch (type)
	{
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			return "Discrete GPU";
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                        return "INTEGRATED GPU";
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                        return "Virtual GPU";
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                        return "CPU";
		default:
			return "other";
	}
}

int main()
{

	//Create vulkan instance
	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "GPU Detector";
	appInfo.applicationVersion = VK_MAKE_VERSION(1,0,0);
	appInfo.pEngineName = "None";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_2;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;

	VkInstance instance;

	VkResult result = vkCreateInstance(
		&createInfo, nullptr, &instance
	);

	if (result != VK_SUCCESS)
	{
		std::cerr << "Failed to initalized Vulkan: "
			<< result << std::endl;
	return 1;
	}

	//Enumerate physical GPUs
	uint32_t gpuCount = 0;

	result = vkEnumeratePhysicalDevices(
		instance, &gpuCount, nullptr
	);

	if(result != VK_SUCCESS || gpuCount == 0)
	{
		std::cout << " no vulkan devices found \n";
		vkDestroyInstance(instance, nullptr);
		return 0;
	}

	std::vector<VkPhysicalDevice> devices(gpuCount);

	vkEnumeratePhysicalDevices(
		instance, &gpuCount, devices.data()
	);

	std::cout << "\nDetected Vulkan device(s): "
		<<gpuCount << "\n\n";
	// inspect each gpu
	for(uint32_t i = 0 ; i < gpuCount ; i++)
	{
		VkPhysicalDeviceProperties properties{};

		vkGetPhysicalDeviceProperties(
			devices[i], &properties
		);

		VkPhysicalDeviceMemoryProperties memory{};

		vkGetPhysicalDeviceMemoryProperties(
			devices[i], &memory
		);

		std::cout << "==========================\n";
		std::cout << "GPU #" << i << "\n";
                std::cout << "==========================\n";

		std::cout << "name: "
			<< properties.deviceName << "\n";
		std::cout << "Vendor"
			<< getVendor(properties.vendorID) << "\n";
		std::cout << "Device  type"
			<< getDeviceType(properties.deviceType) << "\n";
		std::cout << "Vendor id 0x"
                        << properties.vendorID << "\n";
		std::cout << "Device ID: 0x"
			<< properties.deviceID << std::dec << "\n";
		std::cout << "Vulkan api: "
			<< VK_VERSION_MAJOR(properties.apiVersion)
			<< "."
			<< VK_VERSION_MINOR(properties.apiVersion)
			<< "."
			<< VK_VERSION_PATCH(properties.apiVersion)
			<< "\n";

		// MEmory Heaps
		std::cout << "\nMemory Heaps:\n";

		for (uint32_t j = 0; j  < memory.memoryHeapCount; j++)
		{
			const auto& heap = memory.memoryHeaps[j];

		double sizeGB = 
			static_cast<double>(heap.size) /
			(1024.0*1024.0*1024.0);

		std::cout << "Heap" << j << " : "
			<<std::fixed << std::setprecision(2)
			<<sizeGB << "GB";

		if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
			std::cout << " (device local)";
		std::cout << "\n";
		}
	std::cout << "\n";
	}
	vkDestroyInstance(instance, nullptr);

	return 0;
}

