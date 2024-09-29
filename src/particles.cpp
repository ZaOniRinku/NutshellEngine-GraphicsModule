#include "particles.h"
#include <random>

void Particles::init(VkDevice device,
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
	PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2KHR) {
	m_device = device;
	m_graphicsComputeQueue = graphicsComputeQueue;
	m_graphicsComputeQueueFamilyIndex = graphicsComputeQueueFamilyIndex;
	m_initializationCommandPool = initializationCommandPool;
	m_initializationCommandBuffer = initializationCommandBuffer;
	m_initializationFence = initializationFence;
	m_allocator = allocator;
	m_viewport = viewport;
	m_scissor = scissor;
	m_framesInFlight = framesInFlight;
	m_vkCmdBeginRenderingKHR = vkCmdBeginRenderingKHR;
	m_vkCmdEndRenderingKHR = vkCmdEndRenderingKHR;
	m_vkCmdPipelineBarrier2KHR = vkCmdPipelineBarrier2KHR;

	createBuffers();
	createComputeResources();
	createGraphicsResources(drawImageFormat);
}

void Particles::destroy() {
	vkDestroyDescriptorPool(m_device, m_graphicsDescriptorPool, nullptr);
	vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
	vkDestroyPipelineLayout(m_device, m_graphicsPipelineLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_device, m_graphicsDescriptorSetLayout, nullptr);

	vkDestroyDescriptorPool(m_device, m_computeDescriptorPool, nullptr);
	vkDestroyPipeline(m_device, m_computePipeline, nullptr);
	vkDestroyPipelineLayout(m_device, m_computePipelineLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_device, m_computeDescriptorSetLayout, nullptr);

	vmaDestroyBuffer(m_allocator, m_drawIndirectBuffer.handle, m_drawIndirectBuffer.allocation);
	for (size_t i = 0; i < m_framesInFlight; i++) {
		vmaDestroyBuffer(m_allocator, m_stagingBuffers[i].handle, m_stagingBuffers[i].allocation);
	}
	for (size_t i = 0; i < m_particleBuffers.size(); i++) {
		vmaDestroyBuffer(m_allocator, m_particleBuffers[i].handle, m_particleBuffers[i].allocation);
	}
}

void Particles::draw(VkCommandBuffer commandBuffer, VkImage drawImage, VkImageView drawImageView, VkImageView depthImageView) {

}

void Particles::emitParticles(const NtshEngn::ParticleEmitter& particleEmitter, uint32_t currentFrameInFlight) {
	std::random_device r;
	std::default_random_engine randomEngine(r());
	std::uniform_real_distribution<float> randomDistribution(0.0f, 1.0f);

	std::vector<Particle> particles(particleEmitter.number);
	for (Particle& particle : particles) {
		particle.position = NtshEngn::Math::vec3(NtshEngn::Math::lerp(particleEmitter.positionRange[0].x, particleEmitter.positionRange[1].x, randomDistribution(randomEngine)),
			NtshEngn::Math::lerp(particleEmitter.positionRange[0].y, particleEmitter.positionRange[1].y, randomDistribution(randomEngine)),
			NtshEngn::Math::lerp(particleEmitter.positionRange[0].z, particleEmitter.positionRange[1].z, randomDistribution(randomEngine)));
		particle.size = NtshEngn::Math::lerp(particleEmitter.sizeRange[0], particleEmitter.sizeRange[1], randomDistribution(randomEngine));
		particle.color = NtshEngn::Math::vec4(NtshEngn::Math::lerp(particleEmitter.colorRange[0].x, particleEmitter.colorRange[1].x, randomDistribution(randomEngine)),
			NtshEngn::Math::lerp(particleEmitter.colorRange[0].y, particleEmitter.colorRange[1].y, randomDistribution(randomEngine)),
			NtshEngn::Math::lerp(particleEmitter.colorRange[0].z, particleEmitter.colorRange[1].y, randomDistribution(randomEngine)),
			NtshEngn::Math::lerp(particleEmitter.colorRange[0].w, particleEmitter.colorRange[1].w, randomDistribution(randomEngine)));
		NtshEngn::Math::vec3 rotation = NtshEngn::Math::vec3(NtshEngn::Math::lerp(particleEmitter.rotationRange[0].x, particleEmitter.rotationRange[1].x, randomDistribution(randomEngine)),
			NtshEngn::Math::lerp(particleEmitter.rotationRange[0].y, particleEmitter.rotationRange[1].y, randomDistribution(randomEngine)),
			NtshEngn::Math::lerp(particleEmitter.rotationRange[0].z, particleEmitter.rotationRange[1].z, randomDistribution(randomEngine)));
		const NtshEngn::Math::vec3 baseDirection = NtshEngn::Math::normalize(particleEmitter.baseDirection);
		const float baseDirectionYaw = std::atan2(baseDirection.z, baseDirection.x);
		const float baseDirectionPitch = -std::asin(baseDirection.y);
		particle.direction = NtshEngn::Math::normalize(NtshEngn::Math::vec3(
			std::cos(baseDirectionPitch + rotation.x) * std::cos(baseDirectionYaw + rotation.y),
			-std::sin(baseDirectionPitch + rotation.x),
			std::cos(baseDirectionPitch + rotation.x) * std::sin(baseDirectionYaw + rotation.y)
		));
		particle.speed = NtshEngn::Math::lerp(particleEmitter.speedRange[0], particleEmitter.speedRange[1], randomDistribution(randomEngine));
		particle.duration = NtshEngn::Math::lerp(particleEmitter.durationRange[0], particleEmitter.durationRange[1], randomDistribution(randomEngine));
	}

	size_t size = particles.size() * sizeof(Particle);
	memcpy(reinterpret_cast<uint8_t*>(m_stagingBuffers[currentFrameInFlight].address) + m_currentParticleHostSize, particles.data(), size);

	m_currentParticleHostSize += size;

	m_buffersNeedUpdate[currentFrameInFlight] = true;
}

