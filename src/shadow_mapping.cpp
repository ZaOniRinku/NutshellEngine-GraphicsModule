#include "shadow_mapping.h"
#include <array>
#include <cmath>

void ShadowMapping::init(VkDevice device,
	VkQueue graphicsQueue,
	uint32_t graphicsQueueFamilyIndex,
	VmaAllocator allocator,
	VkFence initializationFence,
	uint32_t framesInFlight,
	const std::vector<VulkanBuffer>& objectBuffers,
	const std::vector<VulkanBuffer>& materialBuffers,
	PFN_vkCmdBeginRenderingKHR vkCmdBeginRenderingKHR,
	PFN_vkCmdEndRenderingKHR vkCmdEndRenderingKHR,
	PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2KHR,
	NtshEngn::ECS* ecs) {
	m_device = device;
	m_graphicsQueue = graphicsQueue;
	m_graphicsQueueFamilyIndex = graphicsQueueFamilyIndex;
	m_allocator = allocator;
	m_initializationFence = initializationFence;
	m_framesInFlight = framesInFlight;
	m_vkCmdBeginRenderingKHR = vkCmdBeginRenderingKHR;
	m_vkCmdEndRenderingKHR = vkCmdEndRenderingKHR;
	m_vkCmdPipelineBarrier2KHR = vkCmdPipelineBarrier2KHR;
	m_ecs = ecs;

	m_viewport.x = 0.0f;
	m_viewport.y = 0.0f;
	m_viewport.width = static_cast<float>(SHADOW_MAPPING_RESOLUTION);
	m_viewport.height = static_cast<float>(SHADOW_MAPPING_RESOLUTION);
	m_viewport.minDepth = 0.0f;
	m_viewport.maxDepth = 1.0f;

	m_scissor.offset.x = 0;
	m_scissor.offset.y = 0;
	m_scissor.extent.width = SHADOW_MAPPING_RESOLUTION;
	m_scissor.extent.height = SHADOW_MAPPING_RESOLUTION;

	createImageAndBuffers();
	createDescriptorSetLayout();
	createGraphicsPipelines();
	createDescriptorSets(objectBuffers, materialBuffers);
}

void ShadowMapping::destroy() {
	vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);

	vkDestroyPipeline(m_device, m_spotLightShadowGraphicsPipeline, nullptr);
	vkDestroyPipelineLayout(m_device, m_spotLightShadowGraphicsPipelineLayout, nullptr);

	vkDestroyPipeline(m_device, m_directionalLightShadowGraphicsPipeline, nullptr);
	vkDestroyPipelineLayout(m_device, m_directionalLightShadowGraphicsPipelineLayout, nullptr);

	vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);

	for (uint32_t i = 0; i < m_framesInFlight; i++) {
		m_cascadeSceneBuffers[i].destroy(m_allocator);
		m_shadowBuffers[i].destroy(m_allocator);
	}

	for (auto& spotLightShadowMap : m_spotLightShadowMaps) {
		spotLightShadowMap.shadowMap.destroy(m_device, m_allocator);
	}
	for (auto& directionalLightShadowMap : m_directionalLightShadowMaps) {
		directionalLightShadowMap.shadowMap.destroy(m_device, m_allocator);
	}
	m_dummyShadowMap.destroy(m_device, m_allocator);
}

