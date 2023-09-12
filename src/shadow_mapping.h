#pragma once
#include "common.h"
#include <map>

struct ShadowCascade {
	NtshEngn::Math::mat4 viewProj;
	float splitDepth;
};

struct DirectionalLightShadowMap {
	LayeredVulkanImage shadowMap;
	std::array<ShadowCascade, SHADOW_MAPPING_CASCADE_COUNT> cascades;
};

struct SpotLightShadowMap {
	LayeredVulkanImage shadowMap;
	NtshEngn::Math::mat4 viewProj;
};

class ShadowMapping {
public:
	void init(VkDevice device,
		VkQueue graphicsQueue,
		uint32_t graphicsQueueFamilyIndex,
		VmaAllocator allocator,
		VkCommandPool initializationCommandPool,
		VkCommandBuffer initializationCommandBuffer,
		VkFence initializationFence,
		uint32_t framesInFlight,
		const std::vector<VulkanBuffer>& objectBuffers,
		VulkanBuffer meshBuffer,
		const std::vector<VulkanBuffer>& jointTransformBuffers,
		const std::vector<VulkanBuffer>& materialBuffers,
		PFN_vkCmdBeginRenderingKHR vkCmdBeginRenderingKHR,
		PFN_vkCmdEndRenderingKHR vkCmdEndRenderingKHR,
		PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2KHR,
		NtshEngn::ECS* ecs);
	void destroy();

	void draw(VkCommandBuffer commandBuffer,
		uint32_t currentFrameInFlight,
		float cameraNearPlane,
		float cameraFarPlane,
		const NtshEngn::Math::mat4& cameraView,
		const NtshEngn::Math::mat4& cameraProjection,
		const std::unordered_map<NtshEngn::Entity, InternalObject>& objects,
		const std::vector<InternalMesh>& meshes,
		VulkanBuffer& vertexBuffer,
		VulkanBuffer& indexBuffer);

	void descriptorSetNeedsUpdate(uint32_t frameInFlight);
	void updateDescriptorSets(uint32_t frameInFlight,
		const std::vector<InternalTexture>& textures,
		const std::vector<VkImageView>& textureImageViews,
		const std::unordered_map<std::string, VkSampler>& textureSamplers);

	void createDirectionalLightShadowMap(NtshEngn::Entity entity);
	void destroyDirectionalLightShadowMap(NtshEngn::Entity entity);

	void createSpotLightShadowMap(NtshEngn::Entity entity);
	void destroySpotLightShadowMap(NtshEngn::Entity entity);

	VulkanBuffer& getCascadeSceneBuffer(uint32_t frameInFlight);
	std::vector<LayeredVulkanImage> getShadowMapImages();

private:
	void createImageAndBuffers();

	void createDescriptorSetLayout();

	void createGraphicsPipelines();
	void createDirectionalLightShadowGraphicsPipeline();
	void createSpotLightShadowGraphicsPipeline();

	void createDescriptorSets(const std::vector<VulkanBuffer>& objectBuffers,
		VulkanBuffer meshBuffer,
		const std::vector<VulkanBuffer>& jointTransformBuffers,
		const std::vector<VulkanBuffer>& materialBuffers);

private:
	std::vector<VulkanBuffer> m_shadowBuffers;
	std::vector<VulkanBuffer> m_cascadeSceneBuffers;

	LayeredVulkanImage m_dummyShadowMap;
	std::vector<NtshEngn::Entity> m_directionalLightEntities;
	std::vector<DirectionalLightShadowMap> m_directionalLightShadowMaps;
	std::vector<NtshEngn::Entity> m_spotLightEntities;
	std::vector<SpotLightShadowMap> m_spotLightShadowMaps;

	VkDescriptorSetLayout m_descriptorSetLayout;

	VkPipeline m_directionalLightShadowGraphicsPipeline;
	VkPipelineLayout m_directionalLightShadowGraphicsPipelineLayout;
	VkPipeline m_spotLightShadowGraphicsPipeline;
	VkPipelineLayout m_spotLightShadowGraphicsPipelineLayout;

	VkDescriptorPool m_descriptorPool;
	std::vector<VkDescriptorSet> m_descriptorSets;
	std::vector<bool> m_descriptorSetsNeedUpdate;

	VkViewport m_viewport;
	VkRect2D m_scissor;

	VkDevice m_device;
	VkQueue m_graphicsQueue;
	uint32_t m_graphicsQueueFamilyIndex;
	VmaAllocator m_allocator;
	VkCommandPool m_initializationCommandPool;
	VkCommandBuffer m_initializationCommandBuffer;
	VkFence m_initializationFence;
	uint32_t m_framesInFlight;

	PFN_vkCmdBeginRenderingKHR m_vkCmdBeginRenderingKHR;
	PFN_vkCmdEndRenderingKHR m_vkCmdEndRenderingKHR;
	PFN_vkCmdPipelineBarrier2KHR m_vkCmdPipelineBarrier2KHR;

	NtshEngn::ECS* m_ecs;
};