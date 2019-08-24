#include "LightProbe.h"
#include "Advanced/XUSGDDSLoader.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CosConstants
{
	float		MapSize;
	uint32_t	NumLevels;
	uint32_t	Level;
};

LightProbe::LightProbe(const Device& device) :
	m_device(device),
	m_groundTruth(nullptr),
	m_numMips(11)
{
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

LightProbe::~LightProbe()
{
}

bool LightProbe::Init(const CommandList& commandList, uint32_t width, uint32_t height,
	const shared_ptr<DescriptorTableCache>& descriptorTableCache, vector<Resource>& uploaders,
	const wstring pFileNames[], uint32_t numFiles)
{
	m_descriptorTableCache = descriptorTableCache;

	// Load input image
	auto texWidth = 1u, texHeight = 1u;
	m_sources.resize(numFiles);
	for (auto i = 0u; i < numFiles; ++i)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.push_back(nullptr);
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, commandList, pFileNames[i].c_str(),
			8192, false, m_sources[i], uploaders.back(), &alphaMode), false);

		const auto& desc = m_sources[i]->GetResource()->GetDesc();
		texWidth = (max)(static_cast<uint32_t>(desc.Width), texWidth);
		texHeight = (max)(desc.Height, texHeight);
	}

	// Create resources and pipelines
	m_numMips = (max)(Log2((max)(texWidth, texHeight)), 1ui8) + 1;
	m_mapSize = (texWidth + texHeight) * 0.5f;

	const auto format = Format::R16G16B16A16_FLOAT;
	m_filtered[TABLE_DOWN_SAMPLE].Create(m_device, texWidth, texHeight, format,
		6, ResourceFlag::ALLOW_UNORDERED_ACCESS, m_numMips - 1, 1,
		MemoryType::DEFAULT, ResourceState::COMMON, true);
	m_filtered[TABLE_UP_SAMPLE].Create(m_device, texWidth, texHeight, format,
		6, ResourceFlag::ALLOW_UNORDERED_ACCESS, m_numMips, 1,
		MemoryType::DEFAULT, ResourceState::COMMON, true);

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void LightProbe::UpdateFrame(double time)
{
	m_time = time;
}

void LightProbe::Process(const CommandList& commandList, ResourceState dstState)
{
	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	generateRadiance(commandList);
	process(commandList, dstState);
}

ResourceBase* LightProbe::GetIrradianceGT(const CommandList& commandList,
	const wchar_t* fileName, vector<Resource>* pUploaders)
{
	if (!m_groundTruth && fileName && pUploaders)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		pUploaders->push_back(nullptr);
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, commandList, fileName,
			8192, false, m_groundTruth, pUploaders->back(), &alphaMode), false);
	}

	return m_groundTruth.get();
}

Texture2D& LightProbe::GetIrradiance()
{
	return m_filtered[TABLE_UP_SAMPLE];
}

Texture2D& LightProbe::GetRadiance()
{
	return m_filtered[TABLE_DOWN_SAMPLE];
}

bool LightProbe::createPipelineLayouts()
{
	// Generate Radiance
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetConstants(1, SizeOfInUint32(float), 0);
		utilPipelineLayout.SetRange(2, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(3, DescriptorType::SRV, 2, 0);
		X_RETURN(m_pipelineLayouts[RADIANCE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"RadianceGenerationLayout"), false);
	}

	// Resampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[RESAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingLayout"), false);
	}

	// Up sampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(CosConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingLayout"), false);
	}

	return true;
}

bool LightProbe::createPipelines()
{
	// Generate Radiance
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, RADIANCE, L"CSRadiance.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RADIANCE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, RADIANCE));
		X_RETURN(m_pipelines[RADIANCE], state.GetPipeline(m_computePipelineCache, L"RadianceGeneration"), false);
	}

	// Resampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, RESAMPLE, L"CSResample.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, RESAMPLE));
		X_RETURN(m_pipelines[RESAMPLE], state.GetPipeline(m_computePipelineCache, L"Resampling"), false);
	}

	// Up sampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, UP_SAMPLE, L"CSCosineUp.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, UP_SAMPLE));
		X_RETURN(m_pipelines[UP_SAMPLE], state.GetPipeline(m_computePipelineCache, L"UpSampling"), false);
	}

	return true;
}