void ShadowMapping::draw(VkCommandBuffer commandBuffer,
	uint32_t currentFrameInFlight,
	float cameraNearPlane,
	float cameraFarPlane,
	const NtshEngn::Math::mat4& cameraView,
	const NtshEngn::Math::mat4& cameraProjection,
	const std::unordered_map<NtshEngn::Entity, InternalObject>& objects,
	const std::vector<InternalMesh>& meshes,
	VulkanBuffer& vertexBuffer,
	VulkanBuffer& indexBuffer) {
	std::array<float, SHADOW_MAPPING_CASCADE_COUNT> cascadeSplits;

	const float clipRange = cameraFarPlane - cameraNearPlane;
	const float clipRatio = cameraFarPlane / cameraNearPlane;

	for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_MAPPING_CASCADE_COUNT; cascadeIndex++) {
		const float p = (cascadeIndex + 1) / static_cast<float>(SHADOW_MAPPING_CASCADE_COUNT);
		const float log = cameraNearPlane * std::pow(clipRatio, p);
		const float uniform = cameraNearPlane + (clipRange * p);
		const float d = 0.95f * (log - uniform) + uniform;
		cascadeSplits[cascadeIndex] = (d - cameraNearPlane) / clipRange;
	}

	std::array<NtshEngn::Math::vec3, 8> frustumCorners = { NtshEngn::Math::vec3(-1.0f, 1.0f, 0.0f),
		NtshEngn::Math::vec3(1.0f, 1.0f, 0.0f),
		NtshEngn::Math::vec3(1.0f, -1.0f, 0.0f),
		NtshEngn::Math::vec3(-1.0f, -1.0f, 0.0f),
		NtshEngn::Math::vec3(-1.0f, 1.0f, 1.0f),
		NtshEngn::Math::vec3(1.0f, 1.0f, 1.0f),
		NtshEngn::Math::vec3(1.0f, -1.0f, 1.0f),
		NtshEngn::Math::vec3(-1.0f, -1.0f, 1.0f)
	};

	const NtshEngn::Math::mat4 inverseViewProj = NtshEngn::Math::inverse(cameraProjection * cameraView);
	for (uint8_t i = 0; i < 8; i++) {
		const NtshEngn::Math::vec4 inverseFrustumCorner = inverseViewProj * NtshEngn::Math::vec4(frustumCorners[i], 1.0f);
		frustumCorners[i] = inverseFrustumCorner / inverseFrustumCorner.w;
	}

	float lastSplitDistance = 0.0f;
	for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_MAPPING_CASCADE_COUNT; cascadeIndex++) {
		float splitDistance = cascadeSplits[cascadeIndex];

		std::array<NtshEngn::Math::vec3, 8> cascadeFrustumCorners = frustumCorners;

		for (uint8_t i = 0; i < 4; i++) {
			const NtshEngn::Math::vec3 distance = cascadeFrustumCorners[i + 4] - cascadeFrustumCorners[i];
			cascadeFrustumCorners[i + 4] = cascadeFrustumCorners[i] + (distance * splitDistance);
			cascadeFrustumCorners[i] = cascadeFrustumCorners[i] + (distance * lastSplitDistance);
		}

		NtshEngn::Math::vec3 cascadeFrustumCenter = { 0.0f, 0.0f, 0.0f };
		for (uint32_t i = 0; i < 8; i++) {
			cascadeFrustumCenter += cascadeFrustumCorners[i];
		}
		cascadeFrustumCenter /= 8.0f;

		float radius = 0.0f;
		for (uint8_t i = 0; i < 8; i++) {
			const float distanceToCenter = (cascadeFrustumCorners[i] - cascadeFrustumCenter).length();
			radius = std::max(radius, distanceToCenter);
		}
		radius = std::ceil(radius * 16.0f) / 16.0f;

		for (size_t directionalLightIndex = 0; directionalLightIndex < m_directionalLightEntities.size(); directionalLightIndex++) {
			const NtshEngn::Transform& lightTransform = m_ecs->getComponent<NtshEngn::Transform>(m_directionalLightEntities[directionalLightIndex]);

			const NtshEngn::Math::vec3 normalizedLightDirection = NtshEngn::Math::normalize(lightTransform.rotation);
			const NtshEngn::Math::vec3 upVector = (std::abs(NtshEngn::Math::dot(normalizedLightDirection, NtshEngn::Math::vec3(0.0f, 1.0f, 0.0f))) == 1.0f) ? NtshEngn::Math::vec3(1.0f, 0.0f, 0.0f) : NtshEngn::Math::vec3(0.0f, 1.0f, 0.0f);
			const NtshEngn::Math::mat4 lightView = NtshEngn::Math::lookAtRH(cascadeFrustumCenter - (normalizedLightDirection * radius), cascadeFrustumCenter, upVector);
			const NtshEngn::Math::mat4 lightProj = NtshEngn::Math::orthoRH(-radius, radius, -radius, radius, 0.0f, radius * 2.0f);

			m_directionalLightShadowMaps[directionalLightIndex].cascades[cascadeIndex].viewProj = lightProj * lightView;
			m_directionalLightShadowMaps[directionalLightIndex].cascades[cascadeIndex].splitDepth = -(cameraNearPlane + (splitDistance * clipRange));
		}

		lastSplitDistance = splitDistance;
	}

	void* data;
	NTSHENGN_VK_CHECK(vmaMapMemory(m_allocator, m_shadowBuffers[currentFrameInFlight].allocation, &data));
	for (size_t directionalLightIndex = 0; directionalLightIndex < m_directionalLightEntities.size(); directionalLightIndex++) {
		for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_MAPPING_CASCADE_COUNT; cascadeIndex++) {
			memcpy(reinterpret_cast<char*>(data) + (((directionalLightIndex * SHADOW_MAPPING_CASCADE_COUNT) + cascadeIndex) * sizeof(NtshEngn::Math::mat4)), &m_directionalLightShadowMaps[directionalLightIndex].cascades[cascadeIndex].viewProj, sizeof(NtshEngn::Math::mat4));
		}
	}
	for (size_t spotLightIndex = 0; spotLightIndex < m_spotLightEntities.size(); spotLightIndex++) {
		const NtshEngn::Transform& lightTransform = m_ecs->getComponent<NtshEngn::Transform>(m_spotLightEntities[spotLightIndex]);
		const NtshEngn::Light& lightLight = m_ecs->getComponent<NtshEngn::Light>(m_spotLightEntities[spotLightIndex]);

		const NtshEngn::Math::vec3 normalizedLightDirection = NtshEngn::Math::normalize(lightTransform.rotation);
		const NtshEngn::Math::vec3 upVector = (std::abs(NtshEngn::Math::dot(normalizedLightDirection, NtshEngn::Math::vec3(0.0f, 1.0f, 0.0f))) == 1.0f) ? NtshEngn::Math::vec3(1.0f, 0.0f, 0.0f) : NtshEngn::Math::vec3(0.0f, 1.0f, 0.0f);
		const NtshEngn::Math::mat4 lightView = NtshEngn::Math::lookAtRH(lightTransform.position, lightTransform.position + normalizedLightDirection, upVector);
		NtshEngn::Math::mat4 lightProj = NtshEngn::Math::perspectiveRH(lightLight.cutoff.y * 2.0f, 1.0f, 0.05f, 50.0f);
		lightProj[1][1] *= -1.0f;
		m_spotLightShadowMaps[spotLightIndex].viewProj = lightProj * lightView;
		memcpy(reinterpret_cast<char*>(data) + (((m_directionalLightShadowMaps.size() * SHADOW_MAPPING_CASCADE_COUNT) * sizeof(NtshEngn::Math::mat4)) + (spotLightIndex * sizeof(NtshEngn::Math::mat4))), & m_spotLightShadowMaps[spotLightIndex].viewProj, sizeof(NtshEngn::Math::mat4));
	}
	vmaUnmapMemory(m_allocator, m_shadowBuffers[currentFrameInFlight].allocation);

	NTSHENGN_VK_CHECK(vmaMapMemory(m_allocator, m_cascadeSceneBuffers[currentFrameInFlight].allocation, &data));
	for (size_t directionalLightIndex = 0; directionalLightIndex < m_directionalLightEntities.size(); directionalLightIndex++) {
		for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_MAPPING_CASCADE_COUNT; cascadeIndex++) {
			memcpy(reinterpret_cast<char*>(data) + (((directionalLightIndex * SHADOW_MAPPING_CASCADE_COUNT) + cascadeIndex) * (sizeof(NtshEngn::Math::mat4) + sizeof(NtshEngn::Math::vec4))), &m_directionalLightShadowMaps[directionalLightIndex].cascades[cascadeIndex].viewProj, sizeof(NtshEngn::Math::mat4));
			memcpy(reinterpret_cast<char*>(data) + (((directionalLightIndex * SHADOW_MAPPING_CASCADE_COUNT) + cascadeIndex) * (sizeof(NtshEngn::Math::mat4) + sizeof(NtshEngn::Math::vec4))) + sizeof(NtshEngn::Math::mat4), &m_directionalLightShadowMaps[directionalLightIndex].cascades[cascadeIndex].splitDepth, sizeof(NtshEngn::Math::vec4));
		}
	}
	for (size_t spotLightIndex = 0; spotLightIndex < m_spotLightEntities.size(); spotLightIndex++) {
		memcpy(reinterpret_cast<char*>(data) + (((m_directionalLightEntities.size() * SHADOW_MAPPING_CASCADE_COUNT) * (sizeof(NtshEngn::Math::mat4) + sizeof(NtshEngn::Math::vec4))) + (spotLightIndex * (sizeof(NtshEngn::Math::mat4) + sizeof(NtshEngn::Math::vec4)))), &m_spotLightShadowMaps[spotLightIndex].viewProj, sizeof(NtshEngn::Math::mat4));
	}
	vmaUnmapMemory(m_allocator, m_cascadeSceneBuffers[currentFrameInFlight].allocation);

	// Shadow mapping layout transition
	std::vector<VkImageMemoryBarrier2> undefinedToDepthStencilAttachmentImageMemoryBarriers;
	for (size_t directionalLightIndex = 0; directionalLightIndex < m_directionalLightEntities.size(); directionalLightIndex++) {
		VkImageMemoryBarrier2 undefinedToDepthStencilAttachmentImageMemoryBarrier = {};
		undefinedToDepthStencilAttachmentImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.pNext = nullptr;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.srcQueueFamilyIndex = m_graphicsQueueFamilyIndex;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.dstQueueFamilyIndex = m_graphicsQueueFamilyIndex;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.image = m_directionalLightShadowMaps[directionalLightIndex].shadowMap.handle;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.baseMipLevel = 0;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.levelCount = 1;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.layerCount = SHADOW_MAPPING_CASCADE_COUNT;
		undefinedToDepthStencilAttachmentImageMemoryBarriers.push_back(undefinedToDepthStencilAttachmentImageMemoryBarrier);
	}
	for (size_t spotLightIndex = 0; spotLightIndex < m_spotLightEntities.size(); spotLightIndex++) {
		VkImageMemoryBarrier2 undefinedToDepthStencilAttachmentImageMemoryBarrier = {};
		undefinedToDepthStencilAttachmentImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.pNext = nullptr;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.srcQueueFamilyIndex = m_graphicsQueueFamilyIndex;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.dstQueueFamilyIndex = m_graphicsQueueFamilyIndex;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.image = m_spotLightShadowMaps[spotLightIndex].shadowMap.handle;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.baseMipLevel = 0;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.levelCount = 1;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
		undefinedToDepthStencilAttachmentImageMemoryBarrier.subresourceRange.layerCount = 1;
		undefinedToDepthStencilAttachmentImageMemoryBarriers.push_back(undefinedToDepthStencilAttachmentImageMemoryBarrier);
	}

	VkDependencyInfo undefinedToDepthStencilAttachmentDependencyInfo = {};
	undefinedToDepthStencilAttachmentDependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	undefinedToDepthStencilAttachmentDependencyInfo.pNext = nullptr;
	undefinedToDepthStencilAttachmentDependencyInfo.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	undefinedToDepthStencilAttachmentDependencyInfo.memoryBarrierCount = 0;
	undefinedToDepthStencilAttachmentDependencyInfo.pMemoryBarriers = nullptr;
	undefinedToDepthStencilAttachmentDependencyInfo.bufferMemoryBarrierCount = 0;
	undefinedToDepthStencilAttachmentDependencyInfo.pBufferMemoryBarriers = nullptr;
	undefinedToDepthStencilAttachmentDependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(undefinedToDepthStencilAttachmentImageMemoryBarriers.size());
	undefinedToDepthStencilAttachmentDependencyInfo.pImageMemoryBarriers = undefinedToDepthStencilAttachmentImageMemoryBarriers.data();
	m_vkCmdPipelineBarrier2KHR(commandBuffer, &undefinedToDepthStencilAttachmentDependencyInfo);

	// Begin shadow mapping rendering

	// Bind vertex and index buffers
	VkDeviceSize vertexBufferOffset = 0;
	vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer.handle, &vertexBufferOffset);
	vkCmdBindIndexBuffer(commandBuffer, indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

	// Directional light shadows
	// Bind descriptor set 0
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_directionalLightShadowGraphicsPipelineLayout, 0, 1, &m_descriptorSets[currentFrameInFlight], 0, nullptr);

	// Bind graphics pipeline
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_directionalLightShadowGraphicsPipeline);

	for (size_t directionalLightIndex = 0; directionalLightIndex < m_directionalLightEntities.size(); directionalLightIndex++) {
		for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_MAPPING_CASCADE_COUNT; cascadeIndex++) {
			VkRenderingAttachmentInfo shadowMapCascadeAttachmentInfo = {};
			shadowMapCascadeAttachmentInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			shadowMapCascadeAttachmentInfo.pNext = nullptr;
			shadowMapCascadeAttachmentInfo.imageView = m_directionalLightShadowMaps[directionalLightIndex].shadowMap.views[cascadeIndex];
			shadowMapCascadeAttachmentInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			shadowMapCascadeAttachmentInfo.resolveMode = VK_RESOLVE_MODE_NONE;
			shadowMapCascadeAttachmentInfo.resolveImageView = VK_NULL_HANDLE;
			shadowMapCascadeAttachmentInfo.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			shadowMapCascadeAttachmentInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			shadowMapCascadeAttachmentInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			shadowMapCascadeAttachmentInfo.clearValue.depthStencil = { 1.0f, 0 };

			VkRenderingInfo shadowMapCascadeRenderingInfo = {};
			shadowMapCascadeRenderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
			shadowMapCascadeRenderingInfo.pNext = nullptr;
			shadowMapCascadeRenderingInfo.flags = 0;
			shadowMapCascadeRenderingInfo.renderArea = m_scissor;
			shadowMapCascadeRenderingInfo.layerCount = 1;
			shadowMapCascadeRenderingInfo.viewMask = 0;
			shadowMapCascadeRenderingInfo.colorAttachmentCount = 0;
			shadowMapCascadeRenderingInfo.pColorAttachments = nullptr;
			shadowMapCascadeRenderingInfo.pDepthAttachment = &shadowMapCascadeAttachmentInfo;
			shadowMapCascadeRenderingInfo.pStencilAttachment = nullptr;
			m_vkCmdBeginRenderingKHR(commandBuffer, &shadowMapCascadeRenderingInfo);

			for (const auto& object : objects) {
				// Three indices as push constants: directional light index, cascade index and object index
				std::array<uint32_t, 3> cascadeIndices = { static_cast<uint32_t>(directionalLightIndex), cascadeIndex, object.second.index };
				vkCmdPushConstants(commandBuffer, m_directionalLightShadowGraphicsPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t) * 3, cascadeIndices.data());

				// Draw
				vkCmdDrawIndexed(commandBuffer, meshes[object.second.meshID].indexCount, 1, meshes[object.second.meshID].firstIndex, meshes[object.second.meshID].vertexOffset, 0);
			}

			// End shadow mapping rendering
			m_vkCmdEndRenderingKHR(commandBuffer);
		}
	}

	// Spot light shadows
	// Bind descriptor set 0
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_spotLightShadowGraphicsPipelineLayout, 0, 1, &m_descriptorSets[currentFrameInFlight], 0, nullptr);

	// Bind graphics pipeline
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_spotLightShadowGraphicsPipeline);

	for (size_t spotLightIndex = 0; spotLightIndex < m_spotLightEntities.size(); spotLightIndex++) {
		VkRenderingAttachmentInfo shadowMapCascadeAttachmentInfo = {};
		shadowMapCascadeAttachmentInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		shadowMapCascadeAttachmentInfo.pNext = nullptr;
		shadowMapCascadeAttachmentInfo.imageView = m_spotLightShadowMaps[spotLightIndex].shadowMap.views[0];
		shadowMapCascadeAttachmentInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		shadowMapCascadeAttachmentInfo.resolveMode = VK_RESOLVE_MODE_NONE;
		shadowMapCascadeAttachmentInfo.resolveImageView = VK_NULL_HANDLE;
		shadowMapCascadeAttachmentInfo.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		shadowMapCascadeAttachmentInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		shadowMapCascadeAttachmentInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		shadowMapCascadeAttachmentInfo.clearValue.depthStencil = { 1.0f, 0 };

		VkRenderingInfo shadowMapCascadeRenderingInfo = {};
		shadowMapCascadeRenderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		shadowMapCascadeRenderingInfo.pNext = nullptr;
		shadowMapCascadeRenderingInfo.flags = 0;
		shadowMapCascadeRenderingInfo.renderArea = m_scissor;
		shadowMapCascadeRenderingInfo.layerCount = 1;
		shadowMapCascadeRenderingInfo.viewMask = 0;
		shadowMapCascadeRenderingInfo.colorAttachmentCount = 0;
		shadowMapCascadeRenderingInfo.pColorAttachments = nullptr;
		shadowMapCascadeRenderingInfo.pDepthAttachment = &shadowMapCascadeAttachmentInfo;
		shadowMapCascadeRenderingInfo.pStencilAttachment = nullptr;
		m_vkCmdBeginRenderingKHR(commandBuffer, &shadowMapCascadeRenderingInfo);

		for (const auto& object : objects) {
			// Three indices as push constants: directional light index, cascade index and object index
			std::array<uint32_t, 2> cascadeIndices = { (static_cast<uint32_t>(m_directionalLightShadowMaps.size()) * SHADOW_MAPPING_CASCADE_COUNT) + static_cast<uint32_t>(spotLightIndex), object.second.index };
			vkCmdPushConstants(commandBuffer, m_spotLightShadowGraphicsPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t) * 2, cascadeIndices.data());

			// Draw
			vkCmdDrawIndexed(commandBuffer, meshes[object.second.meshID].indexCount, 1, meshes[object.second.meshID].firstIndex, meshes[object.second.meshID].vertexOffset, 0);
		}

		// End shadow mapping rendering
		m_vkCmdEndRenderingKHR(commandBuffer);
	}

	// Shadow mapping layout transition
	std::vector<VkImageMemoryBarrier2> depthStencilAttachmentToShaderReadOnlyImageMemoryBarriers;
	for (size_t directionalLightIndex = 0; directionalLightIndex < m_directionalLightEntities.size(); directionalLightIndex++) {
		VkImageMemoryBarrier2 depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier = {};
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.pNext = nullptr;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.srcQueueFamilyIndex = m_graphicsQueueFamilyIndex;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.dstQueueFamilyIndex = m_graphicsQueueFamilyIndex;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.image = m_directionalLightShadowMaps[directionalLightIndex].shadowMap.handle;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.baseMipLevel = 0;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.levelCount = 1;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.layerCount = SHADOW_MAPPING_CASCADE_COUNT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarriers.push_back(depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier);
	}
	for (size_t spotLightIndex = 0; spotLightIndex < m_spotLightEntities.size(); spotLightIndex++) {
		VkImageMemoryBarrier2 depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier = {};
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.pNext = nullptr;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.srcQueueFamilyIndex = m_graphicsQueueFamilyIndex;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.dstQueueFamilyIndex = m_graphicsQueueFamilyIndex;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.image = m_spotLightShadowMaps[spotLightIndex].shadowMap.handle;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.baseMipLevel = 0;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.levelCount = 1;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier.subresourceRange.layerCount = 1;
		depthStencilAttachmentToShaderReadOnlyImageMemoryBarriers.push_back(depthStencilAttachmentToShaderReadOnlyImageMemoryBarrier);
	}

	VkDependencyInfo depthStencilAttachmentToShaderReadDependencyInfo = {};
	depthStencilAttachmentToShaderReadDependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	depthStencilAttachmentToShaderReadDependencyInfo.pNext = nullptr;
	depthStencilAttachmentToShaderReadDependencyInfo.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	depthStencilAttachmentToShaderReadDependencyInfo.memoryBarrierCount = 0;
	depthStencilAttachmentToShaderReadDependencyInfo.pMemoryBarriers = nullptr;
	depthStencilAttachmentToShaderReadDependencyInfo.bufferMemoryBarrierCount = 0;
	depthStencilAttachmentToShaderReadDependencyInfo.pBufferMemoryBarriers = nullptr;
	depthStencilAttachmentToShaderReadDependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(depthStencilAttachmentToShaderReadOnlyImageMemoryBarriers.size());
	depthStencilAttachmentToShaderReadDependencyInfo.pImageMemoryBarriers = depthStencilAttachmentToShaderReadOnlyImageMemoryBarriers.data();
	m_vkCmdPipelineBarrier2KHR(commandBuffer, &depthStencilAttachmentToShaderReadDependencyInfo);
}

