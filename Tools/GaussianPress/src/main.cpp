#include <stdio.h>
#include <vector>
#include <algorithm>
#include "compressors.h"
#include "compression_helpers.h"
#include "filters.h"
#include "systeminfo.h"
#include <math.h>
#include <memory>
#include <meshoptimizer.h>

/*
Initial, just raw data compression on bicycle_7k, bicycle_30k, truck_7k:
Compressor     SizeGB CTimeS  DTimeS   Ratio   CGB/s   DGB/s
		 Raw   2.652
  zstd-bd_n1   2.406   3.461   2.662   1.102   0.766   0.996
   zstd-bd_1   2.178   4.031   2.964   1.218   0.658   0.894
	  lz4-bd   2.413   2.934   2.615   1.099   0.904   1.014
	 zstd_n1   2.456   3.905   0.386   1.080   0.679   6.870
	  zstd_1   2.323   5.903   2.340   1.142   0.449   1.133
		 lz4   2.501   2.289   0.353   1.060   1.159   7.514

Morton3D reorder by 21 bits per axis: saves ~50MB raw, ~150-200MB bytedelta
Compressor     SizeGB CTimeS  DTimeS   Ratio   CGB/s   DGB/s
		 Raw   2.652
  zstd-bd_n1   2.164   4.375   3.007   1.225   0.606   0.882
   zstd-bd_1   2.043   5.524   3.278   1.298   0.480   0.809
	  lz4-bd   2.207   3.677   2.578   1.201   0.721   1.029
	 zstd_n1   2.405   3.741   0.380   1.102   0.709   6.982
	  zstd_1   2.271   6.087   2.309   1.167   0.436   1.148
		 lz4   2.473   2.292   0.342   1.072   1.157   7.752
	 meshopt   2.230   5.780   0.820   1.189   0.459   3.232
*/

#define SOKOL_TIME_IMPL
#include "../libs/sokol_time.h"


struct FullVertex
{
	float px, py, pz;
	float nx, ny, nz;
	float dcr, dcg, dcb;
	float shr[15];
	float shg[15];
	float shb[15];
	float opacity;
	float sx, sy, sz;
	float rw, rx, ry, rz;
};
constexpr size_t kFullVertexStride = 248;
constexpr size_t kFullVertexFloats = kFullVertexStride / 4;
static_assert(sizeof(FullVertex) == kFullVertexStride);

constexpr int kRuns = 1;

struct FilterDesc
{
	const char* name = nullptr;
	void (*filterFunc)(const uint8_t* src, uint8_t* dst, size_t channels, size_t dataElems) = nullptr;
	void (*unfilterFunc)(const uint8_t* src, uint8_t* dst, size_t channels, size_t dataElems) = nullptr;
};

static FilterDesc g_FilterByteDelta = { "-bd", Filter_ByteDelta, UnFilter_ByteDelta };

static std::unique_ptr<GenericCompressor> g_CompZstd = std::make_unique<GenericCompressor>(kCompressionZstd);
static std::unique_ptr<GenericCompressor> g_CompLZ4 = std::make_unique<GenericCompressor>(kCompressionLZ4);
static std::unique_ptr<Compressor> g_CompMeshOpt = std::make_unique<MeshOptCompressor>(kCompressionCount);


struct TestFile
{
	const char* title = nullptr;
	const char* path = nullptr;
	std::vector<uint8_t> origFileData;
	std::vector<uint8_t> fileData;
	size_t vertexCount = 0;
	size_t vertexStride = 0;

	FullVertex valMin;
	FullVertex valMax;
	FullVertex errMax;
	FullVertex errAvg;
};

enum BlockSize
{
	kBSizeNone,
	kBSize64k,
	kBSize256k,
	kBSize1M,
	kBSize4M,
	kBSize16M,
	kBSize64M,
	kBSizeCount
};
static const size_t kBlockSizeToActualSize[] =
{
	0,
	64 * 1024,
	256 * 1024,
	1024 * 1024,
	4 * 1024 * 1024,
	16 * 1024 * 1024,
	64 * 1024 * 1024,
};
static_assert(sizeof(kBlockSizeToActualSize)/sizeof(kBlockSizeToActualSize[0]) == kBSizeCount, "block size table size mismatch");
static const char* kBlockSizeName[] =
{
	"",
	"-64k",
	"-256k",
	"-1M",
	"-4M",
	"-16M",
	"-64M",
};
static_assert(sizeof(kBlockSizeName) / sizeof(kBlockSizeName[0]) == kBSizeCount, "block size name table size mismatch");