void Particles::createBuffers() {
	// Create particle buffer
	VkBufferCreateInfo particleBufferCreateInfo = {};
	particleBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	particleBufferCreateInfo.pNext = nullptr;
	particleBufferCreateInfo.flags = 0;
	particleBufferCreateInfo.size = m_maxParticlesNumber * sizeof(Particle);
	particleBufferCreateInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	particleBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	particleBufferCreateInfo.queueFamilyIndexCount = 1;
	particleBufferCreateInfo.pQueueFamilyIndices = &m_graphicsComputeQueueFamilyIndex;

	VmaAllocationInfo particleBufferAllocationInfo;

	VmaAllocationCreateInfo particleBufferAllocationCreateInfo = {};
	particleBufferAllocationCreateInfo.flags = 0;
	particleBufferAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	NTSHENGN_VK_CHECK(vmaCreateBuffer(m_allocator, &particleBufferCreateInfo, &particleBufferAllocationCreateInfo, &m_particleBuffers[0].handle, &m_particleBuffers[0].allocation, &particleBufferAllocationInfo));
	NTSHENGN_VK_CHECK(vmaCreateBuffer(m_allocator, &particleBufferCreateInfo, &particleBufferAllocationCreateInfo, &m_particleBuffers[1].handle, &m_particleBuffers[1].allocation, &particleBufferAllocationInfo));

	// Create staging buffers
	m_stagingBuffers.resize(m_framesInFlight);
	VkBufferCreateInfo stagingBufferCreateInfo = {};
	stagingBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	stagingBufferCreateInfo.pNext = nullptr;
	stagingBufferCreateInfo.flags = 0;
	stagingBufferCreateInfo.size = m_maxParticlesNumber * sizeof(Particle);
	stagingBufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	stagingBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	stagingBufferCreateInfo.queueFamilyIndexCount = 1;
	stagingBufferCreateInfo.pQueueFamilyIndices = &m_graphicsComputeQueueFamilyIndex;

	VmaAllocationInfo stagingBufferAllocationInfo;

	VmaAllocationCreateInfo stagingBufferAllocationCreateInfo = {};
	stagingBufferAllocationCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	stagingBufferAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;

	for (uint32_t i = 0; i < m_framesInFlight; i++) {
		NTSHENGN_VK_CHECK(vmaCreateBuffer(m_allocator, &stagingBufferCreateInfo, &stagingBufferAllocationCreateInfo, &m_stagingBuffers[i].handle, &m_stagingBuffers[i].allocation, &stagingBufferAllocationInfo));
		m_stagingBuffers[i].address = stagingBufferAllocationInfo.pMappedData;
	}

	m_buffersNeedUpdate.resize(m_framesInFlight);
	for (uint32_t i = 0; i < m_framesInFlight; i++) {
		m_buffersNeedUpdate[i] = false;
	}

	// Create draw indirect buffer
	VkBufferCreateInfo drawIndirectBufferCreateInfo = {};
	drawIndirectBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	drawIndirectBufferCreateInfo.pNext = nullptr;
	drawIndirectBufferCreateInfo.flags = 0;
	drawIndirectBufferCreateInfo.size = 4 * sizeof(uint32_t);
	drawIndirectBufferCreateInfo.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	drawIndirectBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	drawIndirectBufferCreateInfo.queueFamilyIndexCount = 1;
	drawIndirectBufferCreateInfo.pQueueFamilyIndices = &m_graphicsComputeQueueFamilyIndex;

	VmaAllocationInfo particleDrawIndirectBufferAllocationInfo;

	VmaAllocationCreateInfo particleDrawIndirectBufferAllocationCreateInfo = {};
	particleDrawIndirectBufferAllocationCreateInfo.flags = 0;
	particleDrawIndirectBufferAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	NTSHENGN_VK_CHECK(vmaCreateBuffer(m_allocator, &drawIndirectBufferCreateInfo, &particleDrawIndirectBufferAllocationCreateInfo, &m_drawIndirectBuffer.handle, &m_drawIndirectBuffer.allocation, &particleDrawIndirectBufferAllocationInfo));

	// Fill draw indirect buffer
	NTSHENGN_VK_CHECK(vkResetCommandPool(m_device, m_initializationCommandPool, 0));

	VkCommandBufferBeginInfo fillDrawIndirectBufferBeginInfo = {};
	fillDrawIndirectBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	fillDrawIndirectBufferBeginInfo.pNext = nullptr;
	fillDrawIndirectBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	fillDrawIndirectBufferBeginInfo.pInheritanceInfo = nullptr;
	NTSHENGN_VK_CHECK(vkBeginCommandBuffer(m_initializationCommandBuffer, &fillDrawIndirectBufferBeginInfo));

	std::vector<uint32_t> drawIndirectData = { 1, 0, 0 };
	vkCmdUpdateBuffer(m_initializationCommandBuffer, m_drawIndirectBuffer.handle, sizeof(uint32_t), 3 * sizeof(uint32_t), drawIndirectData.data());

	NTSHENGN_VK_CHECK(vkEndCommandBuffer(m_initializationCommandBuffer));

	VkSubmitInfo fillDrawIndirectBufferSubmitInfo = {};
	fillDrawIndirectBufferSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	fillDrawIndirectBufferSubmitInfo.pNext = nullptr;
	fillDrawIndirectBufferSubmitInfo.waitSemaphoreCount = 0;
	fillDrawIndirectBufferSubmitInfo.pWaitSemaphores = nullptr;
	fillDrawIndirectBufferSubmitInfo.pWaitDstStageMask = nullptr;
	fillDrawIndirectBufferSubmitInfo.commandBufferCount = 1;
	fillDrawIndirectBufferSubmitInfo.pCommandBuffers = &m_initializationCommandBuffer;
	fillDrawIndirectBufferSubmitInfo.signalSemaphoreCount = 0;
	fillDrawIndirectBufferSubmitInfo.pSignalSemaphores = nullptr;
	NTSHENGN_VK_CHECK(vkQueueSubmit(m_graphicsComputeQueue, 1, &fillDrawIndirectBufferSubmitInfo, m_initializationFence));
	NTSHENGN_VK_CHECK(vkWaitForFences(m_device, 1, &m_initializationFence, VK_TRUE, std::numeric_limits<uint64_t>::max()));
	NTSHENGN_VK_CHECK(vkResetFences(m_device, 1, &m_initializationFence));
}

void Particles::createComputeResources() {

}

void Particles::createGraphicsResources(VkFormat drawImageFormat) {

}
