/*------------------------------------------------------------------------
* Vulkan Conformance Tests
* ------------------------
*
* Copyright (c) 2017 The Khronos Group Inc.
* Copyright (c) 2017 Nvidia Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*		http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
*//*!
* \file
* \brief Device Group Tests
*//*--------------------------------------------------------------------*/

#include "vktDeviceGroupTests.hpp"

#include "vkDefs.hpp"
#include "vkDeviceUtil.hpp"
#include "vkImageUtil.hpp"
#include "vkMemUtil.hpp"
#include "vkPlatform.hpp"
#include "vkPrograms.hpp"
#include "vkQueryUtil.hpp"
#include "vkRef.hpp"
#include "vkRefUtil.hpp"
#include "vkStrUtil.hpp"
#include "vkTypeUtil.hpp"
#include "vktTestCase.hpp"
#include "vktTestCaseUtil.hpp"
#include "vktTestGroupUtil.hpp"

#include "tcuDefs.hpp"
#include "tcuFormatUtil.hpp"
#include "tcuImageCompare.hpp"
#include "tcuResource.hpp"
#include "tcuTestCase.hpp"
#include "tcuTestLog.hpp"
#include "tcuCommandLine.hpp"
#include "tcuTextureUtil.hpp"
#include "tcuImageIO.hpp"

#include "rrRenderer.hpp"

namespace vkt
{
namespace DeviceGroup
{
namespace
{

using namespace vk;
using std::string;
using std::vector;
using tcu::TestLog;
using de::UniquePtr;

//Device group test modes
enum TestModeType
{
	TEST_MODE_SFR			= 1 << 0,			//!< Split frame remdering
	TEST_MODE_AFR			= 1 << 1,			//!< Alternate frame rendering
	TEST_MODE_HOSTMEMORY	= 1 << 2,			//!< Use host memory for rendertarget
	TEST_MODE_DEDICATED		= 1 << 3,			//!< Use dedicated allocations
	TEST_MODE_PEER_FETCH	= 1 << 4,			//!< Peer vertex attributes from peer memroy
	TEST_MODE_TESSELLATION	= 1 << 5,			//!< Generate a tessellated sphere instead of triangle
	TEST_MODE_LINEFILL		= 1 << 6,			//!< Draw polygon edges as line segments
};

class RefVertexShader : public rr::VertexShader
{
public:
	RefVertexShader (void)
		: rr::VertexShader(1, 0)
	{
		m_inputs[0].type = rr::GENERICVECTYPE_FLOAT;
	}
	virtual	~RefVertexShader(void) {}

	void shadeVertices (const rr::VertexAttrib* inputs, rr::VertexPacket* const* packets, const int numPackets) const
	{
		for (int packetNdx = 0; packetNdx < numPackets; ++packetNdx)
		{
			packets[packetNdx]->position = rr::readVertexAttribFloat(inputs[0],
				packets[packetNdx]->instanceNdx,
				packets[packetNdx]->vertexNdx);
		}
	}
};

class RefFragmentShader : public rr::FragmentShader
{
public:
	RefFragmentShader (void)
		: rr::FragmentShader(0, 1)
	{
		m_outputs[0].type = rr::GENERICVECTYPE_FLOAT;
	}

	virtual	~RefFragmentShader(void) {}

	void shadeFragments (rr::FragmentPacket*, const int numPackets, const rr::FragmentShadingContext& context) const
	{
		for (int packetNdx = 0; packetNdx < numPackets; ++packetNdx)
		{
			for (int fragNdx = 0; fragNdx < rr::NUM_FRAGMENTS_PER_PACKET; ++fragNdx)
			{
				rr::writeFragmentOutput(context, packetNdx, fragNdx, 0, tcu::Vec4(1.0f, 1.0f, 0.0f, 1.0f));
			}
		}
	}
};

void renderReferenceTriangle (const tcu::PixelBufferAccess& dst, const tcu::Vec4(&vertices)[3])
{
	const RefVertexShader					vertShader;
	const RefFragmentShader					fragShader;
	const rr::Program						program(&vertShader, &fragShader);
	const rr::MultisamplePixelBufferAccess	colorBuffer = rr::MultisamplePixelBufferAccess::fromSinglesampleAccess(dst);
	const rr::RenderTarget					renderTarget(colorBuffer);
	const rr::RenderState					renderState((rr::ViewportState(colorBuffer)));
	const rr::Renderer						renderer;
	const rr::VertexAttrib					vertexAttribs[] =
	{
		rr::VertexAttrib(rr::VERTEXATTRIBTYPE_FLOAT, 4, sizeof(tcu::Vec4), 0, vertices[0].getPtr())
	};
	renderer.draw(rr::DrawCommand(renderState,
		renderTarget,
		program,
		DE_LENGTH_OF_ARRAY(vertexAttribs),
		&vertexAttribs[0],
		rr::PrimitiveList(rr::PRIMITIVETYPE_TRIANGLES, DE_LENGTH_OF_ARRAY(vertices), 0)));
}

class DeviceGroupTestInstance : public TestInstance
{
public:
	DeviceGroupTestInstance(Context& context, deUint32 mode);
	~DeviceGroupTestInstance(void) {}
private:
			void						init					(void);
			deUint32					getMemoryIndex			(deUint32 memoryTypeBits, deUint32 memoryPropertyFlag);
			void						getDeviceLayers			(vector<string>& enabledLayers);
			bool						isPeerFetchAllowed		(deUint32 memoryTypeIndex, deUint32 firstdeviceID, deUint32 seconddeviceID);
	virtual	tcu::TestStatus				iterate					(void);

			Move<VkDevice>				m_deviceGroup;
			deUint32					m_physicalDeviceCount;
			VkQueue						m_deviceGroupQueue;
			vector<VkPhysicalDevice>	m_physicalDevices;