void ShadowMapping::descriptorSetNeedsUpdate(uint32_t frameInFlight) {
	m_descriptorSetsNeedUpdate[frameInFlight] = true;
}

void ShadowMapping::updateDescriptorSets(uint32_t frameInFlight, const std::vector<InternalTexture>& textures, const std::vector<VkImageView>& textureImageViews, const std::unordered_map<std::string, VkSampler>& textureSamplers) {
	if (!m_descriptorSetsNeedUpdate[frameInFlight]) {
		return;
	}

	std::vector<VkDescriptorImageInfo> texturesDescriptorImageInfos(textures.size());
	for (size_t i = 0; i < textures.size(); i++) {
		texturesDescriptorImageInfos[i].sampler = textureSamplers.at(textures[i].samplerKey);
		texturesDescriptorImageInfos[i].imageView = textureImageViews[textures[i].imageID];
		texturesDescriptorImageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkWriteDescriptorSet texturesDescriptorWriteDescriptorSet = {};
	texturesDescriptorWriteDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	texturesDescriptorWriteDescriptorSet.pNext = nullptr;
	texturesDescriptorWriteDescriptorSet.dstSet = m_descriptorSets[frameInFlight];
	texturesDescriptorWriteDescriptorSet.dstBinding = 3;
	texturesDescriptorWriteDescriptorSet.dstArrayElement = 0;
	texturesDescriptorWriteDescriptorSet.descriptorCount = static_cast<uint32_t>(texturesDescriptorImageInfos.size());
	texturesDescriptorWriteDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	texturesDescriptorWriteDescriptorSet.pImageInfo = texturesDescriptorImageInfos.data();
	texturesDescriptorWriteDescriptorSet.pBufferInfo = nullptr;
	texturesDescriptorWriteDescriptorSet.pTexelBufferView = nullptr;

	vkUpdateDescriptorSets(m_device, 1, &texturesDescriptorWriteDescriptorSet, 0, nullptr);

	m_descriptorSetsNeedUpdate[frameInFlight] = false;
}

void ShadowMapping::createDirectionalLightShadowMap(NtshEngn::Entity entity) {
	m_directionalLightEntities.push_back(entity);
	if (m_directionalLightShadowMaps.size() >= m_directionalLightEntities.size()) {
		return;
	}

	LayeredVulkanImage shadowMap;

	VkExtent3D imageExtent;
	imageExtent.width = SHADOW_MAPPING_RESOLUTION;
	imageExtent.height = SHADOW_MAPPING_RESOLUTION;
	imageExtent.depth = 1;

	VkImageCreateInfo shadowMapCreateInfo = {};
	shadowMapCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	shadowMapCreateInfo.pNext = nullptr;
	shadowMapCreateInfo.flags = 0;
	shadowMapCreateInfo.imageType = VK_IMAGE_TYPE_2D;
	shadowMapCreateInfo.format = VK_FORMAT_D32_SFLOAT;
	shadowMapCreateInfo.extent = imageExtent;
	shadowMapCreateInfo.mipLevels = 1;
	shadowMapCreateInfo.arrayLayers = SHADOW_MAPPING_CASCADE_COUNT;
	shadowMapCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	shadowMapCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	shadowMapCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	shadowMapCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	shadowMapCreateInfo.queueFamilyIndexCount = 1;
	shadowMapCreateInfo.pQueueFamilyIndices = &m_graphicsQueueFamilyIndex;
	shadowMapCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo shadowMapAllocationCreateInfo = {};
	shadowMapAllocationCreateInfo.flags = 0;
	shadowMapAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	NTSHENGN_VK_CHECK(vmaCreateImage(m_allocator, &shadowMapCreateInfo, &shadowMapAllocationCreateInfo, &shadowMap.handle, &shadowMap.allocation, nullptr));

	for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_MAPPING_CASCADE_COUNT; cascadeIndex++) {
		VkImageView shadowMapView;

		VkImageViewCreateInfo shadowMapViewCreateInfo = {};
		shadowMapViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		shadowMapViewCreateInfo.pNext = nullptr;
		shadowMapViewCreateInfo.flags = 0;
		shadowMapViewCreateInfo.image = shadowMap.handle;
		shadowMapViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		shadowMapViewCreateInfo.format = VK_FORMAT_D32_SFLOAT;
		shadowMapViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_R;
		shadowMapViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_G;
		shadowMapViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_B;
		shadowMapViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_A;
		shadowMapViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		shadowMapViewCreateInfo.subresourceRange.baseMipLevel = 0;
		shadowMapViewCreateInfo.subresourceRange.levelCount = 1;
		shadowMapViewCreateInfo.subresourceRange.baseArrayLayer = cascadeIndex;
		shadowMapViewCreateInfo.subresourceRange.layerCount = 1;
		NTSHENGN_VK_CHECK(vkCreateImageView(m_device, &shadowMapViewCreateInfo, nullptr, &shadowMapView));

		shadowMap.views.push_back(shadowMapView);
	}

	VkImageView shadowMapView;

	VkImageViewCreateInfo shadowMapViewCreateInfo = {};
	shadowMapViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	shadowMapViewCreateInfo.pNext = nullptr;
	shadowMapViewCreateInfo.flags = 0;
	shadowMapViewCreateInfo.image = shadowMap.handle;
	shadowMapViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	shadowMapViewCreateInfo.format = VK_FORMAT_D32_SFLOAT;
	shadowMapViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_R;
	shadowMapViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_G;
	shadowMapViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_B;
	shadowMapViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_A;
	shadowMapViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	shadowMapViewCreateInfo.subresourceRange.baseMipLevel = 0;
	shadowMapViewCreateInfo.subresourceRange.levelCount = 1;
	shadowMapViewCreateInfo.subresourceRange.baseArrayLayer = 0;
	shadowMapViewCreateInfo.subresourceRange.layerCount = SHADOW_MAPPING_CASCADE_COUNT;
	NTSHENGN_VK_CHECK(vkCreateImageView(m_device, &shadowMapViewCreateInfo, nullptr, &shadowMapView));

	shadowMap.views.push_back(shadowMapView);

	DirectionalLightShadowMap directionalLightShadowMap;
	directionalLightShadowMap.shadowMap = shadowMap;
	m_directionalLightShadowMaps.push_back(directionalLightShadowMap);

	// Layout transition VK_IMAGE_LAYOUT_UNDEFINED -> VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	VkCommandPool commandPool;

	VkCommandPoolCreateInfo commandPoolCreateInfo = {};
	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.pNext = nullptr;
	commandPoolCreateInfo.flags = 0;
	commandPoolCreateInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
	NTSHENGN_VK_CHECK(vkCreateCommandPool(m_device, &commandPoolCreateInfo, nullptr, &commandPool));

	VkCommandBuffer commandBuffer;

	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {};
	commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocateInfo.pNext = nullptr;
	commandBufferAllocateInfo.commandPool = commandPool;
	commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocateInfo.commandBufferCount = 1;
	NTSHENGN_VK_CHECK(vkAllocateCommandBuffers(m_device, &commandBufferAllocateInfo, &commandBuffer));

	VkCommandBufferBeginInfo commandBufferBeginInfo = {};
	commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	commandBufferBeginInfo.pNext = nullptr;
	commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	commandBufferBeginInfo.pInheritanceInfo = nullptr;
	NTSHENGN_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo));

	VkImageMemoryBarrier2 shadowMapImageMemoryBarrier = {};
	shadowMapImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	shadowMapImageMemoryBarrier.pNext = nullptr;
	shadowMapImageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
	shadowMapImageMemoryBarrier.srcAccessMask = 0;
	shadowMapImageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	shadowMapImageMemoryBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	shadowMapImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	shadowMapImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	shadowMapImageMemoryBarrier.srcQueueFamilyIndex = m_graphicsQueueFamilyIndex;
	shadowMapImageMemoryBarrier.dstQueueFamilyIndex = m_graphicsQueueFamilyIndex;
	shadowMapImageMemoryBarrier.image = shadowMap.handle;
	shadowMapImageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	shadowMapImageMemoryBarrier.subresourceRange.baseMipLevel = 0;
	shadowMapImageMemoryBarrier.subresourceRange.levelCount = 1;
	shadowMapImageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
	shadowMapImageMemoryBarrier.subresourceRange.layerCount = SHADOW_MAPPING_CASCADE_COUNT;
	
	VkDependencyInfo dependencyInfo = {};
	dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dependencyInfo.pNext = nullptr;
	dependencyInfo.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	dependencyInfo.memoryBarrierCount = 0;
	dependencyInfo.pMemoryBarriers = nullptr;
	dependencyInfo.bufferMemoryBarrierCount = 0;
	dependencyInfo.pBufferMemoryBarriers = nullptr;
	dependencyInfo.imageMemoryBarrierCount = 1;
	dependencyInfo.pImageMemoryBarriers = &shadowMapImageMemoryBarrier;
	m_vkCmdPipelineBarrier2KHR(commandBuffer, &dependencyInfo);

	NTSHENGN_VK_CHECK(vkEndCommandBuffer(commandBuffer));

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.pNext = nullptr;
	submitInfo.waitSemaphoreCount = 0;
	submitInfo.pWaitSemaphores = nullptr;
	submitInfo.pWaitDstStageMask = nullptr;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	submitInfo.signalSemaphoreCount = 0;
	submitInfo.pSignalSemaphores = nullptr;
	NTSHENGN_VK_CHECK(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_initializationFence));
	NTSHENGN_VK_CHECK(vkWaitForFences(m_device, 1, &m_initializationFence, VK_TRUE, std::numeric_limits<uint64_t>::max()));
	NTSHENGN_VK_CHECK(vkResetFences(m_device, 1, &m_initializationFence));

	vkDestroyCommandPool(m_device, commandPool, nullptr);
}