struct CompressorConfig
{
	Compressor* cmp;
	FilterDesc* filter;
	BlockSize blockSizeEnum = kBSizeNone;

	std::string GetName() const
	{
		char buf[100];
		cmp->PrintName(sizeof(buf), buf);
		std::string res = buf;
		if (filter != nullptr)
			res += filter->name;
		res += kBlockSizeName[blockSizeEnum];
		return res;
	}

	uint8_t* CompressWhole(const TestFile& tf, int level, size_t& outCompressedSize)
	{
		const uint8_t* srcData = tf.fileData.data();
		uint8_t* filterBuffer = nullptr;
		if (filter)
		{
			filterBuffer = new uint8_t[tf.fileData.size()];
			filter->filterFunc(srcData, filterBuffer, tf.vertexStride, tf.vertexCount);
			srcData = filterBuffer;
		}

		outCompressedSize = 0;
		uint8_t* compressed = cmp->Compress(level, srcData, tf.vertexCount, tf.vertexStride, outCompressedSize);
		delete[] filterBuffer;
		return compressed;
	}

	uint8_t* Compress(const TestFile& tf, int level, size_t& outCompressedSize)
	{
		if (blockSizeEnum == kBSizeNone)
			return CompressWhole(tf, level, outCompressedSize);

		size_t blockSize = kBlockSizeToActualSize[blockSizeEnum];
		// make sure multiple of data elem size
		blockSize = (blockSize / tf.vertexStride) * tf.vertexStride;

		uint8_t* filterBuffer = nullptr;
		if (filter)
			filterBuffer = new uint8_t[blockSize];

		const size_t dataSize = tf.fileData.size();
		const uint8_t* srcData = tf.fileData.data();
		uint8_t* compressed = new uint8_t[dataSize + 4];
		size_t srcOffset = 0;
		size_t cmpOffset = 0;
		while (srcOffset < dataSize)
		{
			size_t thisBlockSize = std::min(blockSize, dataSize - srcOffset);
			if (filter)
			{
				filter->filterFunc(srcData + srcOffset, filterBuffer, tf.vertexStride, thisBlockSize / tf.vertexStride);
			}
			size_t thisCmpSize = 0;
			uint8_t* thisCmp = cmp->Compress(level,
				(filter ? filterBuffer : srcData + srcOffset),
				thisBlockSize / tf.vertexStride,
				tf.vertexStride,
				thisCmpSize);
			if (cmpOffset + thisCmpSize > dataSize)
			{
				// data is not compressible; fallback to just zero indicator + memcpy
				*(uint32_t*)compressed = 0;
				memcpy(compressed + 4, srcData, dataSize);
				outCompressedSize = dataSize + 4;
				delete[] filterBuffer;
				delete[] thisCmp;
				return compressed;
			}
			// store this chunk size and data
			*(uint32_t*)(compressed + cmpOffset) = uint32_t(thisCmpSize);
			memcpy(compressed + cmpOffset + 4, thisCmp, thisCmpSize);
			delete[] thisCmp;

			srcOffset += blockSize;
			cmpOffset += 4 + thisCmpSize;
		}
		delete[] filterBuffer;
		outCompressedSize = cmpOffset;
		return compressed;
	}

	void DecompressWhole(const TestFile& tf, const uint8_t* compressed, size_t compressedSize, uint8_t* dst)
	{
		uint8_t* filterBuffer = nullptr;
		if (filter)
			filterBuffer = new uint8_t[tf.fileData.size()];
		cmp->Decompress(compressed, compressedSize, filter == nullptr ? dst : filterBuffer, tf.vertexCount, tf.vertexStride);

		if (filter)
		{
			filter->unfilterFunc(filterBuffer, dst, tf.vertexStride, tf.vertexCount);
			delete[] filterBuffer;
		}
	}

