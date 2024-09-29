#pragma once
#include "common.h"
#include <vector>
#include <array>

class Particles {
public:
	void init(VkDevice device,
		VkQueue graphicsComputeQueue,
		uint32_t graphicsComputeQueueFamilyIndex,
		VmaAllocator allocator,
		VkFormat drawImageFormat,
		VkCommandPool initializationCommandPool,
		VkCommandBuffer initializationCommandBuffer,
		VkFence initializationFence,
		VkViewport viewport,
		VkRect2D scissor,
		uint32_t framesInFlight,
		PFN_vkCmdBeginRenderingKHR vkCmdBeginRenderingKHR,
		PFN_vkCmdEndRenderingKHR vkCmdEndRenderingKHR,
		PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2KHR);
	void destroy();

	void draw(VkCommandBuffer commandBuffer, VkImage drawImage, VkImageView drawImageView, VkImageView depthImageView);

	void emitParticles(const NtshEngn::ParticleEmitter& particleEmitter, uint32_t currentFrameInFlight);

private:
	void createBuffers();

	void createComputeResources();

	void createGraphicsResources(VkFormat drawImageFormat);

private:
	std::array<VulkanBuffer, 2> m_particleBuffers;
	std::vector<HostVisibleVulkanBuffer> m_stagingBuffers;
	VulkanBuffer m_drawIndirectBuffer;
	std::vector<bool> m_buffersNeedUpdate;
	
	VkDescriptorSetLayout m_computeDescriptorSetLayout;
	VkDescriptorPool m_computeDescriptorPool;
	std::array<VkDescriptorSet, 2> m_computeDescriptorSets;
	VkPipeline m_computePipeline;
	VkPipelineLayout m_computePipelineLayout;

	VkDescriptorSetLayout m_graphicsDescriptorSetLayout;
	VkDescriptorPool m_graphicsDescriptorPool;
	std::vector<VkDescriptorSet> m_graphicsDescriptorSets;
	VkPipeline m_graphicsPipeline;
	VkPipelineLayout m_graphicsPipelineLayout;

	uint32_t m_inParticleBufferCurrentIndex = 0;
	uint32_t m_maxParticlesNumber = 100000;
	size_t m_currentParticleHostSize = 0;

	VkDevice m_device;
	VkQueue m_graphicsComputeQueue;
	uint32_t m_graphicsComputeQueueFamilyIndex;
	VmaAllocator m_allocator;
	VkCommandPool m_initializationCommandPool;
	VkCommandBuffer m_initializationCommandBuffer;
	VkFence m_initializationFence;
	VkViewport m_viewport;
	VkRect2D m_scissor;
	uint32_t m_framesInFlight;

	PFN_vkCmdBeginRenderingKHR m_vkCmdBeginRenderingKHR;
	PFN_vkCmdEndRenderingKHR m_vkCmdEndRenderingKHR;
	PFN_vkCmdPipelineBarrier2KHR m_vkCmdPipelineBarrier2KHR;
};