VkPhysicalDeviceShaderFloat16Int8Features fp16Features{};
fp16Features.sType =
	VK_STRUCTURE_TYPE_PHSYICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
VkPhysicalDeviceFeatures2 features{};
features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
features.pNext = &fp16Features;

vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

if (fp16Features.shaderFloat16)
{
	std::cout << "FP16 arithmetic: Supported\n";
}
else
{
        std::cout << "FP16 arithmetic: NOT  Supported\n";
}