			deUint32					m_testMode;
			bool						m_useHostMemory;
			bool						m_useDedicated;
			bool						m_usePeerFetch;
			bool						m_subsetAllocation;
			bool						m_fillModeNonSolid;
			bool						m_drawTessellatedSphere;
};

DeviceGroupTestInstance::DeviceGroupTestInstance (Context& context, const deUint32 mode)
	: TestInstance				(context)
	, m_physicalDeviceCount		(0)
	, m_deviceGroupQueue		(DE_NULL)
	, m_testMode				(mode)
	, m_useHostMemory			(m_testMode & TEST_MODE_HOSTMEMORY)
	, m_useDedicated			(m_testMode & TEST_MODE_DEDICATED)
	, m_usePeerFetch			(m_testMode & TEST_MODE_PEER_FETCH)
	, m_subsetAllocation		(true)
	, m_fillModeNonSolid		(m_testMode & TEST_MODE_LINEFILL)
	, m_drawTessellatedSphere	(m_testMode & TEST_MODE_TESSELLATION)
{
	init();
}

deUint32 DeviceGroupTestInstance::getMemoryIndex (const deUint32 memoryTypeBits, const deUint32 memoryPropertyFlag)
{
	const VkPhysicalDeviceMemoryProperties deviceMemProps = getPhysicalDeviceMemoryProperties(m_context.getInstanceInterface(), m_context.getPhysicalDevice());
	for (deUint32 memoryTypeNdx = 0; memoryTypeNdx < deviceMemProps.memoryTypeCount; memoryTypeNdx++)
	{
		if ((memoryTypeBits & (1u << memoryTypeNdx)) != 0 &&
			(deviceMemProps.memoryTypes[memoryTypeNdx].propertyFlags & memoryPropertyFlag) == memoryPropertyFlag)
			return memoryTypeNdx;
	}
	TCU_THROW(NotSupportedError, "No compatible memory type found");
}

bool DeviceGroupTestInstance::isPeerFetchAllowed (deUint32 memoryTypeIndex, deUint32 firstdeviceID, deUint32 seconddeviceID)
{
	VkPeerMemoryFeatureFlags				peerMemFeatures1;
	VkPeerMemoryFeatureFlags				peerMemFeatures2;
	const DeviceDriver						vk						(m_context.getInstanceInterface(), *m_deviceGroup);
	const VkPhysicalDeviceMemoryProperties	deviceMemProps1			= getPhysicalDeviceMemoryProperties(m_context.getInstanceInterface(), m_physicalDevices[firstdeviceID]);
	const VkPhysicalDeviceMemoryProperties	deviceMemProps2			= getPhysicalDeviceMemoryProperties(m_context.getInstanceInterface(), m_physicalDevices[seconddeviceID]);
	vk.getDeviceGroupPeerMemoryFeatures(*m_deviceGroup, deviceMemProps2.memoryTypes[memoryTypeIndex].heapIndex, firstdeviceID, seconddeviceID, &peerMemFeatures1);
	vk.getDeviceGroupPeerMemoryFeatures(*m_deviceGroup, deviceMemProps1.memoryTypes[memoryTypeIndex].heapIndex, seconddeviceID, firstdeviceID, &peerMemFeatures2);
	return (peerMemFeatures1 & VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT) && (peerMemFeatures2 & VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT);
}

void DeviceGroupTestInstance::getDeviceLayers (vector<string>& enabledLayers)
{
	const tcu::CommandLine& cmdLine = m_context.getTestContext().getCommandLine();
	if (cmdLine.isValidationEnabled())
	{
		const vector<VkLayerProperties> layerProperties = enumerateDeviceLayerProperties(m_context.getInstanceInterface(), m_context.getPhysicalDevice());

		static const char*	s_magicLayer = "VK_LAYER_LUNARG_standard_validation";
		static const char*	s_defaultLayers[] =
		{
			"VK_LAYER_GOOGLE_threading",
			"VK_LAYER_LUNARG_parameter_validation",
			"VK_LAYER_LUNARG_device_limits",
			"VK_LAYER_LUNARG_object_tracker",
			"VK_LAYER_LUNARG_image",
			"VK_LAYER_LUNARG_core_validation",
			"VK_LAYER_LUNARG_swapchain",
			"VK_LAYER_GOOGLE_unique_objects",
		};

		if (isLayerSupported(layerProperties, RequiredLayer(s_magicLayer)))
			enabledLayers.push_back(s_magicLayer);
		else
		{
			for (deUint32 ndx = 0; ndx < DE_LENGTH_OF_ARRAY(s_defaultLayers); ++ndx)
			{
				if (isLayerSupported(layerProperties, RequiredLayer(s_defaultLayers[ndx])))
					enabledLayers.push_back(s_defaultLayers[ndx]);
			}
		}
		if (enabledLayers.empty())
			TCU_THROW(NotSupportedError, "No device validation layers found");
	}
}

void DeviceGroupTestInstance::init (void)
{
	if (!isInstanceExtensionSupported(m_context.getUsedApiVersion(), m_context.getInstanceExtensions(), "VK_KHR_device_group_creation"))
		TCU_THROW(NotSupportedError, "Device Group tests are not supported, no device group extension present.");

	const InstanceInterface&		instanceInterface	= m_context.getInstanceInterface();
	const deUint32					queueFamilyIndex	= m_context.getUniversalQueueFamilyIndex();
	const deUint32					queueIndex			= 0;
	const float						queuePriority		= 1.0f;
	vector<const char*>				extensionPtrs;
	de::MovePtr<vk::DeviceDriver>	deviceDriver;
	vector<const char*>				layerPtrs;
	vector<string>					deviceExtensions;
	vector<string>					enabledLayers;

	if (!isDeviceExtensionSupported(m_context.getUsedApiVersion(), m_context.getDeviceExtensions(), "VK_KHR_device_group"))
		TCU_THROW(NotSupportedError, "Missing extension: VK_KHR_device_group");

	if (!isCoreDeviceExtension(m_context.getUsedApiVersion(), "VK_KHR_device_group"))
		deviceExtensions.push_back("VK_KHR_device_group");

	if(m_useDedicated)
	{
		if (!isDeviceExtensionSupported(m_context.getUsedApiVersion(), m_context.getDeviceExtensions(), "VK_KHR_dedicated_allocation"))
			TCU_THROW(NotSupportedError, "Missing extension: VK_KHR_dedicated_allocation");

		if (!isCoreDeviceExtension(m_context.getUsedApiVersion(), "VK_KHR_dedicated_allocation"))
			deviceExtensions.push_back("VK_KHR_dedicated_allocation");
	}

	{
		const tcu::CommandLine&								cmdLine = m_context.getTestContext().getCommandLine();
		const vector<VkPhysicalDeviceGroupProperties>		properties = enumeratePhysicalDeviceGroups(instanceInterface, m_context.getInstance());
		if ((size_t)cmdLine.getVKDeviceGroupId() > properties.size())
			TCU_THROW(TestError, "Invalid device group index.");

		m_physicalDeviceCount = properties[cmdLine.getVKDeviceGroupId() - 1].physicalDeviceCount;
		for (deUint32 idx = 0; idx < m_physicalDeviceCount; idx++)
		{
			m_physicalDevices.push_back(properties[cmdLine.getVKDeviceGroupId() - 1].physicalDevices[idx]);
		}

		if (m_usePeerFetch && m_physicalDeviceCount < 2)
			TCU_THROW(NotSupportedError, "Peer fetching needs more than 1 physical device.");

		if (!(m_testMode & TEST_MODE_AFR) || (m_physicalDeviceCount > 1))
		{
			if (!de::contains(m_context.getDeviceExtensions().begin(), m_context.getDeviceExtensions().end(), std::string("VK_KHR_bind_memory2")))
				TCU_THROW(NotSupportedError, "Missing extension: VK_KHR_bind_memory2");
			deviceExtensions.push_back("VK_KHR_bind_memory2");
		}

		const VkDeviceQueueCreateInfo						deviceQueueCreateInfo =
		{
			VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,	//type
			DE_NULL,									//pNext
			(VkDeviceQueueCreateFlags)0u,				//flags
			queueFamilyIndex,							//queueFamilyIndex;
			1u,											//queueCount;
			&queuePriority,								//pQueuePriorities;
		};
		const VkDeviceGroupDeviceCreateInfo		deviceGroupInfo =
		{
			VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,					//stype
			DE_NULL,															//pNext
			properties[cmdLine.getVKDeviceGroupId() - 1].physicalDeviceCount,	//physicalDeviceCount
			properties[cmdLine.getVKDeviceGroupId() - 1].physicalDevices		//physicalDevices
		};

		VkPhysicalDevice			physicalDevice			= properties[cmdLine.getVKDeviceGroupId() - 1].physicalDevices[(size_t)(cmdLine.getVKDeviceId() - 1)];
		VkPhysicalDeviceFeatures	enabledDeviceFeatures	= getPhysicalDeviceFeatures(instanceInterface, physicalDevice);
		m_subsetAllocation									= properties[cmdLine.getVKDeviceGroupId() - 1].subsetAllocation;

		if (m_drawTessellatedSphere & static_cast<bool>(!enabledDeviceFeatures.tessellationShader))
			TCU_THROW(NotSupportedError, "Tessellation is not supported.");

		if (m_fillModeNonSolid & static_cast<bool>(!enabledDeviceFeatures.fillModeNonSolid))
			TCU_THROW(NotSupportedError, "Line polygon mode is not supported.");

		extensionPtrs.resize(deviceExtensions.size());
		for (size_t ndx = 0; ndx < deviceExtensions.size(); ++ndx)
			extensionPtrs[ndx] = deviceExtensions[ndx].c_str();

		// Get Layers
		getDeviceLayers(enabledLayers);
		layerPtrs.resize(enabledLayers.size());
		for (size_t ndx = 0; ndx < enabledLayers.size(); ++ndx)
			layerPtrs[ndx] = enabledLayers[ndx].c_str();

		const VkDeviceCreateInfo	deviceCreateInfo =
		{
			VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,					//sType;
			&deviceGroupInfo,										//pNext;
			(VkDeviceCreateFlags)0u,								//flags
			1,														//queueRecordCount;
			&deviceQueueCreateInfo,									//pRequestedQueues;
			(deUint32)layerPtrs.size(),								//layerCount;
			(layerPtrs.empty() ? DE_NULL : &layerPtrs[0]),			//ppEnabledLayerNames;
			(deUint32)extensionPtrs.size(),							//extensionCount;
			(extensionPtrs.empty() ? DE_NULL : &extensionPtrs[0]),	//ppEnabledExtensionNames;
			&enabledDeviceFeatures,									//pEnabledFeatures;
		};
		m_deviceGroup = createDevice(instanceInterface, physicalDevice, &deviceCreateInfo);
	}

	deviceDriver = de::MovePtr<vk::DeviceDriver>(new vk::DeviceDriver(instanceInterface, *m_deviceGroup));
	m_deviceGroupQueue = getDeviceQueue(*deviceDriver, *m_deviceGroup, queueFamilyIndex, queueIndex);
}

tcu::TestStatus DeviceGroupTestInstance::iterate (void)
{
	const InstanceInterface&	vki						(m_context.getInstanceInterface());
	const DeviceDriver			vk						(vki, *m_deviceGroup);
	const deUint32				queueFamilyIndex		= m_context.getUniversalQueueFamilyIndex();
	const tcu::UVec2			renderSize				(256, 256);
	const VkFormat				colorFormat				= VK_FORMAT_R8G8B8A8_UNORM;
	const tcu::Vec4				clearColor				(0.125f, 0.25f, 0.75f, 1.0f);
	const tcu::Vec4				drawColor				(1.0f, 1.0f, 0.0f, 1.0f);
	const float					tessLevel				= 16.0f;
	SimpleAllocator				memAlloc				(vk, *m_deviceGroup, getPhysicalDeviceMemoryProperties(m_context.getInstanceInterface(), m_context.getPhysicalDevice()));
	bool						iterateResultSuccess	= false;
	const tcu::Vec4				sphereVertices[]		=
	{
		tcu::Vec4(0.0f, 0.0f, 1.0f, 1.0f),
		tcu::Vec4(0.0f, 1.0f, 0.0f, 1.0f),
		tcu::Vec4(1.0f, 0.0f, 0.0f, 1.0f),
		tcu::Vec4(0.0f, 0.0f, -1.0f, 1.0f),
		tcu::Vec4(0.0f, -1.0f, 0.0f, 1.0f),
		tcu::Vec4(-1.0f, 0.0f, 0.0f, 1.0f),
	};
	const deUint32				sphereIndices[]			= {0, 1, 2, 2, 1, 3, 3, 1, 5, 5, 1, 0, 0, 2, 4, 2, 3, 4, 3, 5, 4, 5, 0, 4};
	const tcu::Vec4				triVertices[]			=
	{
		tcu::Vec4(-0.5f, -0.5f, 0.0f, 1.0f),
		tcu::Vec4(+0.5f, -0.5f, 0.0f, 1.0f),
		tcu::Vec4(0.0f, +0.5f, 0.0f, 1.0f)
	};
	const deUint32				triIndices[]			= {0, 1, 2};
	const tcu::Vec4 *			vertices				= m_drawTessellatedSphere ? &sphereVertices[0] : &triVertices[0];
	const deUint32 *			indices					= m_drawTessellatedSphere ? &sphereIndices[0] : &triIndices[0];
	const deUint32				verticesSize			= m_drawTessellatedSphere ? deUint32(sizeof(sphereVertices)) : deUint32(sizeof(triVertices));
	const deUint32				numIndices				= m_drawTessellatedSphere ? deUint32(sizeof(sphereIndices)/sizeof(sphereIndices[0])) : deUint32(sizeof(triIndices)/sizeof(triIndices[0]));
	const deUint32				indicesSize				= m_drawTessellatedSphere ? deUint32(sizeof(sphereIndices)) : deUint32(sizeof(triIndices));

	// Loop through all physical devices in the device group
	for (deUint32 physDevID = 0; physDevID < m_physicalDeviceCount; physDevID++)
	{
		const deUint32					firstDeviceID		= physDevID;
		const deUint32					secondDeviceID		= (firstDeviceID + 1 ) % m_physicalDeviceCount;
		vector<deUint32>				deviceIndices		  (m_physicalDeviceCount);

		// Set broadcast on memory allocation
		const deUint32					allocDeviceMask		= m_subsetAllocation ? (1 << firstDeviceID) | (1 << secondDeviceID) : (1 << m_physicalDeviceCount) - 1;

		for (deUint32 i = 0; i < m_physicalDeviceCount; i++)
			deviceIndices[i] = i;
		deviceIndices[firstDeviceID] = secondDeviceID;
		deviceIndices[secondDeviceID] = firstDeviceID;

		VkMemoryRequirements			memReqs				=
		{
			0,							// VkDeviceSize		size
			0,							// VkDeviceSize		alignment
			0,							// uint32_t			memoryTypeBits
		};
		deUint32						memoryTypeNdx		= 0;
		de::MovePtr<Allocation>			stagingVertexBufferMemory;
		de::MovePtr<Allocation>			stagingIndexBufferMemory;
		de::MovePtr<Allocation>			stagingUniformBufferMemory;
		de::MovePtr<Allocation>			stagingSboBufferMemory;

		vk::Move<vk::VkDeviceMemory>	vertexBufferMemory;
		vk::Move<vk::VkDeviceMemory>	indexBufferMemory;
		vk::Move<vk::VkDeviceMemory>	uniformBufferMemory;
		vk::Move<vk::VkDeviceMemory>	sboBufferMemory;
		vk::Move<vk::VkDeviceMemory>	imageMemory;

		Move<VkRenderPass>				renderPass;
		Move<VkImage>					renderImage;
		Move<VkImage>					readImage;

		Move<VkDescriptorSetLayout>		descriptorSetLayout;
		Move<VkDescriptorPool>			descriptorPool;
		Move<VkDescriptorSet>			descriptorSet;

		Move<VkBuffer>					stagingVertexBuffer;
		Move<VkBuffer>					stagingUniformBuffer;
		Move<VkBuffer>					stagingIndexBuffer;
		Move<VkBuffer>					stagingSboBuffer;

		Move<VkBuffer>					vertexBuffer;
		Move<VkBuffer>					indexBuffer;
		Move<VkBuffer>					uniformBuffer;
		Move<VkBuffer>					sboBuffer;

		Move<VkPipeline>				pipeline;
		Move<VkPipelineLayout>			pipelineLayout;

		Move<VkImageView>				colorAttView;
		Move<VkFramebuffer>				framebuffer;
		Move<VkCommandPool>				cmdPool;
		Move<VkCommandBuffer>			cmdBuffer;

		VkMemoryDedicatedAllocateInfo	dedicatedAllocInfo =
		{
				VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,		// sType
				DE_NULL,												// pNext
				DE_NULL,												// image
				DE_NULL													// buffer
		};

		VkMemoryAllocateFlagsInfo		allocDeviceMaskInfo =
		{
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,		// sType
			m_useDedicated ? &dedicatedAllocInfo : DE_NULL,		// pNext
			VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT,					// flags
			allocDeviceMask,									// deviceMask
		};

		VkMemoryAllocateInfo		allocInfo =
		{
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,			// sType
			&allocDeviceMaskInfo,							// pNext
			0u,												// allocationSize
			0u,												// memoryTypeIndex
		};

		VkDeviceGroupSubmitInfo		deviceGroupSubmitInfo =
		{
			VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO,		// sType
			DE_NULL,										// pNext
			0u,												// waitSemaphoreCount
			DE_NULL,										// pWaitSemaphoreDeviceIndices
			0u,												// commandBufferCount
			DE_NULL,										// pCommandBufferDeviceMasks
			0u,												// signalSemaphoreCount
			DE_NULL,										// pSignalSemaphoreDeviceIndices
		};

		// create vertex buffers
		{
			const VkBufferCreateInfo	stagingVertexBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				0u,																		// flags
				(VkDeviceSize)verticesSize,												// size
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,										// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
			};
			stagingVertexBuffer = createBuffer(vk, *m_deviceGroup, &stagingVertexBufferParams);
			stagingVertexBufferMemory = memAlloc.allocate(getBufferMemoryRequirements(vk, *m_deviceGroup, *stagingVertexBuffer), MemoryRequirement::HostVisible);
			VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, *stagingVertexBuffer, stagingVertexBufferMemory->getMemory(), stagingVertexBufferMemory->getOffset()));