	void Decompress(const TestFile& tf, const uint8_t* compressed, size_t compressedSize, uint8_t* dst)
	{
		if (blockSizeEnum == kBSizeNone)
		{
			DecompressWhole(tf, compressed, compressedSize, dst);
			return;
		}

		uint32_t firstBlockCmpSize = *(const uint32_t*)compressed;
		if (firstBlockCmpSize == 0)
		{
			// it was uncompressible data fallback
			memcpy(dst, compressed + 4, tf.vertexCount * tf.vertexStride);
			return;
		}

		size_t blockSize = kBlockSizeToActualSize[blockSizeEnum];
		// make sure multiple of data elem size
		blockSize = (blockSize / tf.vertexStride) * tf.vertexStride;

		uint8_t* filterBuffer = nullptr;
		if (filter)
			filterBuffer = new uint8_t[blockSize];

		uint8_t* dstData = dst;
		const size_t dataSize = tf.fileData.size();
		
		size_t cmpOffset = 0;
		size_t dstOffset = 0;
		while (cmpOffset < compressedSize)
		{
			size_t thisBlockSize = std::min(blockSize, dataSize - dstOffset);

			uint32_t thisCmpSize = *(const uint32_t*)(compressed + cmpOffset);
			cmp->Decompress(compressed + cmpOffset + 4, thisCmpSize, (filter == nullptr ? dstData + dstOffset : filterBuffer), thisBlockSize / tf.vertexStride, tf.vertexStride);

			if (filter)
				filter->unfilterFunc(filterBuffer, dstData + dstOffset, tf.vertexStride, thisBlockSize / tf.vertexStride);

			cmpOffset += 4 + thisCmpSize;
			dstOffset += thisBlockSize;
		}
		delete[] filterBuffer;
	}
};

static std::vector<CompressorConfig> g_Compressors;

static void TestCompressors(size_t testFileCount, TestFile* testFiles)
{
	//g_Compressors.push_back({ g_CompZstd.get(), &g_FilterByteDelta, kBSize1M });
	//g_Compressors.push_back({ g_CompLZ4.get(), &g_FilterByteDelta, kBSize1M });

	//g_Compressors.push_back({ g_CompZstd.get(), &g_FilterByteDelta });
	g_Compressors.push_back({ g_CompLZ4.get(), &g_FilterByteDelta });

	//g_Compressors.push_back({ g_CompZstd.get() });
	g_Compressors.push_back({ g_CompLZ4.get() });
	//g_Compressors.push_back({ g_CompMeshOpt.get() }); //@TODO: fails with packed data

	size_t maxSize = 0, totalPackedSize = 0, totalOrigSize = 0;
	for (int tfi = 0; tfi < testFileCount; ++tfi)
	{
		size_t size = testFiles[tfi].fileData.size();
		maxSize = std::max(maxSize, size);
		totalOrigSize += testFiles[tfi].origFileData.size();
		totalPackedSize += size;
	}

	std::vector<uint8_t> decompressed(maxSize);

	struct Result
	{
		int level = 0;
		size_t size = 0;
		double cmpTime = 0;
		double decTime = 0;
	};
	typedef std::vector<Result> LevelResults;
	std::vector<LevelResults> results;
	for (auto& cmp : g_Compressors)
	{
		auto levels = cmp.cmp->GetLevels();
		LevelResults res(levels.size());
		for (size_t i = 0; i < levels.size(); ++i)
			res[i].level = levels[i];
		results.emplace_back(res);
	}

	std::string cmpName;
	for (int ir = 0; ir < kRuns; ++ir)
	{
		printf("Run %i/%i, %zi compressors on %zi files:\n", ir+1, kRuns, g_Compressors.size(), testFileCount);
		for (size_t ic = 0; ic < g_Compressors.size(); ++ic)
		{
			auto& config = g_Compressors[ic];
			cmpName = config.GetName();
			LevelResults& levelRes = results[ic];
			printf("%s: %zi levels:\n", cmpName.c_str(), levelRes.size());
			for (Result& res : levelRes)
			{
				printf(".");
				for (int tfi = 0; tfi < testFileCount; ++tfi)
				{
					const TestFile& tf = testFiles[tfi];

					const uint8_t* srcData = tf.fileData.data();
					SysInfoFlushCaches();

					// compress
					uint64_t t0 = stm_now();
					size_t compressedSize = 0;
					uint8_t* compressed = config.Compress(tf, res.level, compressedSize);
					double tComp = stm_sec(stm_since(t0));

					// decompress
					memset(decompressed.data(), 0, tf.fileData.size());
					SysInfoFlushCaches();
					t0 = stm_now();
					config.Decompress(tf, compressed, compressedSize, decompressed.data());
					double tDecomp = stm_sec(stm_since(t0));

					// stats
					res.size += compressedSize;
					res.cmpTime += tComp;
					res.decTime += tDecomp;

					// check validity
					if (memcmp(tf.fileData.data(), decompressed.data(), tf.fileData.size()) != 0)
					{
						printf("  ERROR, %s level %i did not decompress back to input on %s\n", cmpName.c_str(), res.level, tf.path);
						for (size_t i = 0; i < tf.fileData.size(); ++i)
						{
							uint8_t va = tf.fileData[i];
							uint8_t vb = decompressed[i];
							if (va != vb)
							{
								printf("    diff at #%zi: exp %i got %i\n", i, va, vb);
								break;
							}
						}
						exit(1);
					}
					delete[] compressed;
				}
			}
			printf("\n");
		}
		printf("\n");
	}

	// normalize results
	int counterRan = 0;
	for (size_t ic = 0; ic < g_Compressors.size(); ++ic)
	{
		Compressor* cmp = g_Compressors[ic].cmp;
		LevelResults& levelRes = results[ic];
		for (Result& res : levelRes)
		{
			res.size /= kRuns;
			res.cmpTime /= kRuns;
			res.decTime /= kRuns;
			++counterRan;
		}
	}
	printf("  Ran %i cases\n", counterRan);


	double oneMB = 1024.0 * 1024.0;
	double oneGB = oneMB * 1024.0;
	double fullSize = (double)totalOrigSize;
	double packedSize = (double)totalPackedSize;
	// print results to screen
	printf("Compressor     SizeGB CTimeS  DTimeS   Ratio   CGB/s   DGB/s\n");
	printf("%12s %7.3f\n", "Full", fullSize / oneGB);
	printf("%12s %7.3f\n", "Packed", packedSize / oneGB);
	for (size_t ic = 0; ic < g_Compressors.size(); ++ic)
	{
		cmpName = g_Compressors[ic].GetName();
		const LevelResults& levelRes = results[ic];
		for (const Result& res : levelRes)
		{
			char nameBuf[1000];
			if (levelRes.size() == 1)
				snprintf(nameBuf, sizeof(nameBuf), "%s", cmpName.c_str());
			else if (res.level < 0)
				snprintf(nameBuf, sizeof(nameBuf), "%s_n%i", cmpName.c_str(), abs(res.level));
			else
				snprintf(nameBuf, sizeof(nameBuf), "%s_%i", cmpName.c_str(), abs(res.level));
			double csize = (double)res.size;
			double ctime = res.cmpTime;
			double dtime = res.decTime;
			double ratio = packedSize / csize;
			double cspeed = packedSize / ctime;
			double dspeed = packedSize / dtime;
			printf("%12s %7.3f %7.3f %7.3f %7.3f %7.3f %7.3f\n", nameBuf, csize/ oneGB, ctime, dtime, ratio, cspeed/oneGB, dspeed/oneGB);
		}
	}

	// cleanup
	g_Compressors.clear();
}