void ShadowMapping::destroyDirectionalLightShadowMap(NtshEngn::Entity entity) {
	for (auto it = m_directionalLightEntities.begin(); it != m_directionalLightEntities.end(); it++) {
		if (*it == entity) {
			m_directionalLightEntities.erase(it);
			return;
		}
	}
}

void ShadowMapping::createSpotLightShadowMap(NtshEngn::Entity entity) {
	m_spotLightEntities.push_back(entity);
	if (m_spotLightShadowMaps.size() >= m_spotLightEntities.size()) {
		return;
	}

	LayeredVulkanImage shadowMap;

	VkExtent3D imageExtent;
	imageExtent.width = SHADOW_MAPPING_RESOLUTION;
	imageExtent.height = SHADOW_MAPPING_RESOLUTION;
	imageExtent.depth = 1;

	VkImageCreateInfo shadowMapCreateInfo = {};
	shadowMapCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	shadowMapCreateInfo.pNext = nullptr;
	shadowMapCreateInfo.flags = 0;
	shadowMapCreateInfo.imageType = VK_IMAGE_TYPE_2D;
	shadowMapCreateInfo.format = VK_FORMAT_D32_SFLOAT;
	shadowMapCreateInfo.extent = imageExtent;
	shadowMapCreateInfo.mipLevels = 1;
	shadowMapCreateInfo.arrayLayers = 1;
	shadowMapCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	shadowMapCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	shadowMapCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	shadowMapCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	shadowMapCreateInfo.queueFamilyIndexCount = 1;
	shadowMapCreateInfo.pQueueFamilyIndices = &m_graphicsQueueFamilyIndex;
	shadowMapCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo shadowMapAllocationCreateInfo = {};
	shadowMapAllocationCreateInfo.flags = 0;
	shadowMapAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	NTSHENGN_VK_CHECK(vmaCreateImage(m_allocator, &shadowMapCreateInfo, &shadowMapAllocationCreateInfo, &shadowMap.handle, &shadowMap.allocation, nullptr));

	VkImageView shadowMapView;

	VkImageViewCreateInfo shadowMapViewCreateInfo = {};
	shadowMapViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	shadowMapViewCreateInfo.pNext = nullptr;
	shadowMapViewCreateInfo.flags = 0;
	shadowMapViewCreateInfo.image = shadowMap.handle;
	shadowMapViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	shadowMapViewCreateInfo.format = VK_FORMAT_D32_SFLOAT;
	shadowMapViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_R;
	shadowMapViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_G;
	shadowMapViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_B;
	shadowMapViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_A;
	shadowMapViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	shadowMapViewCreateInfo.subresourceRange.baseMipLevel = 0;
	shadowMapViewCreateInfo.subresourceRange.levelCount = 1;
	shadowMapViewCreateInfo.subresourceRange.baseArrayLayer = 0;
	shadowMapViewCreateInfo.subresourceRange.layerCount = 1;
	NTSHENGN_VK_CHECK(vkCreateImageView(m_device, &shadowMapViewCreateInfo, nullptr, &shadowMapView));

	shadowMap.views.push_back(shadowMapView);

	SpotLightShadowMap spotLightShadowMap;
	spotLightShadowMap.shadowMap = shadowMap;
	m_spotLightShadowMaps.push_back(spotLightShadowMap);

	// Layout transition VK_IMAGE_LAYOUT_UNDEFINED -> VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	VkCommandPool commandPool;

	VkCommandPoolCreateInfo commandPoolCreateInfo = {};
	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.pNext = nullptr;
	commandPoolCreateInfo.flags = 0;
	commandPoolCreateInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
	NTSHENGN_VK_CHECK(vkCreateCommandPool(m_device, &commandPoolCreateInfo, nullptr, &commandPool));

	VkCommandBuffer commandBuffer;

	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {};
	commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocateInfo.pNext = nullptr;
	commandBufferAllocateInfo.commandPool = commandPool;
	commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocateInfo.commandBufferCount = 1;
	NTSHENGN_VK_CHECK(vkAllocateCommandBuffers(m_device, &commandBufferAllocateInfo, &commandBuffer));

	VkCommandBufferBeginInfo commandBufferBeginInfo = {};
	commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	commandBufferBeginInfo.pNext = nullptr;
	commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	commandBufferBeginInfo.pInheritanceInfo = nullptr;
	NTSHENGN_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo));

	VkImageMemoryBarrier2 shadowMapImageMemoryBarrier = {};
	shadowMapImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	shadowMapImageMemoryBarrier.pNext = nullptr;
	shadowMapImageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
	shadowMapImageMemoryBarrier.srcAccessMask = 0;
	shadowMapImageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	shadowMapImageMemoryBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	shadowMapImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	shadowMapImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	shadowMapImageMemoryBarrier.srcQueueFamilyIndex = m_graphicsQueueFamilyIndex;
	shadowMapImageMemoryBarrier.dstQueueFamilyIndex = m_graphicsQueueFamilyIndex;
	shadowMapImageMemoryBarrier.image = shadowMap.handle;
	shadowMapImageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	shadowMapImageMemoryBarrier.subresourceRange.baseMipLevel = 0;
	shadowMapImageMemoryBarrier.subresourceRange.levelCount = 1;
	shadowMapImageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
	shadowMapImageMemoryBarrier.subresourceRange.layerCount = 1;

	VkDependencyInfo dependencyInfo = {};
	dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dependencyInfo.pNext = nullptr;
	dependencyInfo.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	dependencyInfo.memoryBarrierCount = 0;
	dependencyInfo.pMemoryBarriers = nullptr;
	dependencyInfo.bufferMemoryBarrierCount = 0;
	dependencyInfo.pBufferMemoryBarriers = nullptr;
	dependencyInfo.imageMemoryBarrierCount = 1;
	dependencyInfo.pImageMemoryBarriers = &shadowMapImageMemoryBarrier;
	m_vkCmdPipelineBarrier2KHR(commandBuffer, &dependencyInfo);

	NTSHENGN_VK_CHECK(vkEndCommandBuffer(commandBuffer));

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.pNext = nullptr;
	submitInfo.waitSemaphoreCount = 0;
	submitInfo.pWaitSemaphores = nullptr;
	submitInfo.pWaitDstStageMask = nullptr;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	submitInfo.signalSemaphoreCount = 0;
	submitInfo.pSignalSemaphores = nullptr;
	NTSHENGN_VK_CHECK(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_initializationFence));
	NTSHENGN_VK_CHECK(vkWaitForFences(m_device, 1, &m_initializationFence, VK_TRUE, std::numeric_limits<uint64_t>::max()));
	NTSHENGN_VK_CHECK(vkResetFences(m_device, 1, &m_initializationFence));

	vkDestroyCommandPool(m_device, commandPool, nullptr);
}

void ShadowMapping::destroySpotLightShadowMap(NtshEngn::Entity entity) {
	for (auto it = m_spotLightEntities.begin(); it != m_spotLightEntities.end(); it++) {
		if (*it == entity) {
			m_spotLightEntities.erase(it);
			return;
		}
	}
}

VulkanBuffer& ShadowMapping::getCascadeSceneBuffer(uint32_t frameInFlight) {
	return m_cascadeSceneBuffers[frameInFlight];
}