			const VkMappedMemoryRange	range	=
			{
				VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,	// sType
				DE_NULL,								// pNext
				stagingVertexBufferMemory->getMemory(),	// memory
				0u,										// offset
				(VkDeviceSize)verticesSize,				// size
			};
			void*	vertexBufPtr	= stagingVertexBufferMemory->getHostPtr();
			deMemcpy(vertexBufPtr, &vertices[0], verticesSize);
			VK_CHECK(vk.flushMappedMemoryRanges(*m_deviceGroup, 1u, &range));
		}

		{
			const VkBufferCreateInfo	vertexBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				0u,																		// flags
				(VkDeviceSize)verticesSize,												// size
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,	// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
			};
			vertexBuffer = createBuffer(vk, *m_deviceGroup, &vertexBufferParams);

			memReqs = getBufferMemoryRequirements(vk, *m_deviceGroup, vertexBuffer.get());
			memoryTypeNdx = getMemoryIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

			dedicatedAllocInfo.buffer = vertexBuffer.get();
			allocInfo.allocationSize = memReqs.size;
			allocInfo.memoryTypeIndex = memoryTypeNdx;
			vertexBufferMemory = allocateMemory(vk, *m_deviceGroup, &allocInfo);

			if (m_usePeerFetch && !isPeerFetchAllowed(memoryTypeNdx, firstDeviceID, secondDeviceID))
				TCU_THROW(NotSupportedError, "Peer fetch is not supported.");

			// Bind vertex buffer
			if (m_usePeerFetch)
			{
				VkBindBufferMemoryDeviceGroupInfo	devGroupBindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO,		// sType
					DE_NULL,													// pNext
					m_physicalDeviceCount,										// deviceIndexCount
					&deviceIndices[0],											// pDeviceIndices
				};