static bool ReadPlyFile(const char* path, std::vector<uint8_t>& dst, size_t& outVertexCount, size_t& outStride)
{
	FILE* f = fopen(path, "rb");
	if (f == nullptr)
	{
		printf("ERROR: failed to open data file %s\n", path);
		return false;
	}
	// read header
	int vertexCount = 0;
	int vertexStride = 0;
	char lineBuf[1024], propType[1024], propName[1024];
	while (true)
	{
		lineBuf[0] = 0;
		fgets(lineBuf, sizeof(lineBuf), f);
		if (0 == strncmp(lineBuf, "end_header", 10))
			break;
		// parse vertex count
		if (1 == sscanf(lineBuf, "element vertex %i", &vertexCount))
		{
			// ok
		}
		// property
		propType[0] = 0;
		propName[0] = 0;
		if (2 == sscanf(lineBuf, "property %s %s", propType, propName))
		{
			if (0 == strcmp(propType, "float")) vertexStride += 4;
			if (0 == strcmp(propType, "double")) vertexStride += 8;
			if (0 == strcmp(propType, "uchar")) vertexStride += 1;
		}
	}

	//printf("PLY file %s: %i verts, %i stride\n", path, vertexCount, vertexStride);

	const size_t kStride = 248;
	if (vertexStride != kStride)
	{
		printf("ERROR: expect vertex stride %zi, file %s had %i\n", kStride, path, vertexStride);
		return false;
	}
	dst.resize(vertexCount * kStride);
	fread(dst.data(), kStride, vertexCount, f);

	outVertexCount = vertexCount;
	outStride = vertexStride;
	return true;
}