bool LightProbe::createDescriptorTables()
{
	const uint8_t numPasses = m_numMips - 1;
	m_uavSrvTables[TABLE_DOWN_SAMPLE].resize(m_numMips);
	m_uavSrvTables[TABLE_UP_SAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		// Get UAV and SRVs
		{
			const Descriptor descriptors[] =
			{
				m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(i),
				m_filtered[TABLE_DOWN_SAMPLE].GetUAV(i + 1)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}
	}

	for (auto i = 0ui8; i < numPasses; ++i)
	{
		{
			const auto coarser = numPasses - i;
			const auto current = coarser - 1;
			const Descriptor descriptors[] =
			{
				m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(current),
				m_filtered[TABLE_UP_SAMPLE].GetSRVLevel(coarser),
				m_filtered[TABLE_UP_SAMPLE].GetUAV(current)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_UP_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}
	}

	// Get UAV and SRVs for the final-time down sampling
	{
		const Descriptor descriptors[] =
		{
			m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(numPasses - 1),
			m_filtered[TABLE_UP_SAMPLE].GetUAV(numPasses)
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get UAV table for radiance generation
	Util::DescriptorTable utilUavTable;
	utilUavTable.SetDescriptors(0, 1, &m_filtered[TABLE_DOWN_SAMPLE].GetUAV());
	X_RETURN(m_uavTable, utilUavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);

	// Get SRV tables for radiance generation
	const auto numSources = static_cast<uint32_t>(m_sources.size());
	m_srvTables.resize(m_sources.size());
	for (auto i = 0u; i + 1 < numSources; ++i)
	{
		Util::DescriptorTable utilSrvTable;
		utilSrvTable.SetDescriptors(0, 1, &m_sources[i]->GetSRV());
		X_RETURN(m_srvTables[i], utilSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}
	{
		const auto i = numSources - 1;
		const Descriptor descriptors[] =
		{
			m_sources[i]->GetSRV(),
			m_sources[0]->GetSRV()
		};
		Util::DescriptorTable utilSrvTable;
		utilSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[i], utilSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_WRAP;
	samplerTable.SetSamplers(0, 1, &sampler, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}

void LightProbe::generateRadiance(const CommandList& commandList)
{
	static const auto period = 3.0;
	ResourceBarrier barrier;
	const auto numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	commandList.Barrier(numBarriers, &barrier);

	const auto numSources = static_cast<uint32_t>(m_srvTables.size());
	auto blend = static_cast<float>(m_time / period);
	auto i = static_cast<uint32_t>(m_time / period);
	blend = numSources > 1 ? blend - i : 0.0f;
	i %= numSources;

	commandList.SetComputePipelineLayout(m_pipelineLayouts[RADIANCE]);
	commandList.SetPipelineState(m_pipelines[RADIANCE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);
	commandList.SetCompute32BitConstant(1, reinterpret_cast<const uint32_t&>(blend));

	m_filtered[TABLE_DOWN_SAMPLE].Blit(commandList, 8, 8, 1, m_uavTable, 2, 0, m_srvTables[i], 3);
}

void LightProbe::process(const CommandList& commandList, ResourceState dstState)
{
	const uint8_t numPasses = m_numMips - 1;

	// Generate Mips
	ResourceBarrier barriers[12];
	auto numBarriers = 0u;
	if (numPasses > 0) numBarriers = m_filtered[TABLE_DOWN_SAMPLE].GenerateMips(commandList, barriers,
		8, 8, 1, ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::PIXEL_SHADER_RESOURCE,
		m_pipelineLayouts[RESAMPLE], m_pipelines[RESAMPLE], m_uavSrvTables[TABLE_DOWN_SAMPLE].data(),
		1, m_samplerTable, 0, numBarriers, nullptr, 0, 1, numPasses - 1);
	numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	commandList.Barrier(numBarriers, barriers);
	numBarriers = 0;

	commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses]);
	commandList.Dispatch(1, 1, 6);

	// Up sampling
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	CosConstants cb = { m_mapSize, m_numMips };
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		cb.Level = c - 1;
		commandList.SetCompute32BitConstants(2, SizeOfInUint32(cb), &cb);
		numBarriers = m_filtered[TABLE_UP_SAMPLE].Blit(commandList, barriers, 8, 8, 1,
			cb.Level, c, dstState, m_uavSrvTables[TABLE_UP_SAMPLE][i], 1, numBarriers);
	}
}