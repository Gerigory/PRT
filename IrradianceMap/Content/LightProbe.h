//--------------------------------------------------------------------------------------
// By XU, Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class LightProbe
{
public:
	enum PipelineType
	{
		HYBRID,
		GRAPHICS,
		COMPUTE,

		NUM_PIPE_TYPE
	};

	LightProbe(const XUSG::Device &device);
	virtual ~LightProbe();

	bool Init(const XUSG::CommandList &commandList, uint32_t width, uint32_t height,
		const std::shared_ptr<XUSG::DescriptorTableCache>& descriptorTableCache,
		std::vector<XUSG::Resource> &uploaders, const std::wstring pFileNames[],
		uint32_t numFiles);

	void UpdateFrame(double time);
	void Process(const XUSG::CommandList &commandList, XUSG::ResourceState dstState, PipelineType pipelineType);

	XUSG::ResourceBase* GetIrradianceGT(const XUSG::CommandList& commandList,
		const wchar_t* fileName = nullptr, std::vector<XUSG::Resource>* pUploaders = nullptr);
	XUSG::Texture2D& GetIrradiance();
	XUSG::Texture2D& GetRadiance();
	XUSG::Descriptor GetSH(const XUSG::CommandList& commandList,
		const wchar_t* fileName = nullptr, std::vector<XUSG::Resource>* pUploaders = nullptr);

protected:
	enum PipelineIndex : uint8_t
	{
		RADIANCE_G,
		RADIANCE_C,
		RESAMPLE_G,
		RESAMPLE_C,
		UP_SAMPLE_G,
		UP_SAMPLE_C,

		NUM_PIPELINE
	};

	enum UavSrvTableIndex : uint8_t
	{
		TABLE_DOWN_SAMPLE,
		TABLE_UP_SAMPLE,
		TABLE_RESAMPLE,

		NUM_UAV_SRV
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, bool typedUAV = true);
	bool createDescriptorTables();

	void generateRadianceGraphics(const XUSG::CommandList& commandList);
	void generateRadianceCompute(const XUSG::CommandList& commandList);
	void generateMipsGraphics(const XUSG::CommandList& commandList, XUSG::ResourceState dstState);
	void generateMipsCompute(const XUSG::CommandList& commandList, XUSG::ResourceState dstState);
	void upsampleGraphics(const XUSG::CommandList& commandList, XUSG::ResourceState dstState);
	void processLegacy(const XUSG::CommandList& commandList, XUSG::ResourceState dstState);
	
	XUSG::Device m_device;

	XUSG::ShaderPool				m_shaderPool;
	XUSG::Graphics::PipelineCache	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache	m_computePipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	std::shared_ptr<XUSG::DescriptorTableCache> m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	std::vector<XUSG::DescriptorTable> m_uavSrvTables[NUM_UAV_SRV];
	std::vector<XUSG::DescriptorTable> m_srvTables;
	XUSG::DescriptorTable	m_uavTable;
	XUSG::DescriptorTable	m_samplerTable;

	std::shared_ptr<XUSG::ResourceBase> m_groundTruth;
	std::vector<std::shared_ptr<XUSG::ResourceBase>> m_sources;
	XUSG::RenderTarget		m_radiance;
	XUSG::RenderTarget		m_irradiance;

	XUSG::ConstantBuffer	m_cbCoeffSH;

	uint8_t					m_numMips;
	float					m_mapSize;
	double					m_time;
};