// Based on https://fgiesen.wordpress.com/2009/12/13/decoding-morton-codes/
//
// "Insert" two 0 bits after each of the 21 low bits of x
static uint64_t MortonPart1By2(uint64_t x)
{
	x &= 0x1fffff;
	x = (x ^ (x << 32)) & 0x1f00000000ffffull;
	x = (x ^ (x << 16)) & 0x1f0000ff0000ffull;
	x = (x ^ (x << 8)) & 0x100f00f00f00f00full;
	x = (x ^ (x << 4)) & 0x10c30c30c30c30c3ull;
	x = (x ^ (x << 2)) & 0x1249249249249249ull;
	return x;
}
// Encode three 21-bit integers into 3D Morton order
static uint64_t MortonEncode3(uint64_t x, uint64_t y, uint64_t z)
{
	return (MortonPart1By2(z) << 2) | (MortonPart1By2(y) << 1) | MortonPart1By2(x);
}

static void ReorderData(TestFile& tf)
{
	assert(tf.vertexStride == kFullVertexStride);

	// The order of data points does not matter: arrange them in 3D Morton order,
	// both for better data delta locality, and for better runtime access
	// (neighboring points would likely get fetched together).

	// Find bounding box of positions
	float bmin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
	float bmax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
	const float* posData = (const float*)tf.fileData.data();
	for (size_t i = 0, dataIdx = 0; i < tf.vertexCount; ++i, dataIdx += tf.vertexStride / 4)
	{
		float x = posData[dataIdx + 0], y = posData[dataIdx + 1], z = posData[dataIdx + 2];
		bmin[0] = std::min(bmin[0], x);
		bmin[1] = std::min(bmin[1], y);
		bmin[2] = std::min(bmin[2], z);
		bmax[0] = std::max(bmin[0], x);
		bmax[1] = std::max(bmax[1], y);
		bmax[2] = std::max(bmax[2], z);
	}
	printf("- %s bounds %.2f,%.2f,%.2f .. %.2f,%.2f,%.2f\n", tf.title, bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2]);

	// Compute Morton codes for the positions, and sort by them
	std::vector<std::pair<uint64_t, size_t>> remap(tf.vertexCount);
	const float kScaler = float((1<<21)-1);
	for (size_t i = 0, dataIdx = 0; i < tf.vertexCount; ++i, dataIdx += tf.vertexStride / 4)
	{
		float x = (posData[dataIdx + 0] - bmin[0]) / (bmax[0] - bmin[0]) * kScaler;
		float y = (posData[dataIdx + 1] - bmin[1]) / (bmax[1] - bmin[1]) * kScaler;
		float z = (posData[dataIdx + 2] - bmin[2]) / (bmax[2] - bmin[2]) * kScaler;
		uint32_t ix = (uint32_t)x;
		uint32_t iy = (uint32_t)y;
		uint32_t iz = (uint32_t)z;
		uint64_t code = MortonEncode3(ix, iy, iz);
		remap[i] = { code, i };
	}
	std::sort(remap.begin(), remap.end(), [](const auto& a, const auto& b)
	{
		if (a.first != b.first)
			return a.first < b.first;
		return a.second < b.second;
	});

	// Reorder the data
	std::vector<uint8_t> dst(tf.fileData.size());
	for (size_t i = 0, idx = 0; i < tf.vertexCount; ++i)
	{
		size_t srcIdx = remap[i].second * tf.vertexStride;
		for (size_t j = 0; j < tf.vertexStride; ++j)
			dst[idx + j] = tf.fileData[srcIdx + j];
		idx += tf.vertexStride;
	}

	// Reverse reorder the data and check if it matches the source
	std::vector<uint8_t> check(tf.fileData.size());
	for (size_t i = 0; i < tf.vertexCount; ++i)
	{
		size_t srcIdx = i * tf.vertexStride;
		size_t dstIdx = remap[i].second * tf.vertexStride;
		for (size_t j = 0; j < tf.vertexStride; ++j)
			check[dstIdx + j] = dst[srcIdx + j];
	}
	if (0 != memcmp(tf.fileData.data(), check.data(), check.size()))
	{
		printf("ERROR in Morton3D remapping of %s\n", tf.title);
	}

	tf.fileData.swap(dst);
}

static void NormalizeRotation(TestFile& tf)
{
	assert(tf.vertexStride == kFullVertexStride);
	FullVertex* data = (FullVertex*)tf.fileData.data();
	for (size_t i = 0; i < tf.vertexCount; ++i)
	{
		float x = data->rx;
		float y = data->ry;
		float z = data->rz;
		float w = data->rw;
		float len = sqrtf(x * x + y * y + z * z + w * w);
		x /= len; y /= len; z /= len; w /= len;
		data->rx = x;
		data->ry = y;
		data->rz = z;
		data->rw = w;
		data++;
	}
}