std::vector<LayeredVulkanImage> ShadowMapping::getShadowMapImages() {
	std::vector<LayeredVulkanImage> shadowMapImages;
	if (m_directionalLightEntities.empty() && m_spotLightEntities.empty()) {
		shadowMapImages.push_back(m_dummyShadowMap);
	}

	for (size_t i = 0; i < m_directionalLightEntities.size(); i++) {
		shadowMapImages.push_back(m_directionalLightShadowMaps[i].shadowMap);
	}
	for (size_t i = 0; i < m_spotLightEntities.size(); i++) {
		shadowMapImages.push_back(m_spotLightShadowMaps[i].shadowMap);
	}

	return shadowMapImages;
}

void ShadowMapping::createImageAndBuffers() {
	// Dummy shadow map
	VkImageCreateInfo dummyShadowMapImageCreateInfo = {};
	dummyShadowMapImageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	dummyShadowMapImageCreateInfo.pNext = nullptr;
	dummyShadowMapImageCreateInfo.flags = 0;
	dummyShadowMapImageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
	dummyShadowMapImageCreateInfo.format = VK_FORMAT_D32_SFLOAT;
	dummyShadowMapImageCreateInfo.extent.width = 1;
	dummyShadowMapImageCreateInfo.extent.height = 1;
	dummyShadowMapImageCreateInfo.extent.depth = 1;
	dummyShadowMapImageCreateInfo.mipLevels = 1;
	dummyShadowMapImageCreateInfo.arrayLayers = 1;
	dummyShadowMapImageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	dummyShadowMapImageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	dummyShadowMapImageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	dummyShadowMapImageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	dummyShadowMapImageCreateInfo.queueFamilyIndexCount = 1;
	dummyShadowMapImageCreateInfo.pQueueFamilyIndices = &m_graphicsQueueFamilyIndex;
	dummyShadowMapImageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo dummyShadowMapImageAllocationCreateInfo = {};
	dummyShadowMapImageAllocationCreateInfo.flags = 0;
	dummyShadowMapImageAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	NTSHENGN_VK_CHECK(vmaCreateImage(m_allocator, &dummyShadowMapImageCreateInfo, &dummyShadowMapImageAllocationCreateInfo, &m_dummyShadowMap.handle, &m_dummyShadowMap.allocation, nullptr));
	
	VkImageView dummyShadowMapView;

	VkImageViewCreateInfo dummyShadowMapImageViewCreateInfo = {};
	dummyShadowMapImageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	dummyShadowMapImageViewCreateInfo.pNext = nullptr;
	dummyShadowMapImageViewCreateInfo.flags = 0;
	dummyShadowMapImageViewCreateInfo.image = m_dummyShadowMap.handle;
	dummyShadowMapImageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	dummyShadowMapImageViewCreateInfo.format = VK_FORMAT_D32_SFLOAT;
	dummyShadowMapImageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_R;
	dummyShadowMapImageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_G;
	dummyShadowMapImageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_B;
	dummyShadowMapImageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_A;
	dummyShadowMapImageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	dummyShadowMapImageViewCreateInfo.subresourceRange.baseMipLevel = 0;
	dummyShadowMapImageViewCreateInfo.subresourceRange.levelCount = 1;
	dummyShadowMapImageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
	dummyShadowMapImageViewCreateInfo.subresourceRange.layerCount = 1;
	NTSHENGN_VK_CHECK(vkCreateImageView(m_device, &dummyShadowMapImageViewCreateInfo, nullptr, &dummyShadowMapView));

	m_dummyShadowMap.views.push_back(dummyShadowMapView);

	// Layout transition VK_IMAGE_LAYOUT_UNDEFINED -> VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	VkCommandPool commandPool;

	VkCommandPoolCreateInfo commandPoolCreateInfo = {};
	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.pNext = nullptr;
	commandPoolCreateInfo.flags = 0;
	commandPoolCreateInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
	NTSHENGN_VK_CHECK(vkCreateCommandPool(m_device, &commandPoolCreateInfo, nullptr, &commandPool));

	VkCommandBuffer commandBuffer;

	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {};
	commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocateInfo.pNext = nullptr;
	commandBufferAllocateInfo.commandPool = commandPool;
	commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocateInfo.commandBufferCount = 1;
	NTSHENGN_VK_CHECK(vkAllocateCommandBuffers(m_device, &commandBufferAllocateInfo, &commandBuffer));

	VkCommandBufferBeginInfo commandBufferBeginInfo = {};
	commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	commandBufferBeginInfo.pNext = nullptr;
	commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	commandBufferBeginInfo.pInheritanceInfo = nullptr;
	NTSHENGN_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo));

	VkImageMemoryBarrier2 dummyShadowMapImageMemoryBarrier = {};
	dummyShadowMapImageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	dummyShadowMapImageMemoryBarrier.pNext = nullptr;
	dummyShadowMapImageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
	dummyShadowMapImageMemoryBarrier.srcAccessMask = 0;
	dummyShadowMapImageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	dummyShadowMapImageMemoryBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	dummyShadowMapImageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	dummyShadowMapImageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dummyShadowMapImageMemoryBarrier.srcQueueFamilyIndex = m_graphicsQueueFamilyIndex;
	dummyShadowMapImageMemoryBarrier.dstQueueFamilyIndex = m_graphicsQueueFamilyIndex;
	dummyShadowMapImageMemoryBarrier.image = m_dummyShadowMap.handle;
	dummyShadowMapImageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	dummyShadowMapImageMemoryBarrier.subresourceRange.baseMipLevel = 0;
	dummyShadowMapImageMemoryBarrier.subresourceRange.levelCount = 1;
	dummyShadowMapImageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
	dummyShadowMapImageMemoryBarrier.subresourceRange.layerCount = 1;

	VkDependencyInfo dependencyInfo = {};
	dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dependencyInfo.pNext = nullptr;
	dependencyInfo.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	dependencyInfo.memoryBarrierCount = 0;
	dependencyInfo.pMemoryBarriers = nullptr;
	dependencyInfo.bufferMemoryBarrierCount = 0;
	dependencyInfo.pBufferMemoryBarriers = nullptr;
	dependencyInfo.imageMemoryBarrierCount = 1;
	dependencyInfo.pImageMemoryBarriers = &dummyShadowMapImageMemoryBarrier;
	m_vkCmdPipelineBarrier2KHR(commandBuffer, &dependencyInfo);

	NTSHENGN_VK_CHECK(vkEndCommandBuffer(commandBuffer));

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.pNext = nullptr;
	submitInfo.waitSemaphoreCount = 0;
	submitInfo.pWaitSemaphores = nullptr;
	submitInfo.pWaitDstStageMask = nullptr;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	submitInfo.signalSemaphoreCount = 0;
	submitInfo.pSignalSemaphores = nullptr;
	NTSHENGN_VK_CHECK(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_initializationFence));
	NTSHENGN_VK_CHECK(vkWaitForFences(m_device, 1, &m_initializationFence, VK_TRUE, std::numeric_limits<uint64_t>::max()));
	NTSHENGN_VK_CHECK(vkResetFences(m_device, 1, &m_initializationFence));

	vkDestroyCommandPool(m_device, commandPool, nullptr);

	// Buffers
	m_shadowBuffers.resize(m_framesInFlight);
	m_cascadeSceneBuffers.resize(m_framesInFlight);
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.pNext = nullptr;
	bufferCreateInfo.flags = 0;
	bufferCreateInfo.size = 65536;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bufferCreateInfo.queueFamilyIndexCount = 1;
	bufferCreateInfo.pQueueFamilyIndices = &m_graphicsQueueFamilyIndex;

	VmaAllocationCreateInfo bufferAllocationCreateInfo = {};
	bufferAllocationCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	bufferAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;

	for (uint32_t i = 0; i < m_framesInFlight; i++) {
		NTSHENGN_VK_CHECK(vmaCreateBuffer(m_allocator, &bufferCreateInfo, &bufferAllocationCreateInfo, &m_shadowBuffers[i].handle, &m_shadowBuffers[i].allocation, nullptr));
		NTSHENGN_VK_CHECK(vmaCreateBuffer(m_allocator, &bufferCreateInfo, &bufferAllocationCreateInfo, &m_cascadeSceneBuffers[i].handle, &m_cascadeSceneBuffers[i].allocation, nullptr));
	}
}

void ShadowMapping::createDescriptorSetLayout() {
	VkDescriptorSetLayoutBinding cascadeDescriptorSetLayoutBinding = {};
	cascadeDescriptorSetLayoutBinding.binding = 0;
	cascadeDescriptorSetLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	cascadeDescriptorSetLayoutBinding.descriptorCount = 1;
	cascadeDescriptorSetLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	cascadeDescriptorSetLayoutBinding.pImmutableSamplers = nullptr;

	VkDescriptorSetLayoutBinding objectsDescriptorSetLayoutBinding = {};
	objectsDescriptorSetLayoutBinding.binding = 1;
	objectsDescriptorSetLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	objectsDescriptorSetLayoutBinding.descriptorCount = 1;
	objectsDescriptorSetLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	objectsDescriptorSetLayoutBinding.pImmutableSamplers = nullptr;

	VkDescriptorSetLayoutBinding materialsDescriptorSetLayoutBinding = {};
	materialsDescriptorSetLayoutBinding.binding = 2;
	materialsDescriptorSetLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	materialsDescriptorSetLayoutBinding.descriptorCount = 1;
	materialsDescriptorSetLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	materialsDescriptorSetLayoutBinding.pImmutableSamplers = nullptr;

	VkDescriptorSetLayoutBinding texturesDescriptorSetLayoutBinding = {};
	texturesDescriptorSetLayoutBinding.binding = 3;
	texturesDescriptorSetLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	texturesDescriptorSetLayoutBinding.descriptorCount = 131072;
	texturesDescriptorSetLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	texturesDescriptorSetLayoutBinding.pImmutableSamplers = nullptr;

	std::array<VkDescriptorBindingFlags, 4> descriptorBindingFlags = { 0, 0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT };
	VkDescriptorSetLayoutBindingFlagsCreateInfo descriptorSetLayoutBindingFlagsCreateInfo = {};
	descriptorSetLayoutBindingFlagsCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	descriptorSetLayoutBindingFlagsCreateInfo.pNext = nullptr;
	descriptorSetLayoutBindingFlagsCreateInfo.bindingCount = static_cast<uint32_t>(descriptorBindingFlags.size());
	descriptorSetLayoutBindingFlagsCreateInfo.pBindingFlags = descriptorBindingFlags.data();

	std::array<VkDescriptorSetLayoutBinding, 4> descriptorSetLayoutBindings = { cascadeDescriptorSetLayoutBinding, objectsDescriptorSetLayoutBinding, materialsDescriptorSetLayoutBinding, texturesDescriptorSetLayoutBinding };
	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {};
	descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptorSetLayoutCreateInfo.pNext = &descriptorSetLayoutBindingFlagsCreateInfo;
	descriptorSetLayoutCreateInfo.flags = 0;
	descriptorSetLayoutCreateInfo.bindingCount = static_cast<uint32_t>(descriptorSetLayoutBindings.size());
	descriptorSetLayoutCreateInfo.pBindings = descriptorSetLayoutBindings.data();
	NTSHENGN_VK_CHECK(vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutCreateInfo, nullptr, &m_descriptorSetLayout));
}