				VkBindBufferMemoryInfo				bindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,					// sType
					&devGroupBindInfo,											// pNext
					vertexBuffer.get(),											// buffer
					vertexBufferMemory.get(),									// memory
					0u,															// memoryOffset
				};
				VK_CHECK(vk.bindBufferMemory2(*m_deviceGroup, 1, &bindInfo));
			}
			else
				VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, *vertexBuffer, vertexBufferMemory.get(), 0));
		}

		// create index buffers
		{
			const VkBufferCreateInfo	stagingIndexBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				0u,																		// flags
				(VkDeviceSize)indicesSize,												// size
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,										// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
			};
			stagingIndexBuffer = createBuffer(vk, *m_deviceGroup, &stagingIndexBufferParams);
			stagingIndexBufferMemory = memAlloc.allocate(getBufferMemoryRequirements(vk, *m_deviceGroup, *stagingIndexBuffer), MemoryRequirement::HostVisible);
			VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, *stagingIndexBuffer, stagingIndexBufferMemory->getMemory(), stagingIndexBufferMemory->getOffset()));

			const VkMappedMemoryRange	range	=
			{
				VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,	// sType
				DE_NULL,								// pNext
				stagingIndexBufferMemory->getMemory(),	// memory
				0u,										// offset
				(VkDeviceSize)indicesSize,				// size
			};
			void*	indexBufPtr	= stagingIndexBufferMemory->getHostPtr();
			deMemcpy(indexBufPtr, &indices[0], indicesSize);
			VK_CHECK(vk.flushMappedMemoryRanges(*m_deviceGroup, 1u, &range));
		}

		{
			const VkBufferCreateInfo	indexBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				0u,																		// flags
				(VkDeviceSize)indicesSize,												// size
				VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,	// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
			};
			indexBuffer = createBuffer(vk, *m_deviceGroup, &indexBufferParams);

			memReqs = getBufferMemoryRequirements(vk, *m_deviceGroup, indexBuffer.get());
			memoryTypeNdx = getMemoryIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

			dedicatedAllocInfo.buffer = indexBuffer.get();
			allocInfo.allocationSize = memReqs.size;
			allocInfo.memoryTypeIndex = memoryTypeNdx;
			indexBufferMemory = allocateMemory(vk, *m_deviceGroup, &allocInfo);

			if (m_usePeerFetch && !isPeerFetchAllowed(memoryTypeNdx, firstDeviceID, secondDeviceID))
				TCU_THROW(NotSupportedError, "Peer fetch is not supported.");

			// Bind index buffer
			if (m_usePeerFetch)
			{
				VkBindBufferMemoryDeviceGroupInfo	devGroupBindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO,		// sType
					DE_NULL,													// pNext
					m_physicalDeviceCount,										// deviceIndexCount
					&deviceIndices[0],											// pDeviceIndices
				};

				VkBindBufferMemoryInfo				bindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,					// sType
					&devGroupBindInfo,											// pNext
					indexBuffer.get(),											// buffer
					indexBufferMemory.get(),									// memory
					0u,															// memoryOffset
				};
				VK_CHECK(vk.bindBufferMemory2(*m_deviceGroup, 1, &bindInfo));
			}
			else
				VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, *indexBuffer, indexBufferMemory.get(), 0));
		}

		// create uniform buffers
		{
			const VkBufferCreateInfo	stagingUniformBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				0u,																		// flags
				(VkDeviceSize)sizeof(drawColor),												// size
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,										// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
			};
			stagingUniformBuffer = createBuffer(vk, *m_deviceGroup, &stagingUniformBufferParams);
			stagingUniformBufferMemory = memAlloc.allocate(getBufferMemoryRequirements(vk, *m_deviceGroup, *stagingUniformBuffer), MemoryRequirement::HostVisible);
			VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, *stagingUniformBuffer, stagingUniformBufferMemory->getMemory(), stagingUniformBufferMemory->getOffset()));

			const VkMappedMemoryRange	range	=
			{
				VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,	// sType
				DE_NULL,								// pNext
				stagingUniformBufferMemory->getMemory(),// memory
				0u,										// offset
				(VkDeviceSize)sizeof(drawColor),		// size
			};
			void*	uniformBufPtr	= stagingUniformBufferMemory->getHostPtr();
			deMemcpy(uniformBufPtr, &drawColor[0], sizeof(drawColor));
			VK_CHECK(vk.flushMappedMemoryRanges(*m_deviceGroup, 1u, &range));
		}

		{
			const VkBufferCreateInfo	uniformBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				0u,																		// flags
				(VkDeviceSize)sizeof(drawColor),										// size
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,	// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
			};
			uniformBuffer = createBuffer(vk, *m_deviceGroup, &uniformBufferParams);

			memReqs = getBufferMemoryRequirements(vk, *m_deviceGroup, uniformBuffer.get());
			memoryTypeNdx = getMemoryIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

			dedicatedAllocInfo.buffer = uniformBuffer.get();
			allocInfo.allocationSize = memReqs.size;
			allocInfo.memoryTypeIndex = memoryTypeNdx;
			uniformBufferMemory = allocateMemory(vk, *m_deviceGroup, &allocInfo);

			if (m_usePeerFetch && !isPeerFetchAllowed(memoryTypeNdx, firstDeviceID, secondDeviceID))
				TCU_THROW(NotSupportedError, "Peer fetch is not supported.");

			if (m_usePeerFetch)
			{
				VkBindBufferMemoryDeviceGroupInfo	devGroupBindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO,		// sType
					DE_NULL,													// pNext
					m_physicalDeviceCount,										// deviceIndexCount
					&deviceIndices[0],											// pDeviceIndices
				};

				VkBindBufferMemoryInfo				bindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,					// sType
					&devGroupBindInfo,											// pNext
					uniformBuffer.get(),										// buffer
					uniformBufferMemory.get(),									// memory
					0u,															// memoryOffset
				};
				VK_CHECK(vk.bindBufferMemory2(*m_deviceGroup, 1, &bindInfo));
			}
			else
				VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, uniformBuffer.get(), uniformBufferMemory.get(), 0));
		}

		// create SBO buffers
		{
			const VkBufferCreateInfo	stagingSboBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				0u,																		// flags
				(VkDeviceSize)sizeof(tessLevel),										// size
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,										// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
			};
			stagingSboBuffer = createBuffer(vk, *m_deviceGroup, &stagingSboBufferParams);
			stagingSboBufferMemory = memAlloc.allocate(getBufferMemoryRequirements(vk, *m_deviceGroup, *stagingSboBuffer), MemoryRequirement::HostVisible);
			VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, *stagingSboBuffer, stagingSboBufferMemory->getMemory(), stagingSboBufferMemory->getOffset()));

			const VkMappedMemoryRange	range	=
			{
				VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,	// sType
				DE_NULL,								// pNext
				stagingSboBufferMemory->getMemory(),	// memory
				0u,										// offset
				(VkDeviceSize)sizeof(tessLevel),		// size
			};
			void*	sboBufPtr	= stagingSboBufferMemory->getHostPtr();
			deMemcpy(sboBufPtr, &tessLevel, sizeof(tessLevel));
			VK_CHECK(vk.flushMappedMemoryRanges(*m_deviceGroup, 1u, &range));
		}

		{
			const VkBufferCreateInfo	sboBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				0u,																		// flags
				(VkDeviceSize)sizeof(tessLevel),										// size
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,	// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
			};
			sboBuffer = createBuffer(vk, *m_deviceGroup, &sboBufferParams);

			memReqs = getBufferMemoryRequirements(vk, *m_deviceGroup, sboBuffer.get());
			memoryTypeNdx = getMemoryIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

			dedicatedAllocInfo.buffer = sboBuffer.get();
			allocInfo.allocationSize = memReqs.size;
			allocInfo.memoryTypeIndex = memoryTypeNdx;
			sboBufferMemory = allocateMemory(vk, *m_deviceGroup, &allocInfo);

			if (m_usePeerFetch && !isPeerFetchAllowed(memoryTypeNdx, firstDeviceID, secondDeviceID))
				TCU_THROW(NotSupportedError, "Peer fetch is not supported.");

			if (m_usePeerFetch)
			{
				VkBindBufferMemoryDeviceGroupInfo	devGroupBindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO,		// sType
					DE_NULL,													// pNext
					m_physicalDeviceCount,										// deviceIndexCount
					&deviceIndices[0],											// pDeviceIndices
				};

				VkBindBufferMemoryInfo				bindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,					// sType
					&devGroupBindInfo,											// pNext
					sboBuffer.get(),											// buffer
					sboBufferMemory.get(),										// memory
					0u,															// memoryOffset
				};
				VK_CHECK(vk.bindBufferMemory2(*m_deviceGroup, 1, &bindInfo));
			}
			else
				VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, sboBuffer.get(), sboBufferMemory.get(), 0));
		}

		// Create image resources
		// Use a consistent usage flag because of memory aliasing
		VkImageUsageFlags imageUsageFlag = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		{
			// Check for SFR support
			VkImageFormatProperties properties;
			if ((m_testMode & TEST_MODE_SFR) && vki.getPhysicalDeviceImageFormatProperties(m_context.getPhysicalDevice(),
				colorFormat,															// format
				VK_IMAGE_TYPE_2D,														// type
				VK_IMAGE_TILING_OPTIMAL,												// tiling
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,	// usage
				VK_IMAGE_CREATE_BIND_SFR_BIT,											// flags
				&properties) != VK_SUCCESS)												// properties
			{
				TCU_THROW(NotSupportedError, "Format not supported for SFR");
			}

			VkImageCreateFlags	imageCreateFlags = VK_IMAGE_CREATE_ALIAS_BIT;	// The image objects alias same memory
			if (m_testMode & TEST_MODE_SFR)
			{
				imageCreateFlags |= VK_IMAGE_CREATE_BIND_SFR_BIT;
			}

			const VkImageCreateInfo		imageParams =
			{
				VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,									// sType
				DE_NULL,																// pNext
				imageCreateFlags,														// flags
				VK_IMAGE_TYPE_2D,														// imageType
				colorFormat,															// format
				{ renderSize.x(), renderSize.y(), 1 },									// extent
				1u,																		// mipLevels
				1u,																		// arraySize
				VK_SAMPLE_COUNT_1_BIT,													// samples
				VK_IMAGE_TILING_OPTIMAL,												// tiling
				imageUsageFlag,															// usage
				VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
				1u,																		// queueFamilyIndexCount
				&queueFamilyIndex,														// pQueueFamilyIndices
				VK_IMAGE_LAYOUT_UNDEFINED,												// initialLayout
			};

			renderImage = createImage(vk, *m_deviceGroup, &imageParams);
			readImage = createImage(vk, *m_deviceGroup, &imageParams);

			dedicatedAllocInfo.image = *renderImage;
			dedicatedAllocInfo.buffer = DE_NULL;
			memReqs = getImageMemoryRequirements(vk, *m_deviceGroup, renderImage.get());
			memoryTypeNdx = getMemoryIndex(memReqs.memoryTypeBits, m_useHostMemory ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			allocInfo.allocationSize = memReqs.size;
			allocInfo.memoryTypeIndex = memoryTypeNdx;
			imageMemory = allocateMemory(vk, *m_deviceGroup, &allocInfo);
		}

		if (m_testMode & TEST_MODE_SFR)
		{
			if (m_usePeerFetch && !isPeerFetchAllowed(memoryTypeNdx, firstDeviceID, secondDeviceID))
				TCU_THROW(NotSupportedError, "Peer texture reads is not supported.");

			VkRect2D zeroRect = {
				{
					0,	//	VkOffset2D.x
					0,	//	VkOffset2D.x
				},
				{
					0,	//	VkExtent2D.x
					0,	//	VkExtent2D.x
				}
			};
			vector<VkRect2D> sfrRects;
			for (deUint32 i = 0; i < m_physicalDeviceCount*m_physicalDeviceCount; i++)
				sfrRects.push_back(zeroRect);

			if (m_physicalDeviceCount == 1u)
			{
				sfrRects[0].extent.width	= (deInt32)renderSize.x();
				sfrRects[0].extent.height	= (deInt32)renderSize.y();
			}
			else
			{
				// Split into 2 vertical halves
				sfrRects[firstDeviceID * m_physicalDeviceCount + firstDeviceID].extent.width	= (deInt32)renderSize.x() / 2;
				sfrRects[firstDeviceID * m_physicalDeviceCount + firstDeviceID].extent.height	= (deInt32)renderSize.y();
				sfrRects[firstDeviceID * m_physicalDeviceCount + secondDeviceID]				= sfrRects[firstDeviceID * m_physicalDeviceCount + firstDeviceID];
				sfrRects[firstDeviceID * m_physicalDeviceCount + secondDeviceID].offset.x		= (deInt32)renderSize.x() / 2;
				sfrRects[secondDeviceID * m_physicalDeviceCount + firstDeviceID]				= sfrRects[firstDeviceID * m_physicalDeviceCount + firstDeviceID];
				sfrRects[secondDeviceID * m_physicalDeviceCount + secondDeviceID]				= sfrRects[firstDeviceID * m_physicalDeviceCount + secondDeviceID];
			}

			VkBindImageMemoryDeviceGroupInfo	devGroupBindInfo =
			{
				VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO,		// sType
				DE_NULL,													// pNext
				0u,															// deviceIndexCount
				DE_NULL,													// pDeviceIndices
				m_physicalDeviceCount*m_physicalDeviceCount,				// SFRRectCount
				&sfrRects[0],												// pSFRRects
			};

			VkBindImageMemoryInfo				bindInfo =
			{
				VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,					// sType
				&devGroupBindInfo,											// pNext
				*renderImage,												// image
				imageMemory.get(),											// memory
				0u,															// memoryOffset
			};
			VK_CHECK(vk.bindImageMemory2(*m_deviceGroup, 1, &bindInfo));
		}
		else
			VK_CHECK(vk.bindImageMemory(*m_deviceGroup, *renderImage, imageMemory.get(), 0));

		VK_CHECK(vk.bindImageMemory(*m_deviceGroup, *readImage, imageMemory.get(), 0));

		// Create renderpass
		{
			const VkAttachmentDescription			colorAttDesc =
			{
				0u,												// flags
				colorFormat,									// format
				VK_SAMPLE_COUNT_1_BIT,							// samples
				VK_ATTACHMENT_LOAD_OP_CLEAR,					// loadOp
				VK_ATTACHMENT_STORE_OP_STORE,					// storeOp
				VK_ATTACHMENT_LOAD_OP_DONT_CARE,				// stencilLoadOp
				VK_ATTACHMENT_STORE_OP_DONT_CARE,				// stencilStoreOp
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,		// initialLayout
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,		// finalLayout
			};
			const VkAttachmentReference				colorAttRef =
			{
				0u,												// attachment
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,		// layout
			};
			const VkSubpassDescription				subpassDesc =
			{
				(VkSubpassDescriptionFlags)0u,					// flags
				VK_PIPELINE_BIND_POINT_GRAPHICS,				// pipelineBindPoint
				0u,												// inputAttachmentCount
				DE_NULL,										// pInputAttachments
				1u,												// colorAttachmentCount
				&colorAttRef,									// pColorAttachments
				DE_NULL,										// pResolveAttachments
				DE_NULL,										// depthStencilAttachment
				0u,												// preserveAttachmentCount
				DE_NULL,										// pPreserveAttachments
			};
			const VkRenderPassCreateInfo			renderPassParams =
			{
				VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,		// sType
				DE_NULL,										// pNext
				0u,												// flags
				1u,												// attachmentCount
				&colorAttDesc,									// pAttachments
				1u,												// subpassCount
				&subpassDesc,									// pSubpasses
				0u,												// dependencyCount
				DE_NULL,										// pDependencies
			};
			renderPass = createRenderPass(vk, *m_deviceGroup, &renderPassParams);
		}

		// Create descriptors
		{
			vector<VkDescriptorSetLayoutBinding>	layoutBindings;
			vector<VkDescriptorPoolSize>			descriptorTypes;
			vector<VkWriteDescriptorSet>			writeDescritporSets;

			const VkDescriptorSetLayoutBinding layoutBindingUBO =
			{
				0u,											// deUint32				binding;
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,			// VkDescriptorType		descriptorType;
				1u,											// deUint32				descriptorCount;
				VK_SHADER_STAGE_FRAGMENT_BIT,				// VkShaderStageFlags	stageFlags;
				DE_NULL										// const VkSampler*		pImmutableSamplers;
			};
			const VkDescriptorSetLayoutBinding layoutBindingSBO =
			{
				1u,											// deUint32				binding;
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,			// VkDescriptorType		descriptorType;
				1u,											// deUint32				descriptorCount;
				VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,	// VkShaderStageFlags	stageFlags;
				DE_NULL										// const VkSampler*		pImmutableSamplers;
			};

			layoutBindings.push_back(layoutBindingUBO);
			if (m_drawTessellatedSphere)
				layoutBindings.push_back(layoutBindingSBO);

			const VkDescriptorSetLayoutCreateInfo	descriptorLayoutParams =
			{
				VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,	// VkStructureType						sType;
				DE_NULL,												// cost void*							pNext;
				(VkDescriptorSetLayoutCreateFlags)0,					// VkDescriptorSetLayoutCreateFlags		flags
				deUint32(layoutBindings.size()),						// deUint32								count;
				layoutBindings.data()									// const VkDescriptorSetLayoutBinding	pBinding;
			};
			descriptorSetLayout = createDescriptorSetLayout(vk, *m_deviceGroup, &descriptorLayoutParams);

			const VkDescriptorPoolSize descriptorTypeUBO =
			{
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,		// VkDescriptorType		type;
				1										// deUint32				count;
			};
			const VkDescriptorPoolSize descriptorTypeSBO =
			{
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,		// VkDescriptorType		type;
				1										// deUint32				count;
			};
			descriptorTypes.push_back(descriptorTypeUBO);
			if (m_drawTessellatedSphere)
				descriptorTypes.push_back(descriptorTypeSBO);

			const VkDescriptorPoolCreateInfo descriptorPoolParams =
			{
				VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,		// VkStructureType					sType;
				DE_NULL,											// void*							pNext;
				VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,	// VkDescriptorPoolCreateFlags		flags;
				1u,													// deUint32							maxSets;
				deUint32(descriptorTypes.size()),					// deUint32							count;
				descriptorTypes.data()								// const VkDescriptorTypeCount*		pTypeCount
			};
			descriptorPool = createDescriptorPool(vk, *m_deviceGroup, &descriptorPoolParams);

			const VkDescriptorSetAllocateInfo descriptorSetParams =
			{
				VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				DE_NULL,
				*descriptorPool,
				1u,
				&descriptorSetLayout.get(),
			};
			descriptorSet = allocateDescriptorSet(vk, *m_deviceGroup, &descriptorSetParams);

			const VkDescriptorBufferInfo uboDescriptorInfo =
			{
				uniformBuffer.get(),
				0,
				(VkDeviceSize)sizeof(drawColor)
			};
			const VkDescriptorBufferInfo sboDescriptorInfo =
			{
				sboBuffer.get(),
				0,
				(VkDeviceSize)sizeof(tessLevel)
			};
			const VkWriteDescriptorSet writeDescritporSetUBO =
			{
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,		// VkStructureType			sType;
				DE_NULL,									// const void*				pNext;
				*descriptorSet,								// VkDescriptorSet			destSet;
				0,											// deUint32					destBinding;
				0,											// deUint32					destArrayElement;
				1u,											// deUint32					count;
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,			// VkDescriptorType			descriptorType;
				(const VkDescriptorImageInfo*)DE_NULL,		// VkDescriptorImageInfo*	pImageInfo;
				&uboDescriptorInfo,							// VkDescriptorBufferInfo*	pBufferInfo;
				(const VkBufferView*)DE_NULL				// VkBufferView*			pTexelBufferView;
			};

			const VkWriteDescriptorSet writeDescritporSetSBO =
			{
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,		// VkStructureType			sType;
				DE_NULL,									// const void*				pNext;
				*descriptorSet,								// VkDescriptorSet			destSet;
				1,											// deUint32					destBinding;
				0,											// deUint32					destArrayElement;
				1u,											// deUint32					count;
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,			// VkDescriptorType			descriptorType;
				(const VkDescriptorImageInfo*)DE_NULL,		// VkDescriptorImageInfo*	pImageInfo;
				&sboDescriptorInfo,							// VkDescriptorBufferInfo*	pBufferInfo;
				(const VkBufferView*)DE_NULL				// VkBufferView*			pTexelBufferView;
			};
			writeDescritporSets.push_back(writeDescritporSetUBO);
			if (m_drawTessellatedSphere)
				writeDescritporSets.push_back(writeDescritporSetSBO);

			vk.updateDescriptorSets(*m_deviceGroup, deUint32(writeDescritporSets.size()), writeDescritporSets.data(), 0u, DE_NULL);
		}

		// Create Pipeline
		{
			vector<VkPipelineShaderStageCreateInfo>	shaderStageParams;
			Move<VkShaderModule>					vertShaderModule;
			Move<VkShaderModule>					tcssShaderModule;
			Move<VkShaderModule>					tessShaderModule;
			Move<VkShaderModule>					fragShaderModule;

			const VkDescriptorSetLayout descset = descriptorSetLayout.get();
			const VkPipelineLayoutCreateInfo	pipelineLayoutParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,			// sType
				DE_NULL,												// pNext
				(vk::VkPipelineLayoutCreateFlags)0,						// flags
				1u,														// setLayoutCount
				&descset,												// pSetLayouts
				0u,														// pushConstantRangeCount
				DE_NULL,												// pPushConstantRanges
			};
			pipelineLayout = createPipelineLayout(vk, *m_deviceGroup, &pipelineLayoutParams);

			// Shaders
			vertShaderModule = createShaderModule(vk, *m_deviceGroup, m_context.getBinaryCollection().get("vert"), 0);
			fragShaderModule = createShaderModule(vk, *m_deviceGroup, m_context.getBinaryCollection().get("frag"), 0);

			const VkSpecializationInfo				emptyShaderSpecParams =
			{
				0u,															// mapEntryCount
				DE_NULL,													// pMap
				0,															// dataSize
				DE_NULL,													// pData
			};
			const VkPipelineShaderStageCreateInfo	vertexShaderStageParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,	// sType
				DE_NULL,												// pNext
				0u,														// flags
				VK_SHADER_STAGE_VERTEX_BIT,								// stage
				*vertShaderModule,										// module
				"main",													// pName
				&emptyShaderSpecParams,									// pSpecializationInfo
			};
			shaderStageParams.push_back(vertexShaderStageParams);

			if (m_drawTessellatedSphere)
			{
				tcssShaderModule = createShaderModule(vk, *m_deviceGroup, m_context.getBinaryCollection().get("tesc"), 0);
				tessShaderModule = createShaderModule(vk, *m_deviceGroup, m_context.getBinaryCollection().get("tese"), 0);

				const VkPipelineShaderStageCreateInfo	tessControlShaderStageParams =
				{
					VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,	// sType
					DE_NULL,												// pNext
					0u,														// flags
					VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,				// stage
					*tcssShaderModule,										// module
					"main",													// pName
					&emptyShaderSpecParams,									// pSpecializationInfo
				};
				const VkPipelineShaderStageCreateInfo	tessEvalShaderStageParams =
				{
					VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,	// sType
					DE_NULL,												// pNext
					0u,														// flags
					VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,			// stage
					*tessShaderModule,										// module
					"main",													// pName
					&emptyShaderSpecParams,									// pSpecializationInfo
				};

				shaderStageParams.push_back(tessControlShaderStageParams);
				shaderStageParams.push_back(tessEvalShaderStageParams);
			}

			const VkPipelineShaderStageCreateInfo	fragmentShaderStageParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,	// sType
				DE_NULL,												// pNext
				0u,														// flags
				VK_SHADER_STAGE_FRAGMENT_BIT,							// stage
				*fragShaderModule,										// module
				"main",													// pName
				&emptyShaderSpecParams,									// pSpecializationInfo
			};
			shaderStageParams.push_back(fragmentShaderStageParams);

			const VkPipelineDepthStencilStateCreateInfo	depthStencilParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,	// sType
				DE_NULL,													// pNext
				0u,															// flags
				DE_FALSE,													// depthTestEnable
				DE_FALSE,													// depthWriteEnable
				VK_COMPARE_OP_ALWAYS,										// depthCompareOp
				DE_FALSE,													// depthBoundsTestEnable
				DE_FALSE,													// stencilTestEnable
				{
					VK_STENCIL_OP_KEEP,										// failOp
					VK_STENCIL_OP_KEEP,										// passOp
					VK_STENCIL_OP_KEEP,										// depthFailOp
					VK_COMPARE_OP_ALWAYS,									// compareOp
					0u,														// compareMask
					0u,														// writeMask
					0u,														// reference
				},															// front
				{
					VK_STENCIL_OP_KEEP,										// failOp
					VK_STENCIL_OP_KEEP,										// passOp
					VK_STENCIL_OP_KEEP,										// depthFailOp
					VK_COMPARE_OP_ALWAYS,									// compareOp
					0u,														// compareMask
					0u,														// writeMask
					0u,														// reference
				},															// back;
				0.0f,														// minDepthBounds;
				1.0f,														// maxDepthBounds;
			};
			const VkViewport	viewport0 =
			{
				0.0f,														// x
				0.0f,														// y
				(float)renderSize.x(),										// width
				(float)renderSize.y(),										// height
				0.0f,														// minDepth
				1.0f,														// maxDepth
			};
			const VkRect2D		scissor0 =
			{
				{
					0u,														// x
					0u,														// y
				},															// offset
				{
					renderSize.x(),											// width
					renderSize.y(),											// height
				},															// extent;
			};
			const VkPipelineViewportStateCreateInfo		viewportParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,		// sType
				DE_NULL,													// pNext
				0u,															// flags
				1u,															// viewportCount
				&viewport0,													// pViewports
				1u,															// scissorCount
				&scissor0													// pScissors
			};
			const VkSampleMask							sampleMask = ~0u;
			const VkPipelineMultisampleStateCreateInfo	multisampleParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,	// sType
				DE_NULL,													// pNext
				0u,															// flags
				VK_SAMPLE_COUNT_1_BIT,										// rasterizationSamples
				VK_FALSE,													// sampleShadingEnable
				0.0f,														// minSampleShading
				&sampleMask,												// sampleMask
				VK_FALSE,													// alphaToCoverageEnable
				VK_FALSE,													// alphaToOneEnable
			};
			const VkPipelineRasterizationStateCreateInfo	rasterParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,			// sType
				DE_NULL,															// pNext
				0u,																	// flags
				VK_TRUE,															// depthClampEnable
				VK_FALSE,															// rasterizerDiscardEnable
				m_fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,	// polygonMode
				VK_CULL_MODE_NONE,													// cullMode
				VK_FRONT_FACE_COUNTER_CLOCKWISE,									// frontFace
				VK_FALSE,															// depthBiasEnable
				0.0f,																// depthBiasConstantFactor
				0.0f,																// depthBiasClamp
				0.0f,																// depthBiasSlopeFactor
				1.0f,																// lineWidth
			};
			const VkPipelineInputAssemblyStateCreateInfo	inputAssemblyParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,	// sType
				DE_NULL,														// pNext
				0u,																// flags
				m_drawTessellatedSphere ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,	// topology
				DE_FALSE,														// primitiveRestartEnable
			};
			const VkVertexInputBindingDescription		vertexBinding0 =
			{
				0u,														// binding
				(deUint32)sizeof(tcu::Vec4),							// stride
				VK_VERTEX_INPUT_RATE_VERTEX,							// inputRate
			};
			const VkVertexInputAttributeDescription		vertexAttrib0 =
			{
				0u,														// location
				0u,														// binding
				VK_FORMAT_R32G32B32A32_SFLOAT,							// format
				0u,														// offset
			};
			const VkPipelineVertexInputStateCreateInfo	vertexInputStateParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,	// sType
				DE_NULL,													// pNext
				0u,															// flags
				1u,															// vertexBindingDescriptionCount
				&vertexBinding0,											// pVertexBindingDescriptions
				1u,															// vertexAttributeDescriptionCount
				&vertexAttrib0,												// pVertexAttributeDescriptions
			};
			const VkPipelineColorBlendAttachmentState	attBlendParams =
			{
				VK_FALSE,													// blendEnable
				VK_BLEND_FACTOR_ONE,										// srcColorBlendFactor
				VK_BLEND_FACTOR_ZERO,										// dstColorBlendFactor
				VK_BLEND_OP_ADD,											// colorBlendOp
				VK_BLEND_FACTOR_ONE,										// srcAlphaBlendFactor
				VK_BLEND_FACTOR_ZERO,										// dstAlphaBlendFactor
				VK_BLEND_OP_ADD,											// alphaBlendOp
				(VK_COLOR_COMPONENT_R_BIT |
				 VK_COLOR_COMPONENT_G_BIT |
				 VK_COLOR_COMPONENT_B_BIT |
				 VK_COLOR_COMPONENT_A_BIT),									// colorWriteMask
			};
			const VkPipelineColorBlendStateCreateInfo	blendParams =
			{
				VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,	// sType
				DE_NULL,													// pNext
				0u,															// flags
				DE_FALSE,													// logicOpEnable
				VK_LOGIC_OP_COPY,											// logicOp
				1u,															// attachmentCount
				&attBlendParams,											// pAttachments
				{ 0.0f, 0.0f, 0.0f, 0.0f },									// blendConstants[4]
			};

			const VkPipelineTessellationStateCreateInfo tessState =
			{
				VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,	// sType
				DE_NULL,													// pNext
				0u,															// flags
				3u,															// patchControlPoints
			};
			const VkGraphicsPipelineCreateInfo		pipelineParams =
			{
				VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,		// sType
				DE_NULL,												// pNext
				0u,														// flags
				deUint32(shaderStageParams.size()),						// stageCount
				shaderStageParams.data(),								// pStages
				&vertexInputStateParams,								// pVertexInputState
				&inputAssemblyParams,									// pInputAssemblyState
				m_drawTessellatedSphere ? &tessState : DE_NULL,			// pTessellationState
				&viewportParams,										// pViewportState
				&rasterParams,											// pRasterizationState
				&multisampleParams,										// pMultisampleState
				&depthStencilParams,									// pDepthStencilState
				&blendParams,											// pColorBlendState
				(const VkPipelineDynamicStateCreateInfo*)DE_NULL,		// pDynamicState
				*pipelineLayout,										// layout
				*renderPass,											// renderPass
				0u,														// subpass
				DE_NULL,												// basePipelineHandle
				0u,														// basePipelineIndex
			};
			pipeline = createGraphicsPipeline(vk, *m_deviceGroup, DE_NULL, &pipelineParams);
		}

		// Create Framebuffer
		{
			const VkImageViewCreateInfo				colorAttViewParams =
			{
				VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,		// sType
				DE_NULL,										// pNext
				0u,												// flags
				*renderImage,									// image
				VK_IMAGE_VIEW_TYPE_2D,							// viewType
				colorFormat,									// format
				{
					VK_COMPONENT_SWIZZLE_R,
					VK_COMPONENT_SWIZZLE_G,
					VK_COMPONENT_SWIZZLE_B,
					VK_COMPONENT_SWIZZLE_A
				},												// components
				{
					VK_IMAGE_ASPECT_COLOR_BIT,					// aspectMask
					0u,											// baseMipLevel
					1u,											// levelCount
					0u,											// baseArrayLayer
					1u,											// layerCount
				},												// subresourceRange
			};
			colorAttView = createImageView(vk, *m_deviceGroup, &colorAttViewParams);

			const VkFramebufferCreateInfo			framebufferParams =
			{
				VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,				// sType
				DE_NULL,												// pNext
				0u,														// flags
				*renderPass,											// renderPass
				1u,														// attachmentCount
				&*colorAttView,											// pAttachments
				renderSize.x(),											// width
				renderSize.y(),											// height
				1u,														// layers
			};
			framebuffer = createFramebuffer(vk, *m_deviceGroup, &framebufferParams);
		}

		// Create Command buffer
		{
			const VkCommandPoolCreateInfo			cmdPoolParams =
			{
				VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,					// sType
				DE_NULL,													// pNext
				VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,			// flags
				queueFamilyIndex,											// queueFamilyIndex
			};
			cmdPool = createCommandPool(vk, *m_deviceGroup, &cmdPoolParams);

			const VkCommandBufferAllocateInfo		cmdBufParams =
			{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,			// sType
				DE_NULL,												// pNext
				*cmdPool,												// pool
				VK_COMMAND_BUFFER_LEVEL_PRIMARY,						// level
				1u,														// bufferCount
			};
			cmdBuffer = allocateCommandBuffer(vk, *m_deviceGroup, &cmdBufParams);
		}

		// Begin recording
		VkCommandBufferBeginInfo				cmdBufBeginParams =
		{
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,			// sType
			DE_NULL,												// pNext
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,			// flags
			(const VkCommandBufferInheritanceInfo*)DE_NULL,
		};
		VK_CHECK(vk.beginCommandBuffer(*cmdBuffer, &cmdBufBeginParams));

		// Prepare render target for rendering
		{
			const VkMemoryBarrier		vertFlushBarrier =
			{
				VK_STRUCTURE_TYPE_MEMORY_BARRIER,			// sType
				DE_NULL,									// pNext
				VK_ACCESS_HOST_WRITE_BIT,					// srcAccessMask
				VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,		// dstAccessMask
			};
			const VkImageMemoryBarrier	colorAttBarrier =
			{
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,		// sType
				DE_NULL,									// pNext
				0u,											// srcAccessMask
				(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),		// dstAccessMask
				VK_IMAGE_LAYOUT_UNDEFINED,					// oldLayout
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,	// newLayout
				queueFamilyIndex,							// srcQueueFamilyIndex
				queueFamilyIndex,							// dstQueueFamilyIndex
				*renderImage,								// image
				{
					VK_IMAGE_ASPECT_COLOR_BIT,				// aspectMask
					0u,										// baseMipLevel
					1u,										// levelCount
					0u,										// baseArrayLayer
					1u,										// layerCount
				}											// subresourceRange
			};
			vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, (VkDependencyFlags)0, 1, &vertFlushBarrier, 0, (const VkBufferMemoryBarrier*)DE_NULL, 1, &colorAttBarrier);
		}

		// Update buffers
		{
			const VkBufferMemoryBarrier		stagingVertexBufferUpdateBarrier =
			{
				VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// VkStructureType	sType;
				DE_NULL,									// const void*		pNext;
				VK_ACCESS_HOST_WRITE_BIT,					// VkAccessFlags	srcAccessMask;
				VK_ACCESS_TRANSFER_READ_BIT,				// VkAccessFlags	dstAccessMask;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			srcQueueFamilyIndex;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			dstQueueFamilyIndex;
				stagingVertexBuffer.get(),					// VkBuffer			buffer;
				0u,											// VkDeviceSize		offset;
				verticesSize								// VkDeviceSize		size;
			};

			const VkBufferMemoryBarrier		vertexBufferUpdateBarrier =
			{
				VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// VkStructureType	sType;
				DE_NULL,									// const void*		pNext;
				VK_ACCESS_TRANSFER_WRITE_BIT,				// VkAccessFlags	srcAccessMask;
				VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,		// VkAccessFlags	dstAccessMask;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			srcQueueFamilyIndex;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			dstQueueFamilyIndex;
				vertexBuffer.get(),							// VkBuffer			buffer;
				0u,											// VkDeviceSize		offset;
				verticesSize								// VkDeviceSize		size;
			};

			const VkBufferMemoryBarrier		stagingIndexBufferUpdateBarrier =
			{
				VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// VkStructureType	sType;
				DE_NULL,									// const void*		pNext;
				VK_ACCESS_HOST_WRITE_BIT,					// VkAccessFlags	srcAccessMask;
				VK_ACCESS_TRANSFER_READ_BIT,				// VkAccessFlags	dstAccessMask;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			srcQueueFamilyIndex;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			dstQueueFamilyIndex;
				stagingIndexBuffer.get(),					// VkBuffer			buffer;
				0u,											// VkDeviceSize		offset;
				indicesSize									// VkDeviceSize		size;
			};

			const VkBufferMemoryBarrier		indexBufferUpdateBarrier =
			{
				VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// VkStructureType	sType;
				DE_NULL,									// const void*		pNext;
				VK_ACCESS_TRANSFER_WRITE_BIT,				// VkAccessFlags	srcAccessMask;
				VK_ACCESS_INDEX_READ_BIT,					// VkAccessFlags	dstAccessMask;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			srcQueueFamilyIndex;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			dstQueueFamilyIndex;
				indexBuffer.get(),							// VkBuffer			buffer;
				0u,											// VkDeviceSize		offset;
				indicesSize									// VkDeviceSize		size;
			};

			const VkBufferMemoryBarrier		stagingUboBufferUpdateBarrier =
			{
				VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// VkStructureType	sType;
				DE_NULL,									// const void*		pNext;
				VK_ACCESS_HOST_WRITE_BIT,					// VkAccessFlags	srcAccessMask;
				VK_ACCESS_TRANSFER_READ_BIT,				// VkAccessFlags	dstAccessMask;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			srcQueueFamilyIndex;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			dstQueueFamilyIndex;
				stagingUniformBuffer.get(),					// VkBuffer			buffer;
				0u,											// VkDeviceSize		offset;
				indicesSize									// VkDeviceSize		size;
			};

			const VkBufferMemoryBarrier		uboUpdateBarrier =
			{
				VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// VkStructureType	sType;
				DE_NULL,									// const void*		pNext;
				VK_ACCESS_TRANSFER_WRITE_BIT,				// VkAccessFlags	srcAccessMask;
				VK_ACCESS_UNIFORM_READ_BIT,					// VkAccessFlags	dstAccessMask;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			srcQueueFamilyIndex;
				VK_QUEUE_FAMILY_IGNORED,					// deUint32			dstQueueFamilyIndex;
				uniformBuffer.get(),						// VkBuffer			buffer;
				0u,											// VkDeviceSize		offset;
				sizeof(drawColor)							// VkDeviceSize		size;
			};


			vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &stagingVertexBufferUpdateBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);
			VkBufferCopy	vertexBufferCopy	= { 0u, 0u, verticesSize };
			vk.cmdCopyBuffer(*cmdBuffer, stagingVertexBuffer.get(), vertexBuffer.get(), 1u, &vertexBufferCopy);
			vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &vertexBufferUpdateBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);

			vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &stagingIndexBufferUpdateBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);
			VkBufferCopy	indexBufferCopy	= { 0u, 0u, indicesSize };
			vk.cmdCopyBuffer(*cmdBuffer, stagingIndexBuffer.get(), indexBuffer.get(), 1u, &indexBufferCopy);
			vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &indexBufferUpdateBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);

			vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &stagingUboBufferUpdateBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);
			VkBufferCopy	uboBufferCopy	= { 0u, 0u, sizeof(drawColor) };
			vk.cmdCopyBuffer(*cmdBuffer, stagingUniformBuffer.get(), uniformBuffer.get(), 1u, &uboBufferCopy);
			vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &uboUpdateBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);

			if (m_drawTessellatedSphere)
			{
				const VkBufferMemoryBarrier		stagingsboUpdateBarrier =
				{
					VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// VkStructureType	sType;
					DE_NULL,									// const void*		pNext;
					VK_ACCESS_HOST_WRITE_BIT,					// VkAccessFlags	srcAccessMask;
					VK_ACCESS_TRANSFER_READ_BIT,				// VkAccessFlags	dstAccessMask;
					VK_QUEUE_FAMILY_IGNORED,					// deUint32			srcQueueFamilyIndex;
					VK_QUEUE_FAMILY_IGNORED,					// deUint32			dstQueueFamilyIndex;
					stagingSboBuffer.get(),						// VkBuffer			buffer;
					0u,											// VkDeviceSize		offset;
					sizeof(tessLevel)							// VkDeviceSize		size;
				};

				const VkBufferMemoryBarrier		sboUpdateBarrier =
				{
					VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// VkStructureType	sType;
					DE_NULL,									// const void*		pNext;
					VK_ACCESS_TRANSFER_WRITE_BIT,				// VkAccessFlags	srcAccessMask;
					VK_ACCESS_SHADER_READ_BIT,					// VkAccessFlags	dstAccessMask;
					VK_QUEUE_FAMILY_IGNORED,					// deUint32			srcQueueFamilyIndex;
					VK_QUEUE_FAMILY_IGNORED,					// deUint32			dstQueueFamilyIndex;
					sboBuffer.get(),							// VkBuffer			buffer;
					0u,											// VkDeviceSize		offset;
					sizeof(tessLevel)							// VkDeviceSize		size;
				};

				vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &stagingsboUpdateBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);
				VkBufferCopy	sboBufferCopy	= { 0u, 0u, sizeof(tessLevel) };
				vk.cmdCopyBuffer(*cmdBuffer, stagingSboBuffer.get(), sboBuffer.get(), 1u, &sboBufferCopy);
				vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &sboUpdateBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);
			}

			vk.cmdBindPipeline(*cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
			vk.cmdBindDescriptorSets(*cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipelineLayout, 0u, 1, &*descriptorSet, 0u, DE_NULL);
			{
				const VkDeviceSize bindingOffset = 0;
				vk.cmdBindVertexBuffers(*cmdBuffer, 0u, 1u, &vertexBuffer.get(), &bindingOffset);
				vk.cmdBindIndexBuffer(*cmdBuffer, *indexBuffer, 0, VK_INDEX_TYPE_UINT32);
			}
		}

		// Begin renderpass
		{
			const VkClearValue clearValue = makeClearValueColorF32(
				clearColor[0],
				clearColor[1],
				clearColor[2],
				clearColor[3]);

			VkRect2D zeroRect = { { 0, 0, },{ 0, 0, } };
			vector<VkRect2D> renderAreas;
			for (deUint32 i = 0; i < m_physicalDeviceCount; i++)
				renderAreas.push_back(zeroRect);

			// Render completely if there is only 1 device
			if (m_physicalDeviceCount == 1u)
			{
				renderAreas[0].extent.width = (deInt32)renderSize.x();
				renderAreas[0].extent.height = (deInt32)renderSize.y();
			}
			else
			{
				// Split into 2 vertical halves
				renderAreas[firstDeviceID].extent.width		= (deInt32)renderSize.x() / 2;
				renderAreas[firstDeviceID].extent.height	= (deInt32)renderSize.y();
				renderAreas[secondDeviceID]					= renderAreas[firstDeviceID];
				renderAreas[secondDeviceID].offset.x		= (deInt32)renderSize.x() / 2;
			}

			const VkDeviceGroupRenderPassBeginInfo deviceGroupRPBeginInfo =
			{
				VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO,
				DE_NULL,
				(deUint32)((1 << m_physicalDeviceCount) - 1),
				m_physicalDeviceCount,
				&renderAreas[0]
			};

			const VkRenderPassBeginInfo	passBeginParams =
			{
				VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,							// sType
				(m_testMode & TEST_MODE_SFR) ? &deviceGroupRPBeginInfo : DE_NULL,	// pNext
				*renderPass,														// renderPass
				*framebuffer,														// framebuffer
				{
					{ 0, 0 },
					{ renderSize.x(), renderSize.y() }
				},																// renderArea
				1u,																// clearValueCount
				&clearValue,													// pClearValues
			};
			vk.cmdBeginRenderPass(*cmdBuffer, &passBeginParams, VK_SUBPASS_CONTENTS_INLINE);
		}

		// Draw
		if (m_testMode & TEST_MODE_AFR)
		{
			vk.cmdSetDeviceMask(*cmdBuffer, 1 << secondDeviceID);
			vk.cmdDrawIndexed(*cmdBuffer, numIndices, 1u, 0, 0, 0);

		}
		else
		{
			vk.cmdSetDeviceMask(*cmdBuffer, ((1 << firstDeviceID) | (1 << secondDeviceID)));
			vk.cmdDrawIndexed(*cmdBuffer, numIndices, 1u, 0, 0, 0);
		}
		vk.cmdEndRenderPass(*cmdBuffer);

		// Change image layout for copy
		{
			const VkImageMemoryBarrier	renderFinishBarrier =
			{
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,		// sType
				DE_NULL,									// pNext
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,		// outputMask
				VK_ACCESS_TRANSFER_READ_BIT,				// inputMask
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,	// oldLayout
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,		// newLayout
				queueFamilyIndex,							// srcQueueFamilyIndex
				queueFamilyIndex,							// dstQueueFamilyIndex
				*renderImage,								// image
				{
					VK_IMAGE_ASPECT_COLOR_BIT,				// aspectMask
					0u,										// baseMipLevel
					1u,										// mipLevels
					0u,										// baseArraySlice
					1u,										// arraySize
				}											// subresourceRange
			};
			vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 0, (const VkBufferMemoryBarrier*)DE_NULL, 1, &renderFinishBarrier);
		}

		VK_CHECK(vk.endCommandBuffer(*cmdBuffer));

		// Submit & wait for completion
		{
			const deUint32 deviceMask = (1 << firstDeviceID) | (1 << secondDeviceID);
			deviceGroupSubmitInfo.commandBufferCount = 1;
			deviceGroupSubmitInfo.pCommandBufferDeviceMasks = &deviceMask;

			const VkFenceCreateInfo	fenceParams =
			{
				VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,	// sType
				DE_NULL,								// pNext
				0u,										// flags
			};
			const VkSubmitInfo		submitInfo =
			{
				VK_STRUCTURE_TYPE_SUBMIT_INFO,			// sType
				&deviceGroupSubmitInfo,					// pNext
				0u,										// waitSemaphoreCount
				DE_NULL,								// pWaitSemaphores
				(const VkPipelineStageFlags*)DE_NULL,	// pWaitDstStageMask
				1u,										// commandBufferCount
				&cmdBuffer.get(),						// pCommandBuffers
				0u,										// signalSemaphoreCount
				DE_NULL,								// pSignalSemaphores
			};
			const Unique<VkFence>	fence(createFence(vk, *m_deviceGroup, &fenceParams));

			VK_CHECK(vk.queueSubmit(m_deviceGroupQueue, 1u, &submitInfo, *fence));
			VK_CHECK(vk.waitForFences(*m_deviceGroup, 1u, &fence.get(), DE_TRUE, ~0ull));
			VK_CHECK(vk.deviceWaitIdle(*m_deviceGroup));
		}

		// Copy image from secondDeviceID
		if (m_physicalDeviceCount > 1)
		{
			Move<VkImage>			peerImage;

			// Create and bind peer image
			{
				const VkImageCreateInfo					peerImageParams =
				{
					VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,									// sType
					DE_NULL,																// pNext
					VK_IMAGE_CREATE_ALIAS_BIT,												// flags
					VK_IMAGE_TYPE_2D,														// imageType
					colorFormat,															// format
					{ renderSize.x(), renderSize.y(), 1 },									// extent
					1u,																		// mipLevels
					1u,																		// arraySize
					VK_SAMPLE_COUNT_1_BIT,													// samples
					VK_IMAGE_TILING_OPTIMAL,												// tiling
					imageUsageFlag,															// usage
					VK_SHARING_MODE_EXCLUSIVE,												// sharingMode
					1u,																		// queueFamilyIndexCount
					&queueFamilyIndex,														// pQueueFamilyIndices
					VK_IMAGE_LAYOUT_UNDEFINED,												// initialLayout
				};
				peerImage = createImage(vk, *m_deviceGroup, &peerImageParams);

				VkBindImageMemoryDeviceGroupInfo	devGroupBindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO,		// sType
					DE_NULL,													// pNext
					m_physicalDeviceCount,										// deviceIndexCount
					&deviceIndices[0],											// pDeviceIndices
					0u,															// SFRRectCount
					DE_NULL,													// pSFRRects
				};

				VkBindImageMemoryInfo				bindInfo =
				{
					VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,					// sType
					&devGroupBindInfo,											// pNext
					peerImage.get(),											// image
					imageMemory.get(),											// memory
					0u,															// memoryOffset
				};
				VK_CHECK(vk.bindImageMemory2(*m_deviceGroup, 1, &bindInfo));
			}

			// Copy peer image
			{
				// Image barriers
				const VkImageMemoryBarrier	preImageBarrier	 =
				{
					VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,		// VkStructureType		 sType;
					DE_NULL,									// const void*			 pNext;
					0,											// VkAccessFlags		 srcAccessMask;
					VK_ACCESS_TRANSFER_WRITE_BIT,				// VkAccessFlags		 dstAccessMask;
					VK_IMAGE_LAYOUT_UNDEFINED,					// VkImageLayout		 oldLayout;
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,		// VkImageLayout		 newLayout;
					VK_QUEUE_FAMILY_IGNORED,					// deUint32				 srcQueueFamilyIndex;
					VK_QUEUE_FAMILY_IGNORED,					// deUint32				 dstQueueFamilyIndex;
					*peerImage,									// VkImage				 image;
					{											// VkImageSubresourceRange subresourceRange;
						VK_IMAGE_ASPECT_COLOR_BIT,				// VkImageAspectFlags	 aspectMask;
						0u,										// deUint32				 baseMipLevel;
						1u,										// deUint32				 mipLevels;
						0u,										// deUint32				 baseArraySlice;
						1u										// deUint32				 arraySize;
					}
				};

				const VkImageMemoryBarrier	postImageBarrier =
				{
					VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,		// VkStructureType			sType;
					DE_NULL,									// const void*				pNext;
					VK_ACCESS_TRANSFER_WRITE_BIT,				// VkAccessFlags			srcAccessMask;
					VK_ACCESS_TRANSFER_READ_BIT,				// VkAccessFlags			dstAccessMask;
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,		// VkImageLayout			oldLayout;
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,		// VkImageLayout			newLayout;
					VK_QUEUE_FAMILY_IGNORED,					// deUint32					srcQueueFamilyIndex;
					VK_QUEUE_FAMILY_IGNORED,					// deUint32					dstQueueFamilyIndex;
					*peerImage,
					{											// VkImageSubresourceRange	subresourceRange;
						VK_IMAGE_ASPECT_COLOR_BIT,				// VkImageAspectFlags		aspectMask;
						0u,										// deUint32					baseMipLevel;
						1u,										// deUint32					mipLevels;
						0u,										// deUint32					baseArraySlice;
						1u										// deUint32					arraySize;
					}
				};

				// AFR: Copy entire image from secondDeviceID
				// SFR: Copy the right half of image from secondDeviceID to firstDeviceID, so that the copy
				// to a buffer below (for checking) does not require VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT
				deInt32 imageOffsetX = (m_testMode & TEST_MODE_AFR) ? 0 : renderSize.x()/2;
				deUint32 imageExtentX = (m_testMode & TEST_MODE_AFR) ? (deUint32)renderSize.x() : (deUint32)renderSize.x()/2;

				const VkImageCopy	imageCopy	=
				{
					{
						VK_IMAGE_ASPECT_COLOR_BIT,
						0, // mipLevel
						0, // arrayLayer
						1  // layerCount
					},
					{ imageOffsetX, 0, 0 },
					{
						VK_IMAGE_ASPECT_COLOR_BIT,
						0, // mipLevel
						0, // arrayLayer
						1  // layerCount
					},
					{ imageOffsetX, 0, 0 },
					{
						imageExtentX,
						(deUint32)renderSize.y(),
						1u
					}
				};

				VK_CHECK(vk.beginCommandBuffer(*cmdBuffer, &cmdBufBeginParams));
				vk.cmdSetDeviceMask(*cmdBuffer, 1 << secondDeviceID);
				vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 0, (const VkBufferMemoryBarrier*)DE_NULL, 1u, &preImageBarrier);
				vk.cmdCopyImage(*cmdBuffer, *renderImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *peerImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopy);
				vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 0, (const VkBufferMemoryBarrier*)DE_NULL, 1u, &postImageBarrier);
				VK_CHECK(vk.endCommandBuffer(*cmdBuffer));
			}

			// Submit & wait for completion
			{
				const deUint32 deviceMask = 1 << secondDeviceID;
				deviceGroupSubmitInfo.pCommandBufferDeviceMasks = &deviceMask;
				const VkFenceCreateInfo	fenceParams =
				{
					VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,	// sType
					DE_NULL,								// pNext
					0u,										// flags
				};
				const VkSubmitInfo		submitInfo =
				{
					VK_STRUCTURE_TYPE_SUBMIT_INFO,			// sType
					&deviceGroupSubmitInfo,					// pNext
					0u,										// waitSemaphoreCount
					DE_NULL,								// pWaitSemaphores
					(const VkPipelineStageFlags*)DE_NULL,
					1u,										// commandBufferCount
					&cmdBuffer.get(),						// pCommandBuffers
					0u,										// signalSemaphoreCount
					DE_NULL,								// pSignalSemaphores
				};
				const Unique<VkFence>	fence(createFence(vk, *m_deviceGroup, &fenceParams));

				VK_CHECK(vk.queueSubmit(m_deviceGroupQueue, 1u, &submitInfo, *fence));
				VK_CHECK(vk.waitForFences(*m_deviceGroup, 1u, &fence.get(), DE_TRUE, ~0ull));
				VK_CHECK(vk.deviceWaitIdle(*m_deviceGroup));
			}
		}

		// copy image to read buffer for checking
		{
			const VkDeviceSize			imageSizeBytes = (VkDeviceSize)(sizeof(deUint32) * renderSize.x() * renderSize.y());
			const VkBufferCreateInfo	readImageBufferParams =
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,		// sType
				DE_NULL,									// pNext
				(VkBufferCreateFlags)0u,					// flags
				imageSizeBytes,								// size
				VK_BUFFER_USAGE_TRANSFER_DST_BIT,			// usage
				VK_SHARING_MODE_EXCLUSIVE,					// sharingMode
				1u,											// queueFamilyIndexCount
				&queueFamilyIndex,							// pQueueFamilyIndices
			};
			const Unique<VkBuffer>		readImageBuffer(createBuffer(vk, *m_deviceGroup, &readImageBufferParams));
			const UniquePtr<Allocation>	readImageBufferMemory(memAlloc.allocate(getBufferMemoryRequirements(vk, *m_deviceGroup, *readImageBuffer), MemoryRequirement::HostVisible));
			VK_CHECK(vk.bindBufferMemory(*m_deviceGroup, *readImageBuffer, readImageBufferMemory->getMemory(), readImageBufferMemory->getOffset()));

			VK_CHECK(vk.beginCommandBuffer(*cmdBuffer, &cmdBufBeginParams));

			// Copy image to buffer
			{
				const VkBufferImageCopy	copyParams =
				{
					(VkDeviceSize)0u,						// bufferOffset
					renderSize.x(),							// bufferRowLength
					renderSize.y(),							// bufferImageHeight
					{
						VK_IMAGE_ASPECT_COLOR_BIT,			// aspectMask
						0u,									// mipLevel
						0u,									// baseArrayLayer
						1u,									// layerCount
					},										// imageSubresource
					{ 0, 0, 0 },							// imageOffset
					{
						renderSize.x(),
						renderSize.y(),
						1u
					}										// imageExtent
				};
				vk.cmdCopyImageToBuffer(*cmdBuffer, *readImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *readImageBuffer, 1u, &copyParams);

				const VkBufferMemoryBarrier	copyFinishBarrier =
				{
					VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,	// sType
					DE_NULL,									// pNext
					VK_ACCESS_TRANSFER_WRITE_BIT,				// srcAccessMask
					VK_ACCESS_HOST_READ_BIT,					// dstAccessMask
					queueFamilyIndex,							// srcQueueFamilyIndex
					queueFamilyIndex,							// dstQueueFamilyIndex
					*readImageBuffer,							// buffer
					0u,											// offset
					imageSizeBytes								// size
				};
				vk.cmdPipelineBarrier(*cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, (VkDependencyFlags)0, 0, (const VkMemoryBarrier*)DE_NULL, 1, &copyFinishBarrier, 0, (const VkImageMemoryBarrier*)DE_NULL);
			}
			VK_CHECK(vk.endCommandBuffer(*cmdBuffer));

			// Submit & wait for completion
			{
				const deUint32 deviceMask = 1 << firstDeviceID;
				deviceGroupSubmitInfo.pCommandBufferDeviceMasks = &deviceMask;
				const VkFenceCreateInfo	fenceParams =
				{
					VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,	// sType
					DE_NULL,								// pNext
					0u,										// flags
				};
				const VkSubmitInfo		submitInfo =
				{
					VK_STRUCTURE_TYPE_SUBMIT_INFO,			// sType
					&deviceGroupSubmitInfo,					// pNext
					0u,										// waitSemaphoreCount
					DE_NULL,								// pWaitSemaphores
					(const VkPipelineStageFlags*)DE_NULL,	// flags
					1u,										// commandBufferCount
					&cmdBuffer.get(),						// pCommandBuffers
					0u,										// signalSemaphoreCount
					DE_NULL,								// pSignalSemaphores
				};
				const Unique<VkFence>	fence(createFence(vk, *m_deviceGroup, &fenceParams));

				VK_CHECK(vk.queueSubmit(m_deviceGroupQueue, 1u, &submitInfo, *fence));
				VK_CHECK(vk.waitForFences(*m_deviceGroup, 1u, &fence.get(), DE_TRUE, ~0ull));
				VK_CHECK(vk.deviceWaitIdle(*m_deviceGroup));
			}

			// Read results and check against reference image
			if (m_drawTessellatedSphere)
			{
				const tcu::TextureFormat			tcuFormat = vk::mapVkFormat(colorFormat);
				const VkMappedMemoryRange			range =
				{
					VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,	// sType
					DE_NULL,								// pNext
					readImageBufferMemory->getMemory(),		// memory
					0,										// offset
					imageSizeBytes,							// size
				};
				const tcu::ConstPixelBufferAccess	resultAccess(tcuFormat, renderSize.x(), renderSize.y(), 1, readImageBufferMemory->getHostPtr());
				VK_CHECK(vk.invalidateMappedMemoryRanges(*m_deviceGroup, 1u, &range));

				tcu::TextureLevel referenceImage;
				string refImage = m_fillModeNonSolid ? "vulkan/data/device_group/sphere.png" : "vulkan/data/device_group/spherefilled.png";
				tcu::ImageIO::loadPNG(referenceImage, m_context.getTestContext().getArchive(),  refImage.c_str());
				iterateResultSuccess = tcu::fuzzyCompare(m_context.getTestContext().getLog(), "ImageComparison", "Image Comparison",
										referenceImage.getAccess(), resultAccess, 0.001f, tcu::COMPARE_LOG_RESULT);
			}
			else
			{
				const tcu::TextureFormat			tcuFormat = vk::mapVkFormat(colorFormat);
				const VkMappedMemoryRange			range =
				{
					VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,	// sType
					DE_NULL,								// pNext
					readImageBufferMemory->getMemory(),		// memory
					0,										// offset
					imageSizeBytes,							// size
				};
				const tcu::ConstPixelBufferAccess	resultAccess(tcuFormat, renderSize.x(), renderSize.y(), 1, readImageBufferMemory->getHostPtr());
				VK_CHECK(vk.invalidateMappedMemoryRanges(*m_deviceGroup, 1u, &range));

				// Render reference and compare
				{
					tcu::TextureLevel	refImage(tcuFormat, (deInt32)renderSize.x(), (deInt32)renderSize.y());
					const tcu::UVec4	threshold(0u);
					const tcu::IVec3	posDeviation(1, 1, 0);

					tcu::clear(refImage.getAccess(), clearColor);
					renderReferenceTriangle(refImage.getAccess(), triVertices);

					iterateResultSuccess = tcu::intThresholdPositionDeviationCompare(m_context.getTestContext().getLog(),
						"ComparisonResult",
						"Image comparison result",
						refImage.getAccess(),
						resultAccess,
						threshold,
						posDeviation,
						false,
						tcu::COMPARE_LOG_RESULT);
				}
			}
		}

		if (!iterateResultSuccess)
			return tcu::TestStatus::fail("Image comparison failed");
	}

	return tcu::TestStatus(QP_TEST_RESULT_PASS, "Device group verification passed");
}