static float Sigmoid(float v)
{
	return 1.0f / (1.0f + expf(-v));
}

static float InvSigmoid(float v)
{
	return logf(v / (std::max(1.0f - v, 1.0e-6f)));
}


static void LinearizeData(TestFile& tf)
{
	assert(tf.vertexStride == kFullVertexStride);
	FullVertex* data = (FullVertex*)tf.fileData.data();
	for (size_t i = 0; i < tf.vertexCount; ++i)
	{
		// opacity
		data->opacity = Sigmoid(data->opacity);
		// scale
		{
			data->sx = expf(data->sx);
			data->sy = expf(data->sy);
			data->sz = expf(data->sz);

			data->sx = sqrtf(data->sx);
			data->sy = sqrtf(data->sy);
			data->sz = sqrtf(data->sz);
			data->sx = sqrtf(data->sx);
			data->sy = sqrtf(data->sy);
			data->sz = sqrtf(data->sz);
		}
		data++;
	}
}

static void UnlinearizeData(TestFile& tf)
{
	assert(tf.vertexStride == kFullVertexStride);
	FullVertex* data = (FullVertex*)tf.fileData.data();
	for (size_t i = 0; i < tf.vertexCount; ++i)
	{
		// opacity
		data->opacity = InvSigmoid(data->opacity);
		// scale
		{
			data->sx *= data->sx;
			data->sy *= data->sy;
			data->sz *= data->sz;
			data->sx *= data->sx;
			data->sy *= data->sy;
			data->sz *= data->sz;

			data->sx = logf(data->sx);
			data->sy = logf(data->sy);
			data->sz = logf(data->sz);
		}
		data++;
	}
}


static void CalcMinMax(TestFile& tf)
{
	assert(tf.vertexStride == kFullVertexStride);
	float* valMax = (float*)&tf.valMax;
	float* valMin = (float*)&tf.valMin;
	for (int i = 0; i < kFullVertexFloats; ++i)
	{
		valMax[i] = -FLT_MAX;
		valMin[i] = FLT_MAX;
	}
	float* data = (float*)tf.fileData.data();
	for (size_t i = 0; i < tf.vertexCount; ++i)
	{
		for (int j = 0; j < kFullVertexFloats; ++j)
		{
			float val = *data++;
			valMax[j] = std::max(valMax[j], val);
			valMin[j] = std::min(valMin[j], val);
		}
	}
}

struct PackedVertex
{
	uint16_t px, py, pz;
	uint16_t dcr, dcg, dcb;
	uint16_t shr[15];
	uint16_t shg[15];
	uint16_t shb[15];
	uint16_t opacity;
	uint16_t sx, sy, sz;
	uint16_t rx, ry, rz, rw;
};
constexpr size_t kPackedVertexSize = sizeof(PackedVertex);

static uint32_t Pack16(float vmin, float vmax, float v)
{
	v = (v - vmin) / (vmax - vmin);
	return meshopt_quantizeUnorm(v, 16);
}

static float Unpack16(float vmin, float vmax, uint32_t u)
{
	float v = float(u) / float((1 << 16) - 1);
	return vmin * (1 - v) + vmax * v;
}

#define PACK_POS(field)		dst->field = Pack16(tf.valMin.field, tf.valMax.field, src->field)
#define PACK_DC(field)		dst->field = Pack16(tf.valMin.field, tf.valMax.field, src->field)
#define PACK_OP(field)		dst->field = Pack16(tf.valMin.field, tf.valMax.field, src->field)
#define PACK_SCALE(field)	dst->field = Pack16(tf.valMin.field, tf.valMax.field, src->field)
#define PACK_ROT(field)		dst->field = Pack16(tf.valMin.field, tf.valMax.field, src->field)

#define UNPACK_POS(field)	dst->field = Unpack16(tf.valMin.field, tf.valMax.field, src->field)
#define UNPACK_DC(field)	dst->field = Unpack16(tf.valMin.field, tf.valMax.field, src->field)
#define UNPACK_OP(field)	dst->field = Unpack16(tf.valMin.field, tf.valMax.field, src->field)
#define UNPACK_SCALE(field)	dst->field = Unpack16(tf.valMin.field, tf.valMax.field, src->field)
#define UNPACK_ROT(field)	dst->field = Unpack16(tf.valMin.field, tf.valMax.field, src->field)