void ShadowMapping::createGraphicsPipelines() {
	createDirectionalLightShadowGraphicsPipeline();
	createSpotLightShadowGraphicsPipeline();
}

void ShadowMapping::createDirectionalLightShadowGraphicsPipeline() {
	// Create graphics pipeline
	VkPipelineRenderingCreateInfo pipelineRenderingCreateInfo = {};
	pipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	pipelineRenderingCreateInfo.pNext = nullptr;
	pipelineRenderingCreateInfo.viewMask = 0;
	pipelineRenderingCreateInfo.colorAttachmentCount = 0;
	pipelineRenderingCreateInfo.pColorAttachmentFormats = nullptr;
	pipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
	pipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

	std::string vertexShaderCode = R"GLSL(
		#version 460

		#define SHADOW_MAPPING_CASCADE_COUNT )GLSL";
	vertexShaderCode += std::to_string(SHADOW_MAPPING_CASCADE_COUNT);
	vertexShaderCode += R"GLSL(

		struct ShadowInfo {
			mat4 viewProj[SHADOW_MAPPING_CASCADE_COUNT];
		};

		struct ObjectInfo {
			mat4 model;
			uint materialID;
		};

		layout(set = 0, binding = 0) restrict readonly buffer Shadows {
			ShadowInfo info[];
		} shadows;

		layout(std430, set = 0, binding = 1) restrict readonly buffer Objects {
			ObjectInfo info[];
		} objects;

		layout(push_constant) uniform PushConstants {
			uint lightIndex;
			uint cascadeIndex;
			uint objectID;
		} pC;

		layout(location = 0) in vec3 position;
		layout(location = 1) in vec2 uv;

		layout(location = 0) out vec2 outUv;
		layout(location = 1) out flat uint outMaterialID;

		void main() {
			outUv = uv;
			outMaterialID = objects.info[pC.objectID].materialID;

			gl_Position = shadows.info[pC.lightIndex].viewProj[pC.cascadeIndex] * objects.info[pC.objectID].model * vec4(position, 1.0);
		}
	)GLSL";
	const std::vector<uint32_t> vertexShaderSpv = compileShader(vertexShaderCode, ShaderType::Vertex);

	VkShaderModule vertexShaderModule;
	VkShaderModuleCreateInfo vertexShaderModuleCreateInfo = {};
	vertexShaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vertexShaderModuleCreateInfo.pNext = nullptr;
	vertexShaderModuleCreateInfo.flags = 0;
	vertexShaderModuleCreateInfo.codeSize = vertexShaderSpv.size() * sizeof(uint32_t);
	vertexShaderModuleCreateInfo.pCode = vertexShaderSpv.data();
	NTSHENGN_VK_CHECK(vkCreateShaderModule(m_device, &vertexShaderModuleCreateInfo, nullptr, &vertexShaderModule));

	VkPipelineShaderStageCreateInfo vertexShaderStageCreateInfo = {};
	vertexShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vertexShaderStageCreateInfo.pNext = nullptr;
	vertexShaderStageCreateInfo.flags = 0;
	vertexShaderStageCreateInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vertexShaderStageCreateInfo.module = vertexShaderModule;
	vertexShaderStageCreateInfo.pName = "main";
	vertexShaderStageCreateInfo.pSpecializationInfo = nullptr;

	const std::string fragmentShaderCode = R"GLSL(
		#version 460
		#extension GL_EXT_nonuniform_qualifier : enable

		struct MaterialInfo {
			uint diffuseTextureIndex;
			uint normalTextureIndex;
			uint metalnessTextureIndex;
			uint roughnessTextureIndex;
			uint occlusionTextureIndex;
			uint emissiveTextureIndex;
			float emissiveFactor;
			float alphaCutoff;
		};

		layout(set = 0, binding = 2) restrict readonly buffer Materials {
			MaterialInfo info[];
		} materials;

		layout(set = 0, binding = 3) uniform sampler2D textures[];

		layout(location = 0) in vec2 uv;
		layout(location = 1) in flat uint materialID;

		void main() {
			if (texture(textures[materials.info[materialID].diffuseTextureIndex], uv).a < materials.info[materialID].alphaCutoff) {
				discard;
			}
		}
	)GLSL";
	const std::vector<uint32_t> fragmentShaderSpv = compileShader(fragmentShaderCode, ShaderType::Fragment);

	VkShaderModule fragmentShaderModule;
	VkShaderModuleCreateInfo fragmentShaderModuleCreateInfo = {};
	fragmentShaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	fragmentShaderModuleCreateInfo.pNext = nullptr;
	fragmentShaderModuleCreateInfo.flags = 0;
	fragmentShaderModuleCreateInfo.codeSize = fragmentShaderSpv.size() * sizeof(uint32_t);
	fragmentShaderModuleCreateInfo.pCode = fragmentShaderSpv.data();
	NTSHENGN_VK_CHECK(vkCreateShaderModule(m_device, &fragmentShaderModuleCreateInfo, nullptr, &fragmentShaderModule));

	VkPipelineShaderStageCreateInfo fragmentShaderStageCreateInfo = {};
	fragmentShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	fragmentShaderStageCreateInfo.pNext = nullptr;
	fragmentShaderStageCreateInfo.flags = 0;
	fragmentShaderStageCreateInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	fragmentShaderStageCreateInfo.module = fragmentShaderModule;
	fragmentShaderStageCreateInfo.pName = "main";
	fragmentShaderStageCreateInfo.pSpecializationInfo = nullptr;

	std::array<VkPipelineShaderStageCreateInfo, 2> shaderStageCreateInfos = { vertexShaderStageCreateInfo, fragmentShaderStageCreateInfo };

	VkVertexInputBindingDescription vertexInputBindingDescription = {};
	vertexInputBindingDescription.binding = 0;
	vertexInputBindingDescription.stride = sizeof(NtshEngn::Vertex);
	vertexInputBindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription vertexPositionInputAttributeDescription = {};
	vertexPositionInputAttributeDescription.location = 0;
	vertexPositionInputAttributeDescription.binding = 0;
	vertexPositionInputAttributeDescription.format = VK_FORMAT_R32G32B32_SFLOAT;
	vertexPositionInputAttributeDescription.offset = 0;

	VkVertexInputAttributeDescription vertexUVInputAttributeDescription = {};
	vertexUVInputAttributeDescription.location = 1;
	vertexUVInputAttributeDescription.binding = 0;
	vertexUVInputAttributeDescription.format = VK_FORMAT_R32G32_SFLOAT;
	vertexUVInputAttributeDescription.offset = offsetof(NtshEngn::Vertex, uv);

	std::array<VkVertexInputAttributeDescription, 2> vertexInputAttributeDescriptions = { vertexPositionInputAttributeDescription, vertexUVInputAttributeDescription };
	VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo = {};
	vertexInputStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputStateCreateInfo.pNext = nullptr;
	vertexInputStateCreateInfo.flags = 0;
	vertexInputStateCreateInfo.vertexBindingDescriptionCount = 1;
	vertexInputStateCreateInfo.pVertexBindingDescriptions = &vertexInputBindingDescription;
	vertexInputStateCreateInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributeDescriptions.size());
	vertexInputStateCreateInfo.pVertexAttributeDescriptions = vertexInputAttributeDescriptions.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo = {};
	inputAssemblyStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssemblyStateCreateInfo.pNext = nullptr;
	inputAssemblyStateCreateInfo.flags = 0;
	inputAssemblyStateCreateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssemblyStateCreateInfo.primitiveRestartEnable = VK_FALSE;

	VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {};
	viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportStateCreateInfo.pNext = nullptr;
	viewportStateCreateInfo.flags = 0;
	viewportStateCreateInfo.viewportCount = 1;
	viewportStateCreateInfo.pViewports = &m_viewport;
	viewportStateCreateInfo.scissorCount = 1;
	viewportStateCreateInfo.pScissors = &m_scissor;

	VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo = {};
	rasterizationStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizationStateCreateInfo.pNext = nullptr;
	rasterizationStateCreateInfo.flags = 0;
	rasterizationStateCreateInfo.depthClampEnable = VK_TRUE;
	rasterizationStateCreateInfo.rasterizerDiscardEnable = VK_FALSE;
	rasterizationStateCreateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizationStateCreateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterizationStateCreateInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizationStateCreateInfo.depthBiasEnable = VK_FALSE;
	rasterizationStateCreateInfo.depthBiasConstantFactor = 0.0f;
	rasterizationStateCreateInfo.depthBiasClamp = 0.0f;
	rasterizationStateCreateInfo.depthBiasSlopeFactor = 0.0f;
	rasterizationStateCreateInfo.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampleStateCreateInfo = {};
	multisampleStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampleStateCreateInfo.pNext = nullptr;
	multisampleStateCreateInfo.flags = 0;
	multisampleStateCreateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisampleStateCreateInfo.sampleShadingEnable = VK_FALSE;
	multisampleStateCreateInfo.minSampleShading = 0.0f;
	multisampleStateCreateInfo.pSampleMask = nullptr;
	multisampleStateCreateInfo.alphaToCoverageEnable = VK_FALSE;
	multisampleStateCreateInfo.alphaToOneEnable = VK_FALSE;

	VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = {};
	depthStencilStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencilStateCreateInfo.pNext = nullptr;
	depthStencilStateCreateInfo.flags = 0;
	depthStencilStateCreateInfo.depthTestEnable = VK_TRUE;
	depthStencilStateCreateInfo.depthWriteEnable = VK_TRUE;
	depthStencilStateCreateInfo.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencilStateCreateInfo.depthBoundsTestEnable = VK_FALSE;
	depthStencilStateCreateInfo.stencilTestEnable = VK_FALSE;
	depthStencilStateCreateInfo.front = {};
	depthStencilStateCreateInfo.back = {};
	depthStencilStateCreateInfo.minDepthBounds = 0.0f;
	depthStencilStateCreateInfo.maxDepthBounds = 1.0f;

	VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo = {};
	colorBlendStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlendStateCreateInfo.pNext = nullptr;
	colorBlendStateCreateInfo.flags = 0;
	colorBlendStateCreateInfo.logicOpEnable = VK_FALSE;
	colorBlendStateCreateInfo.logicOp = VK_LOGIC_OP_COPY;
	colorBlendStateCreateInfo.attachmentCount = 0;
	colorBlendStateCreateInfo.pAttachments = nullptr;

	VkPushConstantRange pushConstantRange = {};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(uint32_t) * 3;

	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
	pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutCreateInfo.pNext = nullptr;
	pipelineLayoutCreateInfo.flags = 0;
	pipelineLayoutCreateInfo.setLayoutCount = 1;
	pipelineLayoutCreateInfo.pSetLayouts = &m_descriptorSetLayout;
	pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
	pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
	NTSHENGN_VK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_directionalLightShadowGraphicsPipelineLayout));

	VkGraphicsPipelineCreateInfo graphicsPipelineCreateInfo = {};
	graphicsPipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	graphicsPipelineCreateInfo.pNext = &pipelineRenderingCreateInfo;
	graphicsPipelineCreateInfo.flags = 0;
	graphicsPipelineCreateInfo.stageCount = 2;
	graphicsPipelineCreateInfo.pStages = shaderStageCreateInfos.data();
	graphicsPipelineCreateInfo.pVertexInputState = &vertexInputStateCreateInfo;
	graphicsPipelineCreateInfo.pInputAssemblyState = &inputAssemblyStateCreateInfo;
	graphicsPipelineCreateInfo.pTessellationState = nullptr;
	graphicsPipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
	graphicsPipelineCreateInfo.pRasterizationState = &rasterizationStateCreateInfo;
	graphicsPipelineCreateInfo.pMultisampleState = &multisampleStateCreateInfo;
	graphicsPipelineCreateInfo.pDepthStencilState = &depthStencilStateCreateInfo;
	graphicsPipelineCreateInfo.pColorBlendState = &colorBlendStateCreateInfo;
	graphicsPipelineCreateInfo.pDynamicState = nullptr;
	graphicsPipelineCreateInfo.layout = m_directionalLightShadowGraphicsPipelineLayout;
	graphicsPipelineCreateInfo.renderPass = VK_NULL_HANDLE;
	graphicsPipelineCreateInfo.subpass = 0;
	graphicsPipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
	graphicsPipelineCreateInfo.basePipelineIndex = 0;
	NTSHENGN_VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &graphicsPipelineCreateInfo, nullptr, &m_directionalLightShadowGraphicsPipeline));

	vkDestroyShaderModule(m_device, vertexShaderModule, nullptr);
	vkDestroyShaderModule(m_device, fragmentShaderModule, nullptr);
}