template<class Instance>
class DeviceGroupTestCase : public TestCase
{
public:
	DeviceGroupTestCase (tcu::TestContext& context,
						const char*	name,
						const char*	description,
						deUint32 mode)
	: TestCase(context, name, description)
	, m_testMode		(mode)
	{}

private:

	deUint32			m_testMode;

	TestInstance*		createInstance	(Context& context) const
	{
		return new Instance(context, m_testMode);
	}

	void				initPrograms	(vk::SourceCollections& programCollection) const
	{
		programCollection.glslSources.add("vert") << glu::VertexSource("#version 430\n"
			"layout(location = 0) in vec4 in_Position;\n"
			"out gl_PerVertex { vec4 gl_Position; float gl_PointSize; };\n"
			"void main() {\n"
			"	gl_Position	= in_Position;\n"
			"	gl_PointSize = 1.0;\n"
			"}\n");

		if (m_testMode & TEST_MODE_TESSELLATION)
		{
			programCollection.glslSources.add("tesc") << glu::TessellationControlSource("#version 450\n"
				"#extension GL_EXT_tessellation_shader : require\n"
				"layout(vertices=3) out;\n"
				"layout(set=0, binding=1) buffer tessLevel { \n"
				"  float tessLvl;\n"
				"};\n"
				"void main()\n"
				"{\n"
				"  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
				"  if (gl_InvocationID == 0) {\n"
				"    for (int i = 0; i < 4; i++)\n"
				"      gl_TessLevelOuter[i] = tessLvl;\n"
				"    for (int i = 0; i < 2; i++)\n"
				"      gl_TessLevelInner[i] = tessLvl;\n"
				"  }\n"
				"}\n");

			programCollection.glslSources.add("tese") << glu::TessellationEvaluationSource("#version 450\n"
				"#extension GL_EXT_tessellation_shader : require\n"
				"layout(triangles) in;\n"
				"layout(equal_spacing) in;\n"
				"layout(ccw) in;\n"
				"void main()\n"
				"{\n"
				"  vec4 pos = vec4(0, 0, 0, 0);\n"
				"  vec3 tessCoord = gl_TessCoord.xyz;\n"
				"  pos += tessCoord.z * gl_in[0].gl_Position;\n"
				"  pos += tessCoord.x * gl_in[1].gl_Position;\n"
				"  pos += tessCoord.y * gl_in[2].gl_Position;\n"
				"  vec3 sign = sign(pos.xyz);\n"
				"  pos.xyz = 0.785398 - abs(pos.xyz) * 1.5707963;\n"
				"  pos.xyz = (1 - tan(pos.xyz))/2.0;\n"
				"  pos.xyz = (sign * pos.xyz) / length(pos.xyz);\n"
				"  gl_Position = pos;\n"
				"}\n");
		}

		programCollection.glslSources.add("frag") << glu::FragmentSource("#version 430\n"
			"layout(location = 0) out vec4 out_FragColor;\n"
			"layout(std140, set=0, binding=0) uniform bufferData { \n"
			"	vec4 color;\n"
			"};\n"
			"void main()\n"
			"{\n"
			"	out_FragColor = color;\n"
			"}\n");
	}
};

} //anonymous