static void PackData(TestFile& tf)
{
	assert(tf.vertexStride == kFullVertexStride);
	std::vector<uint8_t> dstData(tf.vertexCount * kPackedVertexSize);

	const FullVertex* src = (const FullVertex*)tf.fileData.data();
	PackedVertex* dst = (PackedVertex*)dstData.data();
	for (size_t i = 0; i < tf.vertexCount; ++i)
	{

		PACK_POS(px); PACK_POS(py); PACK_POS(pz);
		PACK_DC(dcr); PACK_DC(dcg); PACK_DC(dcb);
		for (int j = 0; j < 15; ++j)
		{
			dst->shr[j] = Pack16(tf.valMin.shr[j], tf.valMax.shr[j], src->shr[j]);
			dst->shg[j] = Pack16(tf.valMin.shg[j], tf.valMax.shg[j], src->shg[j]);
			dst->shb[j] = Pack16(tf.valMin.shb[j], tf.valMax.shb[j], src->shb[j]);
		}
		PACK_OP(opacity);
		PACK_SCALE(sx); PACK_SCALE(sy); PACK_SCALE(sz);
		PACK_ROT(rx); PACK_ROT(ry); PACK_ROT(rz); PACK_ROT(rw);
		++src;
		++dst;
	}

	tf.fileData.swap(dstData);
	tf.vertexStride = kPackedVertexSize;
}

static void UnpackData(TestFile& tf)
{
	assert(tf.vertexStride == kPackedVertexSize);
	std::vector<uint8_t> dstData(tf.vertexCount * kFullVertexStride);

	const PackedVertex* src = (const PackedVertex*)tf.fileData.data();
	FullVertex* dst = (FullVertex*)dstData.data();
	for (size_t i = 0; i < tf.vertexCount; ++i)
	{

		UNPACK_POS(px); UNPACK_POS(py); UNPACK_POS(pz);
		UNPACK_DC(dcr); UNPACK_DC(dcg); UNPACK_DC(dcb);
		for (int j = 0; j < 15; ++j)
		{
			dst->shr[j] = Unpack16(tf.valMin.shr[j], tf.valMax.shr[j], src->shr[j]);
			dst->shg[j] = Unpack16(tf.valMin.shg[j], tf.valMax.shg[j], src->shg[j]);
			dst->shb[j] = Unpack16(tf.valMin.shb[j], tf.valMax.shb[j], src->shb[j]);
		}
		UNPACK_OP(opacity);
		UNPACK_SCALE(sx); UNPACK_SCALE(sy); UNPACK_SCALE(sz);
		UNPACK_ROT(rx); UNPACK_ROT(ry); UNPACK_ROT(rz); UNPACK_ROT(rw);
		++src;
		++dst;
	}

	tf.fileData.swap(dstData);
	tf.vertexStride = kFullVertexStride;
}

#undef PACK_POS
#undef PACK_DC
#undef PACK_OP
#undef PACK_SCALE
#undef PACK_ROT

#undef UNPACK_POS
#undef UNPACK_DC
#undef UNPACK_OP
#undef UNPACK_SCALE
#undef UNPACK_ROT

static void QuatConjugate(const float q[4], float r[4])
{
	r[0] = -q[0];
	r[1] = -q[1];
	r[2] = -q[2];
	r[3] =  q[3];
}

static void QuatMul(const float a[4], const float b[4], float r[4])
{
	r[0] = a[3] * b[0] + (a[0] * b[3] + a[1] * b[2]) - a[2] * b[1];
	r[1] = a[3] * b[1] + (a[1] * b[3] + a[2] * b[0]) - a[0] * b[2];
	r[2] = a[3] * b[2] + (a[2] * b[3] + a[0] * b[1]) - a[1] * b[0];
	r[3] = a[3] * b[3] - (a[3] * b[0] + a[1] * b[1]) - a[2] * b[2];
}

static void QuatNormalize(float q[4])
{
	float lensq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	float len = 1.0f / lensq;
	q[0] /= len; q[1] /= len; q[2] /= len; q[3] /= len;
}

static float QuatAngleBetween(const float q1[4], const float q2[4])
{
	float q1c[4];
	QuatConjugate(q1, q1c);
	float qm[4];
	QuatMul(q1c, q2, qm);
	QuatNormalize(qm);

	float vecLenSq = qm[0] * qm[0] + qm[1] * qm[1] + qm[2] * qm[2];
	float a = asinf(sqrtf(vecLenSq));
	return a * 2;
}