void ShadowMapping::createSpotLightShadowGraphicsPipeline() {
	// Create graphics pipeline
	VkPipelineRenderingCreateInfo pipelineRenderingCreateInfo = {};
	pipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	pipelineRenderingCreateInfo.pNext = nullptr;
	pipelineRenderingCreateInfo.viewMask = 0;
	pipelineRenderingCreateInfo.colorAttachmentCount = 0;
	pipelineRenderingCreateInfo.pColorAttachmentFormats = nullptr;
	pipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
	pipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

	std::string vertexShaderCode = R"GLSL(
		#version 460

		struct ShadowInfo {
			mat4 viewProj;
		};

		struct ObjectInfo {
			mat4 model;
			uint materialID;
		};

		layout(set = 0, binding = 0) restrict readonly buffer Shadows {
			ShadowInfo info[];
		} shadows;

		layout(std430, set = 0, binding = 1) restrict readonly buffer Objects {
			ObjectInfo info[];
		} objects;

		layout(push_constant) uniform PushConstants {
			uint lightIndex;
			uint objectID;
		} pC;

		layout(location = 0) in vec3 position;
		layout(location = 1) in vec2 uv;

		layout(location = 0) out vec2 outUv;
		layout(location = 1) out flat uint outMaterialID;

		void main() {
			outUv = uv;
			outMaterialID = objects.info[pC.objectID].materialID;

			gl_Position = shadows.info[pC.lightIndex].viewProj * objects.info[pC.objectID].model * vec4(position, 1.0);
		}
	)GLSL";
	const std::vector<uint32_t> vertexShaderSpv = compileShader(vertexShaderCode, ShaderType::Vertex);

	VkShaderModule vertexShaderModule;
	VkShaderModuleCreateInfo vertexShaderModuleCreateInfo = {};
	vertexShaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vertexShaderModuleCreateInfo.pNext = nullptr;
	vertexShaderModuleCreateInfo.flags = 0;
	vertexShaderModuleCreateInfo.codeSize = vertexShaderSpv.size() * sizeof(uint32_t);
	vertexShaderModuleCreateInfo.pCode = vertexShaderSpv.data();
	NTSHENGN_VK_CHECK(vkCreateShaderModule(m_device, &vertexShaderModuleCreateInfo, nullptr, &vertexShaderModule));

	VkPipelineShaderStageCreateInfo vertexShaderStageCreateInfo = {};
	vertexShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vertexShaderStageCreateInfo.pNext = nullptr;
	vertexShaderStageCreateInfo.flags = 0;
	vertexShaderStageCreateInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vertexShaderStageCreateInfo.module = vertexShaderModule;
	vertexShaderStageCreateInfo.pName = "main";
	vertexShaderStageCreateInfo.pSpecializationInfo = nullptr;

	const std::string fragmentShaderCode = R"GLSL(
		#version 460
		#extension GL_EXT_nonuniform_qualifier : enable

		struct MaterialInfo {
			uint diffuseTextureIndex;
			uint normalTextureIndex;
			uint metalnessTextureIndex;
			uint roughnessTextureIndex;
			uint occlusionTextureIndex;
			uint emissiveTextureIndex;
			float emissiveFactor;
			float alphaCutoff;
		};

		layout(set = 0, binding = 2) restrict readonly buffer Materials {
			MaterialInfo info[];
		} materials;

		layout(set = 0, binding = 3) uniform sampler2D textures[];

		layout(location = 0) in vec2 uv;
		layout(location = 1) in flat uint materialID;

		void main() {
			if (texture(textures[materials.info[materialID].diffuseTextureIndex], uv).a < materials.info[materialID].alphaCutoff) {
				discard;
			}
		}
	)GLSL";
	const std::vector<uint32_t> fragmentShaderSpv = compileShader(fragmentShaderCode, ShaderType::Fragment);

	VkShaderModule fragmentShaderModule;
	VkShaderModuleCreateInfo fragmentShaderModuleCreateInfo = {};
	fragmentShaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	fragmentShaderModuleCreateInfo.pNext = nullptr;
	fragmentShaderModuleCreateInfo.flags = 0;
	fragmentShaderModuleCreateInfo.codeSize = fragmentShaderSpv.size() * sizeof(uint32_t);
	fragmentShaderModuleCreateInfo.pCode = fragmentShaderSpv.data();
	NTSHENGN_VK_CHECK(vkCreateShaderModule(m_device, &fragmentShaderModuleCreateInfo, nullptr, &fragmentShaderModule));

	VkPipelineShaderStageCreateInfo fragmentShaderStageCreateInfo = {};
	fragmentShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	fragmentShaderStageCreateInfo.pNext = nullptr;
	fragmentShaderStageCreateInfo.flags = 0;
	fragmentShaderStageCreateInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	fragmentShaderStageCreateInfo.module = fragmentShaderModule;
	fragmentShaderStageCreateInfo.pName = "main";
	fragmentShaderStageCreateInfo.pSpecializationInfo = nullptr;

	std::array<VkPipelineShaderStageCreateInfo, 2> shaderStageCreateInfos = { vertexShaderStageCreateInfo, fragmentShaderStageCreateInfo };

	VkVertexInputBindingDescription vertexInputBindingDescription = {};
	vertexInputBindingDescription.binding = 0;
	vertexInputBindingDescription.stride = sizeof(NtshEngn::Vertex);
	vertexInputBindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription vertexPositionInputAttributeDescription = {};
	vertexPositionInputAttributeDescription.location = 0;
	vertexPositionInputAttributeDescription.binding = 0;
	vertexPositionInputAttributeDescription.format = VK_FORMAT_R32G32B32_SFLOAT;
	vertexPositionInputAttributeDescription.offset = 0;

	VkVertexInputAttributeDescription vertexUVInputAttributeDescription = {};
	vertexUVInputAttributeDescription.location = 1;
	vertexUVInputAttributeDescription.binding = 0;
	vertexUVInputAttributeDescription.format = VK_FORMAT_R32G32_SFLOAT;
	vertexUVInputAttributeDescription.offset = offsetof(NtshEngn::Vertex, uv);

	std::array<VkVertexInputAttributeDescription, 2> vertexInputAttributeDescriptions = { vertexPositionInputAttributeDescription, vertexUVInputAttributeDescription };
	VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo = {};
	vertexInputStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputStateCreateInfo.pNext = nullptr;
	vertexInputStateCreateInfo.flags = 0;
	vertexInputStateCreateInfo.vertexBindingDescriptionCount = 1;
	vertexInputStateCreateInfo.pVertexBindingDescriptions = &vertexInputBindingDescription;
	vertexInputStateCreateInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributeDescriptions.size());
	vertexInputStateCreateInfo.pVertexAttributeDescriptions = vertexInputAttributeDescriptions.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo = {};
	inputAssemblyStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssemblyStateCreateInfo.pNext = nullptr;
	inputAssemblyStateCreateInfo.flags = 0;
	inputAssemblyStateCreateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssemblyStateCreateInfo.primitiveRestartEnable = VK_FALSE;

	VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {};
	viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportStateCreateInfo.pNext = nullptr;
	viewportStateCreateInfo.flags = 0;
	viewportStateCreateInfo.viewportCount = 1;
	viewportStateCreateInfo.pViewports = &m_viewport;
	viewportStateCreateInfo.scissorCount = 1;
	viewportStateCreateInfo.pScissors = &m_scissor;

	VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo = {};
	rasterizationStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizationStateCreateInfo.pNext = nullptr;
	rasterizationStateCreateInfo.flags = 0;
	rasterizationStateCreateInfo.depthClampEnable = VK_TRUE;
	rasterizationStateCreateInfo.rasterizerDiscardEnable = VK_FALSE;
	rasterizationStateCreateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizationStateCreateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterizationStateCreateInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizationStateCreateInfo.depthBiasEnable = VK_FALSE;
	rasterizationStateCreateInfo.depthBiasConstantFactor = 0.0f;
	rasterizationStateCreateInfo.depthBiasClamp = 0.0f;
	rasterizationStateCreateInfo.depthBiasSlopeFactor = 0.0f;
	rasterizationStateCreateInfo.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampleStateCreateInfo = {};
	multisampleStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampleStateCreateInfo.pNext = nullptr;
	multisampleStateCreateInfo.flags = 0;
	multisampleStateCreateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisampleStateCreateInfo.sampleShadingEnable = VK_FALSE;
	multisampleStateCreateInfo.minSampleShading = 0.0f;
	multisampleStateCreateInfo.pSampleMask = nullptr;
	multisampleStateCreateInfo.alphaToCoverageEnable = VK_FALSE;
	multisampleStateCreateInfo.alphaToOneEnable = VK_FALSE;

	VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = {};
	depthStencilStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencilStateCreateInfo.pNext = nullptr;
	depthStencilStateCreateInfo.flags = 0;
	depthStencilStateCreateInfo.depthTestEnable = VK_TRUE;
	depthStencilStateCreateInfo.depthWriteEnable = VK_TRUE;
	depthStencilStateCreateInfo.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencilStateCreateInfo.depthBoundsTestEnable = VK_FALSE;
	depthStencilStateCreateInfo.stencilTestEnable = VK_FALSE;
	depthStencilStateCreateInfo.front = {};
	depthStencilStateCreateInfo.back = {};
	depthStencilStateCreateInfo.minDepthBounds = 0.0f;
	depthStencilStateCreateInfo.maxDepthBounds = 1.0f;

	VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo = {};
	colorBlendStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlendStateCreateInfo.pNext = nullptr;
	colorBlendStateCreateInfo.flags = 0;
	colorBlendStateCreateInfo.logicOpEnable = VK_FALSE;
	colorBlendStateCreateInfo.logicOp = VK_LOGIC_OP_COPY;
	colorBlendStateCreateInfo.attachmentCount = 0;
	colorBlendStateCreateInfo.pAttachments = nullptr;

	VkPushConstantRange pushConstantRange = {};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(uint32_t) * 2;

	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
	pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutCreateInfo.pNext = nullptr;
	pipelineLayoutCreateInfo.flags = 0;
	pipelineLayoutCreateInfo.setLayoutCount = 1;
	pipelineLayoutCreateInfo.pSetLayouts = &m_descriptorSetLayout;
	pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
	pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
	NTSHENGN_VK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_spotLightShadowGraphicsPipelineLayout));

	VkGraphicsPipelineCreateInfo graphicsPipelineCreateInfo = {};
	graphicsPipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	graphicsPipelineCreateInfo.pNext = &pipelineRenderingCreateInfo;
	graphicsPipelineCreateInfo.flags = 0;
	graphicsPipelineCreateInfo.stageCount = 2;
	graphicsPipelineCreateInfo.pStages = shaderStageCreateInfos.data();
	graphicsPipelineCreateInfo.pVertexInputState = &vertexInputStateCreateInfo;
	graphicsPipelineCreateInfo.pInputAssemblyState = &inputAssemblyStateCreateInfo;
	graphicsPipelineCreateInfo.pTessellationState = nullptr;
	graphicsPipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
	graphicsPipelineCreateInfo.pRasterizationState = &rasterizationStateCreateInfo;
	graphicsPipelineCreateInfo.pMultisampleState = &multisampleStateCreateInfo;
	graphicsPipelineCreateInfo.pDepthStencilState = &depthStencilStateCreateInfo;
	graphicsPipelineCreateInfo.pColorBlendState = &colorBlendStateCreateInfo;
	graphicsPipelineCreateInfo.pDynamicState = nullptr;
	graphicsPipelineCreateInfo.layout = m_spotLightShadowGraphicsPipelineLayout;
	graphicsPipelineCreateInfo.renderPass = VK_NULL_HANDLE;
	graphicsPipelineCreateInfo.subpass = 0;
	graphicsPipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
	graphicsPipelineCreateInfo.basePipelineIndex = 0;
	NTSHENGN_VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &graphicsPipelineCreateInfo, nullptr, &m_spotLightShadowGraphicsPipeline));

	vkDestroyShaderModule(m_device, vertexShaderModule, nullptr);
	vkDestroyShaderModule(m_device, fragmentShaderModule, nullptr);
}