class DeviceGroupTestRendering : public tcu::TestCaseGroup
{
public:
								DeviceGroupTestRendering	(tcu::TestContext& testCtx);
								~DeviceGroupTestRendering	(void) {}
	void						init(void);

private:
								DeviceGroupTestRendering	(const DeviceGroupTestRendering& other);
	DeviceGroupTestRendering&	operator=					(const DeviceGroupTestRendering& other);
};

DeviceGroupTestRendering::DeviceGroupTestRendering (tcu::TestContext& testCtx)
	: TestCaseGroup (testCtx, "device_group", "Testing device group test cases")
{
	// Left blank on purpose
}

void DeviceGroupTestRendering::init (void)
{
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "sfr",							"Test split frame rendering",														TEST_MODE_SFR));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "sfr_sys",						"Test split frame rendering with render target in host memory",						TEST_MODE_SFR | TEST_MODE_HOSTMEMORY));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "sfr_dedicated",				"Test split frame rendering with dedicated memory allocations",						TEST_MODE_SFR | TEST_MODE_DEDICATED));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "sfr_dedicated_peer",			"Test split frame rendering with dedicated memory allocations and peer fetching",	TEST_MODE_SFR | TEST_MODE_DEDICATED | TEST_MODE_PEER_FETCH));

	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "afr",							"Test alternate frame rendering",													TEST_MODE_AFR));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "afr_sys",						"Test split frame rendering with render target in host memory",						TEST_MODE_AFR | TEST_MODE_HOSTMEMORY));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "afr_dedicated",				"Test split frame rendering with dedicated memory allocations",						TEST_MODE_AFR | TEST_MODE_DEDICATED));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "afr_dedicated_peer",			"Test split frame rendering with dedicated memory allocations and peer fetching",	TEST_MODE_AFR | TEST_MODE_DEDICATED | TEST_MODE_PEER_FETCH));

	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "sfr_tessellated",				"Test split frame rendering with tessellated sphere",								TEST_MODE_SFR | TEST_MODE_TESSELLATION | TEST_MODE_DEDICATED | TEST_MODE_PEER_FETCH));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "sfr_tessellated_linefill",	"Test split frame rendering with tessellated sphere with line segments",			TEST_MODE_SFR | TEST_MODE_TESSELLATION | TEST_MODE_LINEFILL  | TEST_MODE_DEDICATED | TEST_MODE_PEER_FETCH));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "afr_tessellated",				"Test alternate frame rendering with tesselated sphere",							TEST_MODE_AFR | TEST_MODE_TESSELLATION | TEST_MODE_DEDICATED | TEST_MODE_PEER_FETCH));
	addChild(new DeviceGroupTestCase<DeviceGroupTestInstance>(m_testCtx, "afr_tessellated_linefill",	"Test alternate frame rendering with tesselated sphere with line segments",			TEST_MODE_AFR | TEST_MODE_TESSELLATION | TEST_MODE_LINEFILL  | TEST_MODE_DEDICATED | TEST_MODE_PEER_FETCH));
}

tcu::TestCaseGroup* createTests(tcu::TestContext& testCtx)
{
	return new DeviceGroupTestRendering(testCtx);
}
}	// DeviceGroup
}	// vkt