static void CalcErrorFromOrig(TestFile& tf)
{
	assert(tf.vertexStride == kFullVertexStride);
	memset(&tf.errMax, 0, sizeof(tf.errMax));
	memset(&tf.errAvg, 0, sizeof(tf.errAvg));
	const float* src1 = (const float*)tf.origFileData.data();
	const float* src2 = (const float*)tf.fileData.data();
	float* errSumPtr = (float*)&tf.errAvg;
	float* errMaxPtr = (float*)&tf.errMax;
	float errRotSum = 0;
	float errRotMax = 0;
	for (size_t i = 0; i < tf.vertexCount; ++i)
	{
		for (int j = 0; j < kFullVertexFloats; ++j)
		{
			float diff = fabsf(*src1 - *src2);
			errSumPtr[j] += diff;
			errMaxPtr[j] = std::max(errMaxPtr[j], diff);
			++src1;
			++src2;
		}
		// evaluate rotation error
		{
			const FullVertex& v1 = ((const FullVertex*)tf.origFileData.data())[i];
			const FullVertex& v2 = ((const FullVertex*)tf.fileData.data())[i];
			float q1[4] = { v1.rx, v1.ry, v1.rz, v1.rw };
			float q2[4] = { v2.rx, v2.ry, v2.rz, v2.rw };
			float diff = QuatAngleBetween(q1, q2);
			errRotSum += diff;
			errRotMax = std::max(errRotMax, diff);
		}
	}
	for (int j = 0; j < kFullVertexFloats; ++j)
	{
		errSumPtr[j] /= tf.vertexCount;
	}
	float errRotAvg = errRotSum /= tf.vertexCount;

	float errPosMax = std::max(tf.errMax.px, std::max(tf.errMax.py, tf.errMax.pz));
	float errPosAvg = (tf.errAvg.px + tf.errAvg.py + tf.errAvg.pz) / 3.0f;
	float errDcMax = std::max(tf.errMax.dcr, std::max(tf.errMax.dcg, tf.errMax.dcb));
	float errDcAvg = (tf.errAvg.dcr + tf.errAvg.dcg + tf.errAvg.dcb) / 3.0f;
	float errScaleMax = std::max(tf.errMax.sx, std::max(tf.errMax.sy, tf.errMax.sz));
	float errScaleAvg = (tf.errAvg.sx + tf.errAvg.sy + tf.errAvg.sz) / 3.0f;

	printf("Packing error on %s:\n", tf.title);
	printf("  - pos avg %7.4f max %7.4f\n", errPosAvg, errPosMax);
	printf("  - rot avg %7.4f max %7.4f\n", errRotAvg, errRotMax);
	printf("  - scl avg %7.4f max %7.4f\n", errScaleAvg, errScaleMax);
	printf("  - col avg %7.4f max %7.4f\n", errDcAvg, errDcMax);
	printf("  - opa avg %7.4f max %7.4f\n", tf.errAvg.opacity, tf.errMax.opacity);
}



int main()
{
	stm_setup();
	printf("CPU: '%s' Compiler: '%s'\n", SysInfoGetCpuName().c_str(), SysInfoGetCompilerName().c_str());

	TestFile testFiles[] = {
#ifdef _DEBUG
		{"synthetic", "../../../../../Assets/Models~/synthetic/point_cloud/iteration_7000/point_cloud.ply"},
		{"bicycle_crop", "../../../../../Assets/Models~/bicycle_cropped/point_cloud/iteration_7000/point_cloud.ply"},
#else
		{"bicycle_7k", "../../../../../Assets/Models~/bicycle/point_cloud/iteration_7000/point_cloud.ply"},
		//{"bicycle_30k", "../../../../../Assets/Models~/bicycle/point_cloud/iteration_30000/point_cloud.ply"},
		//{"truck_7k", "../../../../../Assets/Models~/truck/point_cloud/iteration_7000/point_cloud.ply"},
#endif
	};
	for (auto& tf : testFiles)
	{
		if (!ReadPlyFile(tf.path, tf.fileData, tf.vertexCount, tf.vertexStride))
			return 1;
		ReorderData(tf);

		tf.origFileData = tf.fileData;
		NormalizeRotation(tf);
		LinearizeData(tf);
		CalcMinMax(tf);
		PackData(tf);
	}
	TestCompressors(std::size(testFiles), testFiles);
	for (auto& tf : testFiles)
	{
		UnpackData(tf);
		UnlinearizeData(tf);
		CalcErrorFromOrig(tf);
	}
	return 0;
}