void ShadowMapping::createDescriptorSets(const std::vector<VulkanBuffer>& objectBuffers, const std::vector<VulkanBuffer>& materialBuffers) {
	// Create descriptor pool
	VkDescriptorPoolSize cascadeDescriptorPoolSize = {};
	cascadeDescriptorPoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	cascadeDescriptorPoolSize.descriptorCount = m_framesInFlight;

	VkDescriptorPoolSize objectsDescriptorPoolSize = {};
	objectsDescriptorPoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	objectsDescriptorPoolSize.descriptorCount = m_framesInFlight;

	VkDescriptorPoolSize materialsDescriptorPoolSize = {};
	materialsDescriptorPoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	materialsDescriptorPoolSize.descriptorCount = m_framesInFlight;

	VkDescriptorPoolSize texturesDescriptorPoolSize = {};
	texturesDescriptorPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	texturesDescriptorPoolSize.descriptorCount = 131072 * m_framesInFlight;

	std::array<VkDescriptorPoolSize, 4> descriptorPoolSizes = { cascadeDescriptorPoolSize, objectsDescriptorPoolSize, materialsDescriptorPoolSize, texturesDescriptorPoolSize };
	VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {};
	descriptorPoolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	descriptorPoolCreateInfo.pNext = nullptr;
	descriptorPoolCreateInfo.flags = 0;
	descriptorPoolCreateInfo.maxSets = m_framesInFlight;
	descriptorPoolCreateInfo.poolSizeCount = static_cast<uint32_t>(descriptorPoolSizes.size());
	descriptorPoolCreateInfo.pPoolSizes = descriptorPoolSizes.data();
	NTSHENGN_VK_CHECK(vkCreateDescriptorPool(m_device, &descriptorPoolCreateInfo, nullptr, &m_descriptorPool));

	// Allocate descriptor sets
	m_descriptorSets.resize(m_framesInFlight);
	for (uint32_t i = 0; i < m_framesInFlight; i++) {
		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {};
		descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptorSetAllocateInfo.pNext = nullptr;
		descriptorSetAllocateInfo.descriptorPool = m_descriptorPool;
		descriptorSetAllocateInfo.descriptorSetCount = 1;
		descriptorSetAllocateInfo.pSetLayouts = &m_descriptorSetLayout;
		NTSHENGN_VK_CHECK(vkAllocateDescriptorSets(m_device, &descriptorSetAllocateInfo, &m_descriptorSets[i]));
	}

	// Update descriptor sets
	for (uint32_t i = 0; i < m_framesInFlight; i++) {
		std::vector<VkWriteDescriptorSet> writeDescriptorSets;

		VkDescriptorBufferInfo cascadeDescriptorBufferInfo;
		cascadeDescriptorBufferInfo.buffer = m_shadowBuffers[i].handle;
		cascadeDescriptorBufferInfo.offset = 0;
		cascadeDescriptorBufferInfo.range = 65536;

		VkWriteDescriptorSet cascadeDescriptorWriteDescriptorSet = {};
		cascadeDescriptorWriteDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		cascadeDescriptorWriteDescriptorSet.pNext = nullptr;
		cascadeDescriptorWriteDescriptorSet.dstSet = m_descriptorSets[i];
		cascadeDescriptorWriteDescriptorSet.dstBinding = 0;
		cascadeDescriptorWriteDescriptorSet.dstArrayElement = 0;
		cascadeDescriptorWriteDescriptorSet.descriptorCount = 1;
		cascadeDescriptorWriteDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		cascadeDescriptorWriteDescriptorSet.pImageInfo = nullptr;
		cascadeDescriptorWriteDescriptorSet.pBufferInfo = &cascadeDescriptorBufferInfo;
		cascadeDescriptorWriteDescriptorSet.pTexelBufferView = nullptr;
		writeDescriptorSets.push_back(cascadeDescriptorWriteDescriptorSet);

		VkDescriptorBufferInfo objectsDescriptorBufferInfo;
		objectsDescriptorBufferInfo.buffer = objectBuffers[i].handle;
		objectsDescriptorBufferInfo.offset = 0;
		objectsDescriptorBufferInfo.range = 32768;

		VkWriteDescriptorSet objectsDescriptorWriteDescriptorSet = {};
		objectsDescriptorWriteDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		objectsDescriptorWriteDescriptorSet.pNext = nullptr;
		objectsDescriptorWriteDescriptorSet.dstSet = m_descriptorSets[i];
		objectsDescriptorWriteDescriptorSet.dstBinding = 1;
		objectsDescriptorWriteDescriptorSet.dstArrayElement = 0;
		objectsDescriptorWriteDescriptorSet.descriptorCount = 1;
		objectsDescriptorWriteDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		objectsDescriptorWriteDescriptorSet.pImageInfo = nullptr;
		objectsDescriptorWriteDescriptorSet.pBufferInfo = &objectsDescriptorBufferInfo;
		objectsDescriptorWriteDescriptorSet.pTexelBufferView = nullptr;
		writeDescriptorSets.push_back(objectsDescriptorWriteDescriptorSet);

		VkDescriptorBufferInfo materialsDescriptorBufferInfo;
		materialsDescriptorBufferInfo.buffer = materialBuffers[i].handle;
		materialsDescriptorBufferInfo.offset = 0;
		materialsDescriptorBufferInfo.range = 32768;

		VkWriteDescriptorSet materialsDescriptorWriteDescriptorSet = {};
		materialsDescriptorWriteDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		materialsDescriptorWriteDescriptorSet.pNext = nullptr;
		materialsDescriptorWriteDescriptorSet.dstSet = m_descriptorSets[i];
		materialsDescriptorWriteDescriptorSet.dstBinding = 2;
		materialsDescriptorWriteDescriptorSet.dstArrayElement = 0;
		materialsDescriptorWriteDescriptorSet.descriptorCount = 1;
		materialsDescriptorWriteDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		materialsDescriptorWriteDescriptorSet.pImageInfo = nullptr;
		materialsDescriptorWriteDescriptorSet.pBufferInfo = &materialsDescriptorBufferInfo;
		materialsDescriptorWriteDescriptorSet.pTexelBufferView = nullptr;
		writeDescriptorSets.push_back(materialsDescriptorWriteDescriptorSet);

		vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);
	}

	m_descriptorSetsNeedUpdate.resize(m_framesInFlight);
	for (uint32_t i = 0; i < m_framesInFlight; i++) {
		m_descriptorSetsNeedUpdate[i] = false;
	}
}
