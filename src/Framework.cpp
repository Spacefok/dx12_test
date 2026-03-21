#include "Framework.hpp"
#include <DirectXColors.h>
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <cmath>
#include <string>
#include <wincodec.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "ufbx.h"

#include <filesystem>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <fstream>
#include <cwctype>
#include <string_view>

#if defined(_DEBUG)
#include <d3d12sdklayers.h>
#endif

using namespace DirectX;

#pragma comment(lib, "windowscodecs.lib")

namespace {
struct RgbaImage {
	UINT Width = 1;
	UINT Height = 1;
	std::vector<std::uint8_t> Pixels = { 255, 255, 255, 255 };
};

struct LoadedTextureData {
	UINT Width = 1;
	UINT Height = 1;
	UINT MipLevels = 1;
	DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	std::vector<std::uint8_t> Bytes = { 255, 255, 255, 255 };
	std::vector<D3D12_SUBRESOURCE_DATA> Subresources;
};

#pragma pack(push, 1)
struct DdsPixelFormat {
	std::uint32_t Size;
	std::uint32_t Flags;
	std::uint32_t FourCC;
	std::uint32_t RgbBitCount;
	std::uint32_t RBitMask;
	std::uint32_t GBitMask;
	std::uint32_t BBitMask;
	std::uint32_t ABitMask;
};

struct DdsHeader {
	std::uint32_t Size;
	std::uint32_t Flags;
	std::uint32_t Height;
	std::uint32_t Width;
	std::uint32_t PitchOrLinearSize;
	std::uint32_t Depth;
	std::uint32_t MipMapCount;
	std::uint32_t Reserved1[11];
	DdsPixelFormat Ddspf;
	std::uint32_t Caps;
	std::uint32_t Caps2;
	std::uint32_t Caps3;
	std::uint32_t Caps4;
	std::uint32_t Reserved2;
};

struct DdsHeaderDx10 {
	std::uint32_t DxgiFormat;
	std::uint32_t ResourceDimension;
	std::uint32_t MiscFlag;
	std::uint32_t ArraySize;
	std::uint32_t MiscFlags2;
};
#pragma pack(pop)

constexpr std::uint32_t MakeFourCc(char a, char b, char c, char d)
{
	return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a))
		| (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8)
		| (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16)
		| (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

constexpr std::uint32_t DdsMagic = MakeFourCc('D', 'D', 'S', ' ');
constexpr std::uint32_t DdsFourCcFlag = 0x00000004u;

RgbaImage MakeSolidImage(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
	RgbaImage image;
	image.Width = 1;
	image.Height = 1;
	image.Pixels = { r, g, b, a };
	return image;
}

bool LoadImageFromTga(const std::filesystem::path& imagePath, RgbaImage& outImage) {
#pragma pack(push, 1)
	struct TgaHeader {
		std::uint8_t idLength;
		std::uint8_t colorMapType;
		std::uint8_t imageType;
		std::uint16_t colorMapOrigin;
		std::uint16_t colorMapLength;
		std::uint8_t colorMapDepth;
		std::uint16_t xOrigin;
		std::uint16_t yOrigin;
		std::uint16_t width;
		std::uint16_t height;
		std::uint8_t pixelDepth;
		std::uint8_t imageDescriptor;
	};
#pragma pack(pop)

	std::ifstream file(imagePath, std::ios::binary);
	if (!file) {
		return false;
	}

	TgaHeader hdr = {};
	file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
	if (!file) {
		return false;
	}

	if (hdr.colorMapType != 0) {
		return false;
	}
	if (hdr.width == 0 || hdr.height == 0) {
		return false;
	}
	if (hdr.pixelDepth != 24 && hdr.pixelDepth != 32) {
		return false;
	}
	if (hdr.imageType != 2 && hdr.imageType != 10) {
		return false;
	}

	if (hdr.idLength > 0) {
		file.seekg(hdr.idLength, std::ios::cur);
		if (!file) {
			return false;
		}
	}

	const UINT width = hdr.width;
	const UINT height = hdr.height;
	const size_t bytesPerPixel = hdr.pixelDepth / 8;
	const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

	std::vector<std::uint8_t> srcData(pixelCount * bytesPerPixel);

	if (hdr.imageType == 2)
	{
		file.read(reinterpret_cast<char*>(srcData.data()), static_cast<std::streamsize>(srcData.size()));
		if (!file) {
			return false;
		}
	}
	else
	{
		size_t outPixel = 0;
		while (outPixel < pixelCount && file)
		{
			std::uint8_t packetHeader = 0;
			file.read(reinterpret_cast<char*>(&packetHeader), 1);
			if (!file) {
				return false;
			}

			const size_t runLength = static_cast<size_t>(packetHeader & 0x7F) + 1;
			if (runLength == 0 || outPixel + runLength > pixelCount) {
				return false;
			}

			if (packetHeader & 0x80)
			{
				std::array<std::uint8_t, 4> px = {};
				file.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(bytesPerPixel));
				if (!file) {
					return false;
				}

				for (size_t i = 0; i < runLength; ++i)
				{
					memcpy(srcData.data() + (outPixel + i) * bytesPerPixel, px.data(), bytesPerPixel);
				}
			}
			else
			{
				const size_t bytesToRead = runLength * bytesPerPixel;
				file.read(
					reinterpret_cast<char*>(srcData.data() + outPixel * bytesPerPixel),
					static_cast<std::streamsize>(bytesToRead));
				if (!file) {
					return false;
				}
			}

			outPixel += runLength;
		}

		if (outPixel != pixelCount) {
			return false;
		}
	}

	outImage.Width = width;
	outImage.Height = height;
	outImage.Pixels.resize(pixelCount * 4);

	const bool originTop = (hdr.imageDescriptor & 0x20) != 0;
	const bool originRight = (hdr.imageDescriptor & 0x10) != 0;

	for (UINT y = 0; y < height; ++y)
	{
		const UINT srcY = originTop ? y : (height - 1 - y);
		for (UINT x = 0; x < width; ++x)
		{
			const UINT srcX = originRight ? (width - 1 - x) : x;
			const size_t srcPixel = static_cast<size_t>(srcY) * width + srcX;
			const size_t srcIndex = srcPixel * bytesPerPixel;

			const size_t dstPixel = static_cast<size_t>(y) * width + x;
			const size_t dstIndex = dstPixel * 4;

			outImage.Pixels[dstIndex + 0] = srcData[srcIndex + 2];
			outImage.Pixels[dstIndex + 1] = srcData[srcIndex + 1];
			outImage.Pixels[dstIndex + 2] = srcData[srcIndex + 0];
			outImage.Pixels[dstIndex + 3] = (bytesPerPixel == 4) ? srcData[srcIndex + 3] : 255;
		}
	}

	return true;
}

bool LoadImageFromFileWic(const std::filesystem::path& imagePath, RgbaImage& outImage) {
	ComPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(
		CLSID_WICImagingFactory,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (FAILED(hr)) {
		return false;
	}

	ComPtr<IWICBitmapDecoder> decoder;
	hr = factory->CreateDecoderFromFilename(
		imagePath.wstring().c_str(),
		nullptr,
		GENERIC_READ,
		WICDecodeMetadataCacheOnLoad,
		&decoder);
	if (FAILED(hr)) {
		return false;
	}

	ComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr)) {
		return false;
	}

	ComPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(&converter);
	if (FAILED(hr)) {
		return false;
	}

	hr = converter->Initialize(
		frame.Get(),
		GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone,
		nullptr,
		0.0f,
		WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) {
		return false;
	}

	UINT width = 0;
	UINT height = 0;
	hr = converter->GetSize(&width, &height);
	if (FAILED(hr) || width == 0 || height == 0) {
		return false;
	}

	const UINT rowPitch = width * 4;
	outImage.Width = width;
	outImage.Height = height;
	outImage.Pixels.resize(static_cast<size_t>(rowPitch) * static_cast<size_t>(height));

	hr = converter->CopyPixels(nullptr, rowPitch, static_cast<UINT>(outImage.Pixels.size()), outImage.Pixels.data());
	if (FAILED(hr)) {
		return false;
	}

	return true;
}

bool LoadImageFromFile(const std::filesystem::path& imagePath, RgbaImage& outImage)
{
	if (LoadImageFromFileWic(imagePath, outImage)) {
		return true;
	}

	std::wstring ext = imagePath.extension().wstring();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

	if (ext == L".tga") {
		return LoadImageFromTga(imagePath, outImage);
	}

	return false;
}

LoadedTextureData MakeTextureDataFromRgbaImage(const RgbaImage& image)
{
	LoadedTextureData texture;
	texture.Width = image.Width;
	texture.Height = image.Height;
	texture.MipLevels = 1;
	texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texture.Bytes = image.Pixels;
	texture.Subresources.push_back({
		texture.Bytes.data(),
		static_cast<LONG_PTR>(texture.Width * 4),
		static_cast<LONG_PTR>(texture.Bytes.size())
	});
	return texture;
}

bool IsBlockCompressedFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

UINT BlockCompressedBlockSize(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		return 8;

	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return 16;

	default:
		return 0;
	}
}

UINT BytesPerPixel(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return 4;
	default:
		return 0;
	}
}

bool ComputeSurfaceInfo(
	UINT width,
	UINT height,
	DXGI_FORMAT format,
	size_t& outRowBytes,
	size_t& outSliceBytes,
	UINT& outNumRows)
{
	if (width == 0 || height == 0) {
		return false;
	}

	if (IsBlockCompressedFormat(format))
	{
		const UINT blockSize = BlockCompressedBlockSize(format);
		if (blockSize == 0) {
			return false;
		}

		const size_t blocksWide = (std::max<UINT>(1u, width) + 3u) / 4u;
		const size_t blocksHigh = (std::max<UINT>(1u, height) + 3u) / 4u;
		outRowBytes = blocksWide * blockSize;
		outNumRows = static_cast<UINT>(blocksHigh);
		outSliceBytes = outRowBytes * blocksHigh;
		return true;
	}

	const UINT bpp = BytesPerPixel(format);
	if (bpp == 0) {
		return false;
	}

	outRowBytes = static_cast<size_t>(width) * bpp;
	outNumRows = height;
	outSliceBytes = outRowBytes * height;
	return true;
}

bool ResolveLegacyDdsFormat(const DdsPixelFormat& ddspf, DXGI_FORMAT& outFormat)
{
	if ((ddspf.Flags & DdsFourCcFlag) == 0) {
		return false;
	}

	switch (ddspf.FourCC)
	{
	case MakeFourCc('D', 'X', 'T', '1'):
		outFormat = DXGI_FORMAT_BC1_UNORM;
		return true;

	case MakeFourCc('D', 'X', 'T', '5'):
		outFormat = DXGI_FORMAT_BC3_UNORM;
		return true;

	case MakeFourCc('A', 'T', 'I', '2'):
	case MakeFourCc('B', 'C', '5', 'U'):
		outFormat = DXGI_FORMAT_BC5_UNORM;
		return true;

	case MakeFourCc('D', 'X', '1', '0'):
		return true;

	default:
		return false;
	}
}

bool IsSupportedDdsFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return true;
	default:
		return false;
	}
}

bool LoadTextureDataFromDds(const std::filesystem::path& imagePath, LoadedTextureData& outTexture)
{
	std::ifstream file(imagePath, std::ios::binary);
	if (!file) {
		return false;
	}

	file.seekg(0, std::ios::end);
	const std::streamoff fileSize = file.tellg();
	file.seekg(0, std::ios::beg);
	if (fileSize < static_cast<std::streamoff>(sizeof(std::uint32_t) + sizeof(DdsHeader))) {
		return false;
	}

	std::vector<std::uint8_t> fileBytes(static_cast<size_t>(fileSize));
	file.read(reinterpret_cast<char*>(fileBytes.data()), static_cast<std::streamsize>(fileBytes.size()));
	if (!file) {
		return false;
	}

	const auto* magic = reinterpret_cast<const std::uint32_t*>(fileBytes.data());
	if (*magic != DdsMagic) {
		return false;
	}

	const auto* header = reinterpret_cast<const DdsHeader*>(fileBytes.data() + sizeof(std::uint32_t));
	if (header->Size != sizeof(DdsHeader) || header->Ddspf.Size != sizeof(DdsPixelFormat)) {
		return false;
	}

	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	if (!ResolveLegacyDdsFormat(header->Ddspf, format)) {
		return false;
	}

	size_t dataOffset = sizeof(std::uint32_t) + sizeof(DdsHeader);
	if (header->Ddspf.FourCC == MakeFourCc('D', 'X', '1', '0'))
	{
		if (fileBytes.size() < dataOffset + sizeof(DdsHeaderDx10)) {
			return false;
		}

		const auto* headerDx10 = reinterpret_cast<const DdsHeaderDx10*>(fileBytes.data() + dataOffset);
		format = static_cast<DXGI_FORMAT>(headerDx10->DxgiFormat);
		dataOffset += sizeof(DdsHeaderDx10);
	}

	if (!IsSupportedDdsFormat(format)) {
		return false;
	}

	const UINT mipLevels = (header->MipMapCount > 0) ? header->MipMapCount : 1u;
	if (header->Width == 0 || header->Height == 0 || mipLevels == 0) {
		return false;
	}

	outTexture = {};
	outTexture.Width = header->Width;
	outTexture.Height = header->Height;
	outTexture.MipLevels = mipLevels;
	outTexture.Format = format;
	outTexture.Bytes = std::move(fileBytes);
	outTexture.Subresources.clear();
	outTexture.Subresources.reserve(mipLevels);

	size_t subresourceOffset = dataOffset;
	UINT mipWidth = outTexture.Width;
	UINT mipHeight = outTexture.Height;

	for (UINT mipIndex = 0; mipIndex < mipLevels; ++mipIndex)
	{
		size_t rowBytes = 0;
		size_t sliceBytes = 0;
		UINT numRows = 0;
		if (!ComputeSurfaceInfo(mipWidth, mipHeight, format, rowBytes, sliceBytes, numRows)) {
			return false;
		}

		if (subresourceOffset + sliceBytes > outTexture.Bytes.size()) {
			return false;
		}

		outTexture.Subresources.push_back({
			outTexture.Bytes.data() + subresourceOffset,
			static_cast<LONG_PTR>(rowBytes),
			static_cast<LONG_PTR>(sliceBytes)
		});

		subresourceOffset += sliceBytes;
		mipWidth = std::max<UINT>(1u, mipWidth >> 1u);
		mipHeight = std::max<UINT>(1u, mipHeight >> 1u);
	}

	return true;
}

bool LoadTextureDataFromFile(const std::filesystem::path& imagePath, LoadedTextureData& outTexture)
{
	std::wstring ext = imagePath.extension().wstring();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

	if (ext == L".dds" && LoadTextureDataFromDds(imagePath, outTexture)) {
		return true;
	}

	RgbaImage image;
	if (!LoadImageFromFile(imagePath, image)) {
		return false;
	}

	outTexture = MakeTextureDataFromRgbaImage(image);
	return true;
}

std::string ToLowerCopy(std::string value)
{
	std::transform(
		value.begin(),
		value.end(),
		value.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

bool ContainsAny(std::string_view value, std::initializer_list<std::string_view> needles)
{
	for (std::string_view needle : needles) {
		if (value.find(needle) != std::string_view::npos) {
			return true;
		}
	}
	return false;
}

bool IsWindMaterial(std::string_view lowerName)
{
	return ContainsAny(lowerName, { "leaf", "foliage", "ivy", "plant", "tree", "bush" });
}

bool IsTransparentMaterial(std::string_view lowerName)
{
	return ContainsAny(lowerName, { "glass", "water", "ice", "wine", "beer", "transparent", "bottle" });
}

bool IsEmissiveMaterial(std::string_view lowerName)
{
	return ContainsAny(lowerName, { "emissive", "lamp", "light", "bulb", "lantern", "spotlight" });
}

DirectX::XMFLOAT3 NormalizeOrFallback(const DirectX::XMFLOAT3& value, const DirectX::XMFLOAT3& fallback)
{
	using namespace DirectX;

	const XMVECTOR v = XMLoadFloat3(&value);
	const float lengthSq = XMVectorGetX(XMVector3LengthSq(v));
	if (lengthSq <= 1e-8f) {
		return fallback;
	}

	DirectX::XMFLOAT3 result;
	XMStoreFloat3(&result, XMVector3Normalize(v));
	return result;
}

DirectX::XMFLOAT3 BuildFallbackTangent(const DirectX::XMFLOAT3& normal)
{
	using namespace DirectX;

	const XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&normal));
	XMVECTOR helper = (std::fabs(normal.y) < 0.999f)
		? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
		: XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);

	DirectX::XMFLOAT3 tangent;
	XMStoreFloat3(&tangent, XMVector3Normalize(XMVector3Cross(helper, n)));
	return tangent;
}

void ComputeTriangleNormalAndTangent(Vertex& v0, Vertex& v1, Vertex& v2)
{
	using namespace DirectX;

	const XMVECTOR p0 = XMLoadFloat3(&v0.Pos);
	const XMVECTOR p1 = XMLoadFloat3(&v1.Pos);
	const XMVECTOR p2 = XMLoadFloat3(&v2.Pos);

	const XMVECTOR edge1 = p1 - p0;
	const XMVECTOR edge2 = p2 - p0;
	XMVECTOR faceNormal = XMVector3Cross(edge1, edge2);
	if (XMVectorGetX(XMVector3LengthSq(faceNormal)) <= 1e-8f) {
		faceNormal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	}
	faceNormal = XMVector3Normalize(faceNormal);

	XMFLOAT3 normal;
	XMStoreFloat3(&normal, faceNormal);

	auto FixNormal = [&](Vertex& vertex)
	{
		const XMVECTOR n = XMLoadFloat3(&vertex.Normal);
		if (XMVectorGetX(XMVector3LengthSq(n)) <= 1e-8f) {
			vertex.Normal = normal;
		}
		else {
			vertex.Normal = NormalizeOrFallback(vertex.Normal, normal);
		}
	};

	FixNormal(v0);
	FixNormal(v1);
	FixNormal(v2);

	const float du1 = v1.TexC.x - v0.TexC.x;
	const float dv1 = v1.TexC.y - v0.TexC.y;
	const float du2 = v2.TexC.x - v0.TexC.x;
	const float dv2 = v2.TexC.y - v0.TexC.y;
	const float denom = du1 * dv2 - dv1 * du2;

	XMFLOAT3 tangent = BuildFallbackTangent(normal);
	if (std::fabs(denom) > 1e-8f)
	{
		const float invDenom = 1.0f / denom;
		XMVECTOR tangentV = (edge1 * dv2 - edge2 * dv1) * invDenom;
		if (XMVectorGetX(XMVector3LengthSq(tangentV)) > 1e-8f) {
			XMStoreFloat3(&tangent, XMVector3Normalize(tangentV));
		}
	}

	v0.Tangent = tangent;
	v1.Tangent = tangent;
	v2.Tangent = tangent;
}

std::filesystem::path ResolveSceneTexturePath(const std::filesystem::path& modelPath, const std::filesystem::path& rawPath)
{
	if (rawPath.empty()) {
		return {};
	}

	std::filesystem::path resolved = rawPath;
	if (!resolved.is_absolute()) {
		resolved = modelPath.parent_path() / resolved;
	}
	return resolved.lexically_normal();
}

std::filesystem::path ResolveSceneTexturePathUtf8(const std::filesystem::path& modelPath, const std::string& utf8Path)
{
	if (utf8Path.empty()) {
		return {};
	}
	return ResolveSceneTexturePath(modelPath, std::filesystem::u8path(utf8Path));
}

std::filesystem::path UfbxTexturePath(const std::filesystem::path& modelPath, const ufbx_texture* texture)
{
	if (!texture) {
		return {};
	}

	const ufbx_texture* fileTexture = texture;
	if (fileTexture->type != UFBX_TEXTURE_FILE && fileTexture->file_textures.count > 0) {
		fileTexture = fileTexture->file_textures[0];
	}

	auto StringToPath = [&](const ufbx_string& str) -> std::filesystem::path
	{
		if (str.length == 0 || !str.data) {
			return {};
		}
		return std::filesystem::u8path(std::string(str.data, str.length));
	};

	std::array<std::filesystem::path, 3> rawPaths = {
		StringToPath(fileTexture->relative_filename),
		StringToPath(fileTexture->filename),
		StringToPath(fileTexture->absolute_filename)
	};

	for (const std::filesystem::path& rawPath : rawPaths)
	{
		if (rawPath.empty()) {
			continue;
		}

		const std::filesystem::path resolved = ResolveSceneTexturePath(modelPath, rawPath);
		if (std::filesystem::exists(resolved)) {
			return resolved;
		}
	}

	for (const std::filesystem::path& rawPath : rawPaths)
	{
		if (!rawPath.empty()) {
			return ResolveSceneTexturePath(modelPath, rawPath);
		}
	}

	return {};
}

bool LooksLikeNormalMapPath(const std::filesystem::path& texturePath)
{
	if (texturePath.empty()) {
		return false;
	}

	const std::string lowerName = ToLowerCopy(texturePath.filename().string());
	return ContainsAny(lowerName, {
		"ddn", "normal", "_nrm", "nrm", "_nor", "_norm", "bump"
	});
}

bool LooksLikeDisplacementMapPath(const std::filesystem::path& texturePath)
{
	if (texturePath.empty()) {
		return false;
	}

	const std::string lowerName = ToLowerCopy(texturePath.filename().string());
	return ContainsAny(lowerName, {
		"disp", "displace", "displacement", "height", "_hgt", "_height", "parallax"
	});
}
} // namespace

Framework::Framework(int width, int height, const wchar_t* title)
	: m_initWidth(width)
	, m_initHeight(height)
	, m_title(title ? title : L"")
	, m_clientWidth(width)
	, m_clientHeight(height)
{
}

Framework::~Framework() {
	if (m_device)
		FlushCommandQueue();

	if (m_fenceEvent) {
		CloseHandle(m_fenceEvent);
		m_fenceEvent = nullptr;
	}

	if (m_comInitialized) {
		CoUninitialize();
		m_comInitialized = false;
	}
}

bool Framework::Init() {
	const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (SUCCEEDED(coHr)) {
		m_comInitialized = true;
	}
	else if (coHr != RPC_E_CHANGED_MODE) {
		ThrowIfFailed(coHr);
	}

	m_window = std::make_unique<Window>(m_initWidth, m_initHeight, m_title, this);

	InitDxgi();
	InitD3D12Device();
	m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CreateCommandObjects();
	CreateFence();
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();
	BuildShaders();
	BuildConstantBuffers();
	BuildBoxGeometry();
	InitializeSceneDefinitions();
	BuildSceneGeometryUpload();
	BuildCbvHeap();
	BuildRootSignature();
	BuildPSO();
	BuildSceneLights();
	ResetCameraForCurrentScene();
	UpdateWindowTitle();

	OnResize();

	return MainWnd() != nullptr;
}

int Framework::Run() {
	m_timer.Reset();

	while (m_window->ProcessMessages()) {
		m_timer.Tick();

		if (!m_appPaused) {
			const double dt = m_timer.DeltaTime();
			Update(dt);
			Draw();
		}
		else {
			Sleep(100);
		}
	}

	return 0;
}

LRESULT Framework::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_SIZE:
		m_clientWidth = LOWORD(lParam);
		m_clientHeight = HIWORD(lParam);

		if (wParam == SIZE_MINIMIZED) {
			m_appPaused = true;
			m_minimized = true;
			m_maximized = false;
			m_timer.Stop();
		}
		else if (wParam == SIZE_MAXIMIZED) {
			m_appPaused = false;
			m_minimized = false;
			m_maximized = true;
			m_timer.Start();
			OnResize();
		}
		else if (wParam == SIZE_RESTORED) {
			if (m_minimized) {
				m_appPaused = false;
				m_minimized = false;
				m_timer.Start();
				OnResize();
			}
			else if (m_maximized) {
				m_appPaused = false;
				m_maximized = false;
				m_timer.Start();
				OnResize();
			}
			else if (m_resizing) {

			}
			else {
				OnResize();
			}
		}

		return 0;

	case WM_ACTIVATEAPP:
		if (wParam == FALSE)
		{
			m_appPaused = true;
			m_timer.Stop();
		}
		else
		{
			m_appPaused = false;
			m_timer.Start();
		}
		return 0;

	case WM_ENTERSIZEMOVE:
		m_appPaused = true;
		m_resizing = true;
		m_timer.Stop();
		return 0;

	case WM_EXITSIZEMOVE:
		m_appPaused = false;
		m_resizing = false;
		m_timer.Start();
		OnResize();
		return 0;

	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		OnMouseDown(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_MOUSEMOVE:
		OnMouseMove(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	{
		const uint8_t vk = static_cast<uint8_t>(wParam);
		const bool firstPress = (lParam & 0x40000000) == 0;
		if (firstPress)
		{
			if (vk == VK_F1) {
				m_showBufferDebug = !m_showBufferDebug;
			}
			else if (vk >= '1' && vk <= '9')
			{
				const size_t sceneIndex = static_cast<size_t>(vk - '1');
				if (sceneIndex < m_sceneDefinitions.size()) {
					LoadScene(sceneIndex, true);
				}
			}
		}
		m_keyDown[vk] = true;
		return 0;
	}

	case WM_KEYUP:
	case WM_SYSKEYUP:
	{
		const uint8_t vk = static_cast<uint8_t>(wParam);
		m_keyDown[vk] = false;
		return 0;
	}

	// чтобы при потере фокуса не было "залипших" клавиш
	case WM_KILLFOCUS:
	{
		m_keyDown.fill(false);
		return 0;
	}

	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Framework::CreateRtvAndDsvDescriptorHeaps()
{
	m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
}

void Framework::OnResize()
{
	if (!m_device || !m_swapChain || !m_commandQueue || !m_directCmdListAlloc || !m_commandList)
		return;
	if (m_clientWidth <= 0 || m_clientHeight <= 0)
		return;

	FlushCommandQueue();

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	for (UINT i = 0; i < SwapChainBufferCount; ++i) {
		m_swapChainBuffer[i].Reset();
	}

	m_depthStencilBuffer.Reset();

	ThrowIfFailed(m_swapChain->ResizeBuffers(SwapChainBufferCount, m_clientWidth, m_clientHeight, m_backBufferFormat, 0));

	m_currBackBuffer = static_cast<int>(m_swapChain->GetCurrentBackBufferIndex());

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < SwapChainBufferCount; ++i) {
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffer[i])));
		m_device->CreateRenderTargetView(m_swapChainBuffer[i].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += m_rtvDescriptorSize;
	}

	D3D12_RESOURCE_DESC depthDesc = {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Alignment = 0;
	depthDesc.Width = static_cast<UINT64>(m_clientWidth);
	depthDesc.Height = static_cast<UINT64>(m_clientHeight);
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = m_depthStencilResourceFormat;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.SampleDesc.Quality = 0;
	depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear = {};
	optClear.Format = m_depthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_COMMON, &optClear, IID_PPV_ARGS(&m_depthStencilBuffer)));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format = m_depthStencilFormat;
	dsvDesc.Texture2D.MipSlice = 0;
	m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc, DepthStencilView());

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = m_depthStencilBuffer.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &barrier);

	m_gbuffer.Resize(
		m_device.Get(),
		static_cast<UINT>(m_clientWidth),
		static_cast<UINT>(m_clientHeight),
		m_rtvDescriptorSize);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdsLists);

	FlushCommandQueue();

	m_screenViewport.TopLeftX = 0.0f;
	m_screenViewport.TopLeftY = 0.0f;
	m_screenViewport.Width = static_cast<float>(m_clientWidth);
	m_screenViewport.Height = static_cast<float>(m_clientHeight);
	m_screenViewport.MinDepth = 0.0f;
	m_screenViewport.MaxDepth = 1.0f;

	m_scissorRect = { 0, 0, m_clientWidth, m_clientHeight };

	if (m_cbvHeap) {
		BuildCbvViews();
	}
}

void Framework::Update(const double& dt)
{
	const SceneDefinition* scene = m_sceneDefinitions.empty()
		? nullptr
		: &m_sceneDefinitions[m_currentSceneIndex];

	m_uvAnimation.x += m_uvAnimationSpeed.x * static_cast<float>(dt);
	m_uvAnimation.y += m_uvAnimationSpeed.y * static_cast<float>(dt);

	m_uvAnimation.x -= std::floor(m_uvAnimation.x);
	m_uvAnimation.y -= std::floor(m_uvAnimation.y);

	ObjectConstants obj = {};
	XMMATRIX world =
		XMMatrixTranslation(-m_modelCenter.x, -m_modelCenter.y, -m_modelCenter.z) *
		XMMatrixScaling(m_modelScale, m_modelScale, m_modelScale);
	XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

	XMStoreFloat4x4(&obj.World, XMMatrixTranspose(world));
	XMStoreFloat4x4(&obj.WorldInvTranspose, worldInvTranspose);

	m_objectCB->CopyData(0, obj);

	XMVECTOR pos = XMLoadFloat3(&m_camPos);
	XMVECTOR target = XMLoadFloat3(&m_camTarget);
	XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&m_camUp));

	XMVECTOR forward = XMVector3Normalize(target - pos);
	XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));

	float speed = m_cameraMoveSpeed;
	if (m_keyDown[VK_SHIFT]) speed *= 3.0f; 

	float step = speed * static_cast<float>(dt);
	XMVECTOR move = XMVectorZero();

	if (m_keyDown['W']) move += forward;
	if (m_keyDown['S']) move -= forward;
	if (m_keyDown['D']) move += right;
	if (m_keyDown['A']) move -= right;

	if (m_keyDown[VK_SPACE])   move += up;
	if (m_keyDown[VK_CONTROL]) move -= up;

	if (!XMVector3Equal(move, XMVectorZero()))
		move = XMVector3Normalize(move) * step;

	pos += move;
	target += move;

	XMStoreFloat3(&m_camPos, pos);
	XMStoreFloat3(&m_camTarget, target);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);

	float aspect = (float)m_clientWidth / (float)m_clientHeight;
	XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 0.1f, 1000.0f);

	XMMATRIX viewProj = view * proj;

	PassConstants pass{};
	XMStoreFloat4x4(&pass.ViewProj, XMMatrixTranspose(viewProj));

	XMStoreFloat3(&pass.EyePosW, pos);

	if (m_directionalLightCount > 0) {
		pass.LightDirW = {
			m_directionalLights[0].DirectionIntensity.x,
			m_directionalLights[0].DirectionIntensity.y,
			m_directionalLights[0].DirectionIntensity.z
		};
	}
	else {
		pass.LightDirW = { 0.577f, -0.3f, 0.577f };
	}
	pass.Ambient = { 0.2f, 0.2f, 0.2f, 1.0f };
	pass.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
	pass.Specular = { 1.0f, 1.0f, 1.0f, 1.0f };
	pass.SpecPower = 32.0f;
	if (scene)
	{
		pass.Ambient = scene->ForwardAmbient;
		pass.Diffuse = scene->ForwardDiffuse;
		pass.Specular = scene->ForwardSpecular;
		pass.SpecPower = scene->ForwardSpecPower;
		pass.UvScroll = scene->EnableUvScroll ? m_uvAnimation : DirectX::XMFLOAT2{ 0.0f, 0.0f };
		pass.UvTiling = scene->GlobalUvTiling;
	}
	else
	{
		pass.UvScroll = m_uvAnimation;
		pass.UvTiling = m_uvGlobalTiling;
	}
	pass.Time = static_cast<float>(m_timer.TotalTime());
	pass.TessellationParams = {
		m_tessellationMinDistance,
		m_tessellationMaxDistance,
		m_tessellationMinFactor,
		m_tessellationMaxFactor
	};

	m_passCB->CopyData(0, pass);

	DeferredPassConstants deferred{};
	XMStoreFloat3(&deferred.EyePosW, pos);
	deferred.AmbientIntensity = scene ? scene->DeferredAmbientIntensity : 0.18f;
	deferred.AmbientColor = scene ? scene->DeferredAmbientColor : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
	XMStoreFloat4x4(&deferred.InvViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj)));

	const UINT dirCount = std::min<UINT>(m_directionalLightCount, MaxDirectionalLights);
	const UINT pointCount = std::min<UINT>(m_pointLightCount, MaxPointLights);
	const UINT spotCount = std::min<UINT>(m_spotLightCount, MaxSpotLights);

	deferred.DirectionalLightCount = dirCount;
	deferred.PointLightCount = pointCount;
	deferred.SpotLightCount = spotCount;
	deferred.DebugViewEnabled = m_showBufferDebug ? 1u : 0u;

	for (UINT i = 0; i < dirCount; ++i) {
		m_directionalLightSB->CopyData(i, m_directionalLights[i]);
	}

	const float time = static_cast<float>(m_timer.TotalTime());
	for (UINT i = 0; i < pointCount; ++i)
	{
		GpuPointLight light = m_pointLights[i];
		light.PositionRange.y += 0.08f * std::sin(time * 0.85f + 0.35f * static_cast<float>(i));
		m_pointLightSB->CopyData(i, light);
	}

	for (UINT i = 0; i < spotCount; ++i) {
		m_spotLightSB->CopyData(i, m_spotLights[i]);
	}

	m_deferredPassCB->CopyData(0, deferred);
}

void Framework::Draw()
{
	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	m_commandList->RSSetViewports(1, &m_screenViewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);

	ID3D12DescriptorHeap* heaps[] = { m_cbvHeap.Get() };
	m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);

	m_gbuffer.TransitionToRenderTargets(m_commandList.Get());

	m_commandList->SetGraphicsRootSignature(m_renderingSystem.GeometryRootSignature());
	m_commandList->SetGraphicsRootDescriptorTable(0, CbvSrvGpuHandle(0));

	auto BindMaterial = [&](const Framework::ModelMaterial& srcMaterial)
	{
		MaterialConstants mat = {};
		mat.DiffuseAlbedo = srcMaterial.DiffuseAlbedo;
		mat.UvTilingOffset = {
			srcMaterial.UvTiling.x,
			srcMaterial.UvTiling.y,
			srcMaterial.UvOffset.x,
			srcMaterial.UvOffset.y
		};
		mat.Flags = srcMaterial.Flags;
		mat.DisplacementScale = srcMaterial.DisplacementScale;
		mat.DisplacementBias = srcMaterial.DisplacementBias;
		mat.AlphaCutoff = srcMaterial.AlphaCutoff;
		mat.WindParams = srcMaterial.WindParams;

		m_commandList->SetGraphicsRoot32BitConstants(
			1,
			static_cast<UINT>(sizeof(MaterialConstants) / sizeof(UINT32)),
			&mat,
			0);

		const D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = CbvSrvGpuHandle(m_textureSrvBaseIndex + srcMaterial.SrvBaseIndex);
		m_commandList->SetGraphicsRootDescriptorTable(2, textureHandle);
	};

	m_gbuffer.BindAsRenderTargets(m_commandList.Get(), DepthStencilView());
	m_gbuffer.Clear(m_commandList.Get());
	m_commandList->ClearDepthStencilView(
		DepthStencilView(),
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
		1.0f,
		0,
		0,
		nullptr
	);

	if (m_modelVB && !m_modelSubsets.empty())
	{
		m_commandList->IASetVertexBuffers(0, 1, &m_modelVBV);
		bool tessellationPipelineActive = false;
		bool pipelineInitialized = false;

		for (const ModelSubset& subset : m_modelSubsets)
		{
			const UINT materialIndex = (subset.MaterialIndex < m_modelMaterials.size()) ? subset.MaterialIndex : 0;
			const ModelMaterial& material = m_modelMaterials[materialIndex];
			const bool usesTessellation =
				(material.Flags & MaterialFlagHasDisplacementTexture) != 0u &&
				std::fabs(material.DisplacementScale) > 1e-6f;

			if (!pipelineInitialized || tessellationPipelineActive != usesTessellation)
			{
				tessellationPipelineActive = usesTessellation;
				pipelineInitialized = true;

				if (tessellationPipelineActive)
				{
					m_commandList->SetPipelineState(m_renderingSystem.GeometryTessellationPSO());
					m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
				}
				else
				{
					m_commandList->SetPipelineState(m_renderingSystem.GeometryBasicPSO());
					m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				}
			}

			BindMaterial(material);
			m_commandList->DrawInstanced(subset.VertexCount, 1, subset.StartVertex, 0);
		}
	}
	else
	{
		m_commandList->SetPipelineState(m_renderingSystem.GeometryBasicPSO());
		BindMaterial(ModelMaterial{});
		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_commandList->IASetVertexBuffers(0, 1, &m_boxVBView);
		m_commandList->IASetIndexBuffer(&m_boxIBView);
		m_commandList->DrawIndexedInstanced(m_boxIndexCount, 1, 0, 0, 0);
	}

	m_gbuffer.TransitionToShaderResources(m_commandList.Get());

	D3D12_RESOURCE_BARRIER depthToSrv{};
	depthToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	depthToSrv.Transition.pResource = m_depthStencilBuffer.Get();
	depthToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	depthToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	depthToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &depthToSrv);

	D3D12_RESOURCE_BARRIER toRT{};
	toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toRT.Transition.pResource = CurrentBackBuffer();
	toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &toRT);

	D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = CurrentBackBufferView();
	m_commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);
	m_commandList->ClearRenderTargetView(backBufferRtv, DirectX::Colors::Black, 0, nullptr);

	m_commandList->SetGraphicsRootSignature(m_renderingSystem.LightingRootSignature());
	m_commandList->SetPipelineState(m_renderingSystem.LightingPSO());
	m_commandList->SetGraphicsRootDescriptorTable(0, CbvSrvGpuHandle(m_deferredPassCbvIndex));
	m_commandList->SetGraphicsRootDescriptorTable(1, CbvSrvGpuHandle(m_gbufferSrvBaseIndex));
	m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_commandList->DrawInstanced(3, 1, 0, 0);

	D3D12_RESOURCE_BARRIER depthToWrite{};
	depthToWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	depthToWrite.Transition.pResource = m_depthStencilBuffer.Get();
	depthToWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	depthToWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	depthToWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &depthToWrite);

	D3D12_RESOURCE_BARRIER toPresent{};
	toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toPresent.Transition.pResource = CurrentBackBuffer();
	toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &toPresent);

	ThrowIfFailed(m_commandList->Close());

	ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(m_swapChain->Present(0, 0));
	m_currBackBuffer = (m_currBackBuffer + 1) % SwapChainBufferCount;

	FlushCommandQueue();
}


void Framework::InitDxgi() {
	UINT factoryFlags = 0;

#if defined(_DEBUG)
	factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

	ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_dxgiFactory)));

#if defined(_DEBUG)
	LogAdapters();
#endif

	PickAdapter();
}

void Framework::PickAdapter() {
	m_dxgiAdapter.Reset();
	m_adapterName.clear();

	ComPtr<IDXGIFactory6> factory6;

	if (SUCCEEDED(m_dxgiFactory.As(&factory6))) {
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIAdapter1> adapter;

			if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 desc = {};
			ThrowIfFailed(adapter->GetDesc1(&desc));

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			ComPtr<ID3D12Device> testDevice;

			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDevice)))) {
				m_dxgiAdapter = adapter;
				m_adapterName = desc.Description;
				break;
			}
		}
	}

	if (!m_dxgiAdapter) {
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIAdapter1> adapter;

			if (m_dxgiFactory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 desc = {};
			ThrowIfFailed(adapter->GetDesc1(&desc));

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			ComPtr<ID3D12Device> testDevice;

			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDevice)))) {
				m_dxgiAdapter = adapter;
				m_adapterName = desc.Description;
				break;
			}
		}
	}

	if (!m_dxgiAdapter) {
		throw std::runtime_error("No suitable DXGI adapter found (D3D12-capable).");
	}

#if defined(_DEBUG)
	std::wstring msg = L"[DXGI] Using adapter: " + m_adapterName + L"\n";
	OutputDebugStringW(msg.c_str());
#endif
}

void Framework::LogAdapters() {
#if defined(_DEBUG)
	OutputDebugStringW(L"[DXGI] Adapters:\n");

	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		
		HRESULT hr = m_dxgiFactory->EnumAdapters1(i, &adapter);
		if (hr == DXGI_ERROR_NOT_FOUND) break;
		ThrowIfFailed(hr);

		if (m_dxgiFactory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
			break;

		DXGI_ADAPTER_DESC1 desc = {};
		ThrowIfFailed(adapter->GetDesc1(&desc));

		std::wstring line = L"  -  ";
		line += desc.Description;
		line += (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L" (SOPTWARE)\n" : L"\n";
		OutputDebugStringW(line.c_str());

		LogAdapterOutputs(adapter.Get());
	}
#endif
}

void Framework::LogAdapterOutputs(IDXGIAdapter1* adapter) {
#if defined(_DEBUG)
	for (UINT j = 0;; ++j) {
		ComPtr<IDXGIOutput> output;

		if (adapter->EnumOutputs(j, &output) == DXGI_ERROR_NOT_FOUND)
			break;

		DXGI_OUTPUT_DESC outDesc = {};
		ThrowIfFailed(output->GetDesc(&outDesc));

		std::wstring line = L"		Ouput: ";
		line += outDesc.DeviceName;
		line += L"\n";
		OutputDebugStringW(line.c_str());
	}
#endif
}

void Framework::InitD3D12Device() {
#if defined(_DEBUG)
	ComPtr<ID3D12Debug> debugController;

	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
		debugController->EnableDebugLayer();
		OutputDebugStringW(L"[D3d12] Debug layer enabled\n");
	}
	else {
		OutputDebugStringW(L"[D3D12] Debug layer NOT available (Graphics Tools may be missing)\n");
	}
#endif

	HRESULT hr = D3D12CreateDevice(m_dxgiAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));

	if (FAILED(hr)) {
		OutputDebugStringW(L"[D3D12] Hardware device failed, falling back to WARP\n");

		ThrowIfFailed(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&m_dxgiAdapter)));
		ThrowIfFailed(D3D12CreateDevice(m_dxgiAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)));
	}

#if defined(_DEBUG)
	OutputDebugStringW(L"[D3D12] Device created \n");

	ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(m_device.As(&infoQueue))) {
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
	}
#endif
}

void Framework::CreateCommandObjects() {
	D3D12_COMMAND_QUEUE_DESC qdesc = {};
	qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	ThrowIfFailed(m_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&m_commandQueue)));

	ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_directCmdListAlloc)));

	ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_directCmdListAlloc.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));

	ThrowIfFailed(m_commandList->Close());

#if defined(_DEBUG)
	OutputDebugStringW(L"[D3D12] Command queue/allocator/list created\n");
#endif
}

void Framework::CreateFence() {
	ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

	m_currentFence = 0;

	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	if (m_fenceEvent == nullptr)
		throw std::runtime_error("CreateEvent failed for fence event.");
}

void Framework::FlushCommandQueue() {
	if (!m_commandQueue || !m_fence || !m_fenceEvent)
		return;

	++m_currentFence;
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_currentFence));

	if (m_fence->GetCompletedValue() < m_currentFence) {
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_currentFence, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}
}

void Framework::CreateSwapChain() {
	m_swapChain.Reset();

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = m_clientWidth;
	sd.Height = m_clientHeight;
	sd.Format = m_backBufferFormat;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	sd.Flags = 0;

	ComPtr<IDXGISwapChain1> swapChain1;
	ThrowIfFailed(m_dxgiFactory->CreateSwapChainForHwnd(m_commandQueue.Get(), MainWnd(), &sd, nullptr, nullptr, &swapChain1));
	ThrowIfFailed(m_dxgiFactory->MakeWindowAssociation(MainWnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain1.As(&m_swapChain));
	m_currBackBuffer = static_cast<int>(m_swapChain->GetCurrentBackBufferIndex());
}

void Framework::BuildShaders()
{
	m_renderingSystem.BuildShaders();
}

void Framework::BuildConstantBuffers()
{
	m_objectCB = std::make_unique<UploadBuffer<ObjectConstants>>(m_device.Get(), 1, true);
	m_passCB = std::make_unique<UploadBuffer<PassConstants>>(m_device.Get(), 1, true);
	m_deferredPassCB = std::make_unique<UploadBuffer<DeferredPassConstants>>(m_device.Get(), 1, true);
	m_directionalLightSB = std::make_unique<UploadBuffer<GpuDirectionalLight>>(m_device.Get(), MaxDirectionalLights, false);
	m_pointLightSB = std::make_unique<UploadBuffer<GpuPointLight>>(m_device.Get(), MaxPointLights, false);
	m_spotLightSB = std::make_unique<UploadBuffer<GpuSpotLight>>(m_device.Get(), MaxSpotLights, false);
}

void Framework::BuildCbvHeap()
{
	m_textureSrvBaseIndex = 2;
	const UINT materialBlockCount = std::max<UINT>(1u, static_cast<UINT>(m_modelMaterials.size()));
	m_textureSrvCount = materialBlockCount * MaterialTextureSlotCount;
	m_deferredPassCbvIndex = m_textureSrvBaseIndex + m_textureSrvCount;
	m_gbufferSrvBaseIndex = m_deferredPassCbvIndex + 1;
	m_depthSrvIndex = m_gbufferSrvBaseIndex + Gbuffer::kTargetCount;
	m_directionalLightSrvIndex = m_depthSrvIndex + 1;
	m_pointLightSrvIndex = m_directionalLightSrvIndex + 1;
	m_spotLightSrvIndex = m_pointLightSrvIndex + 1;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = m_spotLightSrvIndex + 1;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_cbvHeap.GetAddressOf())));
}

void Framework::BuildCbvViews()
{
	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = m_objectCB->Resource()->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = CalcConstantBufferByteSize(sizeof(ObjectConstants));

		m_device->CreateConstantBufferView(&cbvDesc, CbvSrvCpuHandle(0));
	}

	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = m_passCB->Resource()->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = CalcConstantBufferByteSize(sizeof(PassConstants));

		m_device->CreateConstantBufferView(&cbvDesc, CbvSrvCpuHandle(1));
	}

	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = m_deferredPassCB->Resource()->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = CalcConstantBufferByteSize(sizeof(DeferredPassConstants));
		m_device->CreateConstantBufferView(&cbvDesc, CbvSrvCpuHandle(m_deferredPassCbvIndex));
	}

	if (m_textureSrvCount > 0)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.PlaneSlice = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		for (UINT materialIndex = 0; materialIndex < std::max<UINT>(1u, static_cast<UINT>(m_modelMaterials.size())); ++materialIndex)
		{
			const ModelMaterial* materialPtr = (materialIndex < m_modelMaterials.size())
				? &m_modelMaterials[materialIndex]
				: nullptr;
			const ModelMaterial fallbackMaterial = {};
			const ModelMaterial& material = materialPtr ? *materialPtr : fallbackMaterial;

			for (UINT slot = 0; slot < MaterialTextureSlotCount; ++slot)
			{
				const UINT textureIndex = material.TextureIndices[slot];
				ID3D12Resource* texture = (textureIndex < m_textureResources.size()) ? m_textureResources[textureIndex].Get() : nullptr;
				srvDesc.Format = texture ? texture->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
				m_device->CreateShaderResourceView(
					texture,
					&srvDesc,
					CbvSrvCpuHandle(m_textureSrvBaseIndex + material.SrvBaseIndex + slot));
			}
		}
	}

	if (m_gbuffer.IsValid()) {
		m_gbuffer.CreateSrvDescriptors(m_device.Get(), CbvSrvCpuHandle(m_gbufferSrvBaseIndex), m_cbvSrvUavDescriptorSize);
	}

	if (m_depthStencilBuffer) {
		D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
		depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		depthSrvDesc.Format = m_depthStencilSrvFormat;
		depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		depthSrvDesc.Texture2D.MostDetailedMip = 0;
		depthSrvDesc.Texture2D.MipLevels = 1;
		depthSrvDesc.Texture2D.PlaneSlice = 0;
		depthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		m_device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &depthSrvDesc, CbvSrvCpuHandle(m_depthSrvIndex));
	}

	auto CreateStructuredLightSrv = [&](ID3D12Resource* resource, UINT numElements, UINT stride, UINT descriptorIndex)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = numElements;
		srvDesc.Buffer.StructureByteStride = stride;
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		m_device->CreateShaderResourceView(resource, &srvDesc, CbvSrvCpuHandle(descriptorIndex));
	};

	CreateStructuredLightSrv(
		m_directionalLightSB ? m_directionalLightSB->Resource() : nullptr,
		MaxDirectionalLights,
		static_cast<UINT>(sizeof(GpuDirectionalLight)),
		m_directionalLightSrvIndex);

	CreateStructuredLightSrv(
		m_pointLightSB ? m_pointLightSB->Resource() : nullptr,
		MaxPointLights,
		static_cast<UINT>(sizeof(GpuPointLight)),
		m_pointLightSrvIndex);

	CreateStructuredLightSrv(
		m_spotLightSB ? m_spotLightSB->Resource() : nullptr,
		MaxSpotLights,
		static_cast<UINT>(sizeof(GpuSpotLight)),
		m_spotLightSrvIndex);
}

void Framework::BuildRootSignature()
{
	m_renderingSystem.BuildRootSignatures(m_device.Get());
}

void Framework::BuildPSO()
{
	m_renderingSystem.BuildPSOs(m_device.Get(), m_backBufferFormat, m_depthStencilFormat);
}

void Framework::InitializeSceneDefinitions()
{
	if (!m_sceneDefinitions.empty()) {
		return;
	}

	const std::filesystem::path assetsPath = std::filesystem::path(L"assets");

	SceneDefinition sponza = {};
	sponza.Name = L"Sponza";
	sponza.ModelPaths = {
		assetsPath / L"sponza" / L"sponza.obj"
	};
	sponza.Format = SceneAssetFormat::Obj;
	sponza.CameraPos = { 2.2f, 1.5f, -4.2f };
	sponza.CameraTarget = { 0.0f, 0.15f, 0.0f };
	sponza.CameraMoveSpeed = 3.0f;
	sponza.TessellationMinDistance = 0.55f;
	sponza.TessellationMaxDistance = 2.6f;
	sponza.TessellationMinFactor = 1.0f;
	sponza.TessellationMaxFactor = 7.0f;
	sponza.DefaultDisplacementScale = 0.055f;
	sponza.DefaultDisplacementBias = 0.0f;
	sponza.AlphaCutoff = 0.33f;
	sponza.EnableWindAnimation = true;
	sponza.EnableUvScroll = true;
	sponza.LightingPreset = SceneLightingPreset::Default;
	m_sceneDefinitions.push_back(sponza);

	SceneDefinition bistro = {};
	bistro.Name = L"Bistro";
	bistro.ModelPaths = {
		assetsPath / L"bistro" / L"BistroInterior.fbx",
		assetsPath / L"bistro" / L"BistroInterior_Wine.fbx",
		assetsPath / L"bistro" / L"BistroExterior.fbx"
	};
	bistro.Format = SceneAssetFormat::Fbx;
	bistro.CameraPos = { 2.6f, 1.6f, -5.0f };
	bistro.CameraTarget = { 0.0f, 0.15f, 0.0f };
	bistro.CameraMoveSpeed = 2.8f;
	bistro.TessellationMinDistance = 0.35f;
	bistro.TessellationMaxDistance = 1.8f;
	bistro.TessellationMinFactor = 1.0f;
	bistro.TessellationMaxFactor = 3.5f;
	bistro.DefaultDisplacementScale = 0.020f;
	bistro.DefaultDisplacementBias = 0.0f;
	bistro.AlphaCutoff = 0.40f;
	bistro.EnableWindAnimation = false;
	bistro.EnableUvScroll = false;
	bistro.LightingPreset = SceneLightingPreset::Bistro;
	bistro.ForwardAmbient = { 0.14f, 0.14f, 0.15f, 1.0f };
	bistro.ForwardDiffuse = { 0.98f, 0.96f, 0.92f, 1.0f };
	bistro.ForwardSpecular = { 0.75f, 0.72f, 0.68f, 1.0f };
	bistro.ForwardSpecPower = 24.0f;
	bistro.DeferredAmbientIntensity = 0.24f;
	bistro.DeferredAmbientColor = { 0.98f, 0.96f, 0.92f, 1.0f };
	m_sceneDefinitions.push_back(bistro);

	SceneDefinition sanMiguel = {};
	sanMiguel.Name = L"San Miguel";
	sanMiguel.ModelPaths = {
		assetsPath / L"san_miguel" / L"san-miguel-low-poly.obj"
	};
	sanMiguel.Format = SceneAssetFormat::Obj;
	sanMiguel.CameraPos = { 2.8f, 1.7f, -5.6f };
	sanMiguel.CameraTarget = { 0.0f, 0.15f, 0.0f };
	sanMiguel.CameraMoveSpeed = 2.8f;
	sanMiguel.TessellationMinDistance = 0.45f;
	sanMiguel.TessellationMaxDistance = 2.1f;
	sanMiguel.TessellationMinFactor = 1.0f;
	sanMiguel.TessellationMaxFactor = 5.0f;
	sanMiguel.DefaultDisplacementScale = 0.018f;
	sanMiguel.DefaultDisplacementBias = 0.0f;
	sanMiguel.AlphaCutoff = 0.35f;
	sanMiguel.EnableWindAnimation = false;
	sanMiguel.EnableUvScroll = false;
	sanMiguel.AllowKeywordedBumpAsDisplacement = true;
	sanMiguel.LightingPreset = SceneLightingPreset::SanMiguel;
	sanMiguel.ForwardAmbient = { 0.18f, 0.16f, 0.14f, 1.0f };
	sanMiguel.ForwardDiffuse = { 0.98f, 0.93f, 0.84f, 1.0f };
	sanMiguel.ForwardSpecular = { 0.62f, 0.58f, 0.52f, 1.0f };
	sanMiguel.ForwardSpecPower = 22.0f;
	sanMiguel.DeferredAmbientIntensity = 0.28f;
	sanMiguel.DeferredAmbientColor = { 0.98f, 0.92f, 0.84f, 1.0f };
	m_sceneDefinitions.push_back(sanMiguel);
}

void Framework::LoadScene(size_t sceneIndex, bool resetCamera)
{
	if (sceneIndex >= m_sceneDefinitions.size()) {
		return;
	}

	m_currentSceneIndex = sceneIndex;

	const SceneDefinition& scene = m_sceneDefinitions[m_currentSceneIndex];
	m_cameraMoveSpeed = scene.CameraMoveSpeed;
	m_tessellationMinDistance = scene.TessellationMinDistance;
	m_tessellationMaxDistance = scene.TessellationMaxDistance;
	m_tessellationMinFactor = scene.TessellationMinFactor;
	m_tessellationMaxFactor = scene.TessellationMaxFactor;
	m_uvGlobalTiling = scene.GlobalUvTiling;

	BuildSceneLights();

	BuildSceneGeometryUpload();
	BuildCbvHeap();
	BuildCbvViews();

	if (resetCamera) {
		ResetCameraForCurrentScene();
	}

	UpdateWindowTitle();
}

void Framework::ResetCameraForCurrentScene()
{
	if (m_sceneDefinitions.empty()) {
		return;
	}

	const SceneDefinition& scene = m_sceneDefinitions[m_currentSceneIndex];
	m_camPos = scene.CameraPos;
	m_camTarget = scene.CameraTarget;
	m_camUp = { 0.0f, 1.0f, 0.0f };
	m_cameraMoveSpeed = scene.CameraMoveSpeed;

	using namespace DirectX;
	const XMVECTOR pos = XMLoadFloat3(&m_camPos);
	const XMVECTOR target = XMLoadFloat3(&m_camTarget);
	const XMVECTOR forward = XMVector3Normalize(target - pos);

	const float fx = XMVectorGetX(forward);
	const float fy = XMVectorGetY(forward);
	const float fz = XMVectorGetZ(forward);
	m_pitch = std::asin(std::clamp(fy, -1.0f, 1.0f));
	m_yaw = std::atan2(fx, fz);
}

void Framework::UpdateWindowTitle() const
{
	if (!MainWnd()) {
		return;
	}

	std::wstring title = m_title ? m_title : L"CG Window";
	if (!m_sceneDefinitions.empty()) {
		title += L" | Scene: ";
		title += m_sceneDefinitions[m_currentSceneIndex].Name;
		title += L" | 1-";
		title += std::to_wstring(m_sceneDefinitions.size());
		title += L" switch, F1 debug";
	}

	SetWindowTextW(MainWnd(), title.c_str());
}

void Framework::BuildSceneLights()
{
	for (GpuDirectionalLight& light : m_directionalLights) {
		light = {};
	}
	for (GpuPointLight& light : m_pointLights) {
		light = {};
	}
	for (GpuSpotLight& light : m_spotLights) {
		light = {};
	}

	const SceneDefinition* scene = m_sceneDefinitions.empty()
		? nullptr
		: &m_sceneDefinitions[m_currentSceneIndex];

	if (scene && scene->LightingPreset == SceneLightingPreset::Bistro)
	{
		m_directionalLightCount = std::min<UINT>(2u, static_cast<UINT>(m_directionalLights.size()));
		m_directionalLights[0].DirectionIntensity = { 0.32f, -1.0f, 0.18f, 1.35f };
		m_directionalLights[0].Color = { 1.00f, 0.95f, 0.88f, 1.0f };

		m_directionalLights[1].DirectionIntensity = { -0.12f, -0.45f, -0.88f, 0.18f };
		m_directionalLights[1].Color = { 0.52f, 0.58f, 0.70f, 1.0f };

		m_pointLightCount = 0;
		m_spotLightCount = 0;
		return;
	}

	if (scene && scene->LightingPreset == SceneLightingPreset::SanMiguel)
	{
		m_directionalLightCount = std::min<UINT>(3u, static_cast<UINT>(m_directionalLights.size()));
		m_directionalLights[0].DirectionIntensity = { 0.58f, -1.0f, 0.24f, 1.45f };
		m_directionalLights[0].Color = { 1.00f, 0.92f, 0.80f, 1.0f };

		m_directionalLights[1].DirectionIntensity = { -0.18f, -0.36f, -0.92f, 0.24f };
		m_directionalLights[1].Color = { 0.52f, 0.63f, 0.78f, 1.0f };

		m_directionalLights[2].DirectionIntensity = { -0.76f, -0.18f, 0.08f, 0.12f };
		m_directionalLights[2].Color = { 1.00f, 0.74f, 0.48f, 1.0f };

		m_pointLightCount = 0;
		m_spotLightCount = 0;
		return;
	}

	m_directionalLightCount = std::min<UINT>(2u, static_cast<UINT>(m_directionalLights.size()));
	m_directionalLights[0].DirectionIntensity = { 0.45f, -1.0f, 0.20f, 1.10f };
	m_directionalLights[0].Color = { 1.00f, 0.96f, 0.90f, 1.0f };

	m_directionalLights[1].DirectionIntensity = { -0.30f, -0.70f, -0.60f, 0.35f };
	m_directionalLights[1].Color = { 0.45f, 0.55f, 0.80f, 1.0f };

	static const DirectX::XMFLOAT3 pointPalette[] = {
		{ 1.0f, 0.40f, 0.30f },
		{ 0.30f, 0.80f, 1.00f },
		{ 1.00f, 0.85f, 0.35f },
		{ 0.55f, 1.00f, 0.45f },
		{ 0.85f, 0.45f, 1.00f },
		{ 1.00f, 0.65f, 0.30f }
	};

	m_pointLightCount = std::min<UINT>(24u, static_cast<UINT>(m_pointLights.size()));
	for (UINT i = 0; i < m_pointLightCount; ++i)
	{
		const float x = (static_cast<float>(i % 6) - 2.5f) * 0.65f;
		const float z = (static_cast<float>(i / 6) - 1.5f) * 0.85f;
		const float y = 0.12f + 0.35f * static_cast<float>(i % 3);

		const DirectX::XMFLOAT3& color = pointPalette[i % _countof(pointPalette)];
		m_pointLights[i].PositionRange = { x, y, z, 1.8f };
		m_pointLights[i].ColorIntensity = { color.x, color.y, color.z, 3.2f };
	}

	m_spotLightCount = std::min<UINT>(6u, static_cast<UINT>(m_spotLights.size()));
	const float innerCos = std::cos(DirectX::XMConvertToRadians(16.0f));
	const float outerCos = std::cos(DirectX::XMConvertToRadians(30.0f));

	for (UINT i = 0; i < m_spotLightCount; ++i)
	{
		const float angle = (DirectX::XM_2PI * static_cast<float>(i)) / static_cast<float>(m_spotLightCount);
		const float x = std::cos(angle) * 1.35f;
		const float z = std::sin(angle) * 1.15f;
		const float y = 1.25f;

		DirectX::XMFLOAT3 dir = { -x * 0.75f, -1.2f, -z * 0.75f };
		DirectX::XMVECTOR dirV = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&dir));
		DirectX::XMStoreFloat3(&dir, dirV);

		const DirectX::XMFLOAT3& color = pointPalette[(i * 2) % _countof(pointPalette)];
		m_spotLights[i].PositionRange = { x, y, z, 3.0f };
		m_spotLights[i].DirectionCosInner = { dir.x, dir.y, dir.z, innerCos };
		m_spotLights[i].ColorIntensity = { color.x, color.y, color.z, 4.2f };
		m_spotLights[i].Params = { outerCos, 0.0f, 0.0f, 0.0f };
	}
}

void Framework::BuildBoxGeometry()
{
	auto ColorFromPos = [](float x, float y, float z)
		{
			return DirectX::XMFLOAT4(
				(x + 1.0f) * 0.5f,
				(y + 1.0f) * 0.5f,
				(z + 1.0f) * 0.5f,
				1.0f
			);
		};

	std::array<Vertex, 24> vertices =
	{
		Vertex{ { -1.0f, -1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },
		Vertex{ { -1.0f,  1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f, -1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },

		Vertex{ { -1.0f, -1.0f,  1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f, -1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ { -1.0f, -1.0f, -1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f, -1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },

		Vertex{ { -1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },
		Vertex{ {  1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },
	};

	std::array<std::uint16_t, 36> indices =
	{
		0, 1, 2,  0, 2, 3,
		4, 5, 6,  4, 6, 7,
		8, 9,10,  8,10,11,
		12,13,14, 12,14,15,
		16,17,18, 16,18,19,
		20,21,22, 20,22,23
	};

	m_boxIndexCount = (UINT)indices.size();

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto MakeBufferDesc = [](UINT64 byteSize)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Alignment = 0;
			d.Width = byteSize;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count = 1;
			d.SampleDesc.Quality = 0;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			d.Flags = D3D12_RESOURCE_FLAG_NONE;
			return d;
		};

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	defaultHeap.CreationNodeMask = 1;
	defaultHeap.VisibleNodeMask = 1;

	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
	uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	uploadHeap.CreationNodeMask = 1;
	uploadHeap.VisibleNodeMask = 1;

	auto vbDesc = MakeBufferDesc(vbByteSize);
	auto ibDesc = MakeBufferDesc(ibByteSize);

	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(m_boxVB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&ibDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(m_boxIB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_boxVBUpload.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&ibDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_boxIBUpload.GetAddressOf())));

	{
		void* mapped = nullptr;
		ThrowIfFailed(m_boxVBUpload->Map(0, nullptr, &mapped));
		memcpy(mapped, vertices.data(), vbByteSize);
		m_boxVBUpload->Unmap(0, nullptr);
	}
	{
		void* mapped = nullptr;
		ThrowIfFailed(m_boxIBUpload->Map(0, nullptr, &mapped));
		memcpy(mapped, indices.data(), ibByteSize);
		m_boxIBUpload->Unmap(0, nullptr);
	}

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	D3D12_RESOURCE_BARRIER toCopyDest[2] = {};
	toCopyDest[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toCopyDest[0].Transition.pResource = m_boxVB.Get();
	toCopyDest[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	toCopyDest[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	toCopyDest[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	toCopyDest[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toCopyDest[1].Transition.pResource = m_boxIB.Get();
	toCopyDest[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	toCopyDest[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	toCopyDest[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	m_commandList->ResourceBarrier(2, toCopyDest);

	m_commandList->CopyBufferRegion(m_boxVB.Get(), 0, m_boxVBUpload.Get(), 0, vbByteSize);
	m_commandList->CopyBufferRegion(m_boxIB.Get(), 0, m_boxIBUpload.Get(), 0, ibByteSize);

	D3D12_RESOURCE_BARRIER barriers[2] = {};

	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = m_boxVB.Get();
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = m_boxIB.Get();
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	m_commandList->ResourceBarrier(2, barriers);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmds[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmds);

	FlushCommandQueue();

	m_boxVBView.BufferLocation = m_boxVB->GetGPUVirtualAddress();
	m_boxVBView.StrideInBytes = sizeof(Vertex);
	m_boxVBView.SizeInBytes = vbByteSize;

	m_boxIBView.BufferLocation = m_boxIB->GetGPUVirtualAddress();
	m_boxIBView.Format = DXGI_FORMAT_R16_UINT;
	m_boxIBView.SizeInBytes = ibByteSize;

	m_boxVBUpload.Reset();
	m_boxIBUpload.Reset();
}

void Framework::BuildObjVB_Upload()
{
#if 0
	using namespace DirectX;

	const std::filesystem::path objPath = std::filesystem::path(L"assets") / L"sponza.obj";
	const std::string objPathUtf8 = objPath.u8string();
	const std::string baseDirUtf8 = objPath.parent_path().u8string();

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> tinyMaterials;
	std::string warn, err;

	bool ok = tinyobj::LoadObj(
		&attrib, &shapes, &tinyMaterials,
		&warn, &err,
		objPathUtf8.c_str(),
		baseDirUtf8.empty() ? nullptr : baseDirUtf8.c_str(),
		true);

	if (!warn.empty())
		OutputDebugStringA(("[tinyobj warn] " + warn + "\n").c_str());
	if (!err.empty())
		OutputDebugStringA(("[tinyobj err ] " + err + "\n").c_str());

	if (!ok) {
		throw std::runtime_error("tinyobj::LoadObj failed (see Output window).");
	}

	m_modelMaterials.clear();
	m_modelSubsets.clear();
	m_textureResources.clear();
	m_textureUploadResources.clear();
	m_modelVB.Reset();
	m_modelVBUpload.Reset();
	m_modelVertexCount = 0;

	std::vector<RgbaImage> textureImages;
	textureImages.reserve(32);
	textureImages.push_back(RgbaImage{});

	std::unordered_map<std::wstring, UINT> textureIndexByPath;
	textureIndexByPath.reserve(64);

	ModelMaterial defaultMaterial = {};
	defaultMaterial.HasTexture = true;
	defaultMaterial.TextureIndex = 0;
	m_modelMaterials.push_back(defaultMaterial);

	for (const tinyobj::material_t& srcMaterial : tinyMaterials)
	{
		ModelMaterial mat = {};
		mat.DiffuseAlbedo = {
			static_cast<float>(srcMaterial.diffuse[0]),
			static_cast<float>(srcMaterial.diffuse[1]),
			static_cast<float>(srcMaterial.diffuse[2]),
			static_cast<float>(srcMaterial.dissolve > 0.0f ? srcMaterial.dissolve : 1.0f)
		};

		float uScale = static_cast<float>(srcMaterial.diffuse_texopt.scale[0]);
		float vScale = static_cast<float>(srcMaterial.diffuse_texopt.scale[1]);
		if (std::fabs(uScale) < 1e-6f) uScale = 1.0f;
		if (std::fabs(vScale) < 1e-6f) vScale = 1.0f;

		mat.UvTiling = { uScale, vScale };
		mat.UvOffset = {
			static_cast<float>(srcMaterial.diffuse_texopt.origin_offset[0]),
			static_cast<float>(srcMaterial.diffuse_texopt.origin_offset[1])
		};
		mat.WindParams = { 0.0f, 0.0f, 0.0f, 0.0f };
		mat.HasTexture = false;
		mat.TextureIndex = 0;

		std::string materialNameLower = srcMaterial.name;
		std::transform(
			materialNameLower.begin(),
			materialNameLower.end(),
			materialNameLower.begin(),
			[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

		if (materialNameLower.find("leaf") != std::string::npos) {
			mat.WindParams = { 1.0f, 0.020f, 1.45f, 1.8f };
		}

		if (!srcMaterial.diffuse_texname.empty())
		{
			std::filesystem::path texturePath = std::filesystem::u8path(srcMaterial.diffuse_texname);
			if (!texturePath.is_absolute()) {
				texturePath = objPath.parent_path() / texturePath;
			}
			texturePath = texturePath.lexically_normal();

			const std::wstring key = texturePath.wstring();
			auto existing = textureIndexByPath.find(key);
			if (existing != textureIndexByPath.end()) {
				mat.TextureIndex = existing->second;
				mat.HasTexture = true;
			}
			else {
				RgbaImage loadedImage;
				if (LoadImageFromFile(texturePath, loadedImage)) {
					const UINT textureIndex = static_cast<UINT>(textureImages.size());
					textureImages.push_back(std::move(loadedImage));
					textureIndexByPath.emplace(key, textureIndex);

					mat.TextureIndex = textureIndex;
					mat.HasTexture = true;
				}
				else {
					std::wstring warnLine = L"[Texture] Failed to load: ";
					warnLine += texturePath.wstring();
					warnLine += L"\n";
					OutputDebugStringW(warnLine.c_str());
				}
			}
		}

		m_modelMaterials.push_back(mat);
	}

	const bool hasNormals = !attrib.normals.empty();
	const bool hasTexcoords = !attrib.texcoords.empty();

	XMFLOAT3 minP = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
	XMFLOAT3 maxP = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	auto ExpandBounds = [&](const XMFLOAT3& p) {
		if (p.x < minP.x) minP.x = p.x;
		if (p.y < minP.y) minP.y = p.y;
		if (p.z < minP.z) minP.z = p.z;
		if (p.x > maxP.x) maxP.x = p.x;
		if (p.y > maxP.y) maxP.y = p.y;
		if (p.z > maxP.z) maxP.z = p.z;
	};

	auto ReadPos = [&](int vIdx) -> XMFLOAT3 {
		if (vIdx < 0) {
			return { 0.0f, 0.0f, 0.0f };
		}
		const size_t i = static_cast<size_t>(vIdx);
		return {
			attrib.vertices[3 * i + 0],
			attrib.vertices[3 * i + 1],
			attrib.vertices[3 * i + 2]
		};
	};

	auto ReadNormal = [&](int nIdx) -> XMFLOAT3 {
		if (!hasNormals || nIdx < 0) {
			return { 0.0f, 1.0f, 0.0f };
		}
		const size_t i = static_cast<size_t>(nIdx);
		return {
			attrib.normals[3 * i + 0],
			attrib.normals[3 * i + 1],
			attrib.normals[3 * i + 2]
		};
	};

	auto ReadTexCoord = [&](int tIdx) -> XMFLOAT2 {
		if (!hasTexcoords || tIdx < 0) {
			return { 0.0f, 0.0f };
		}
		const size_t i = static_cast<size_t>(tIdx);
		return {
			attrib.texcoords[2 * i + 0],
			1.0f - attrib.texcoords[2 * i + 1]
		};
	};

	std::vector<std::vector<Vertex>> materialVertices(m_modelMaterials.size());

	for (const tinyobj::shape_t& shape : shapes)
	{
		size_t indexOffset = 0;
		for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f)
		{
			const int fv = shape.mesh.num_face_vertices[f];
			if (fv != 3) {
				indexOffset += static_cast<size_t>(fv);
				continue;
			}

			UINT materialIndex = 0;
			if (f < shape.mesh.material_ids.size()) {
				const int tinyMatId = shape.mesh.material_ids[f];
				if (tinyMatId >= 0 && tinyMatId < static_cast<int>(tinyMaterials.size())) {
					materialIndex = static_cast<UINT>(tinyMatId + 1);
				}
			}
			if (materialIndex >= materialVertices.size()) {
				materialIndex = 0;
			}

			Vertex tri[3] = {};
			for (int v = 0; v < 3; ++v)
			{
				const tinyobj::index_t idx = shape.mesh.indices[indexOffset + static_cast<size_t>(v)];
				tri[v].Pos = ReadPos(idx.vertex_index);
				tri[v].Normal = ReadNormal(idx.normal_index);
				tri[v].Color = { 1.0f, 1.0f, 1.0f, 1.0f };
				tri[v].TexC = ReadTexCoord(idx.texcoord_index);
			}

			if (!hasNormals ||
				shape.mesh.indices[indexOffset + 0].normal_index < 0 ||
				shape.mesh.indices[indexOffset + 1].normal_index < 0 ||
				shape.mesh.indices[indexOffset + 2].normal_index < 0)
			{
				const XMVECTOR a = XMLoadFloat3(&tri[0].Pos);
				const XMVECTOR b = XMLoadFloat3(&tri[1].Pos);
				const XMVECTOR c = XMLoadFloat3(&tri[2].Pos);
				const XMVECTOR n = XMVector3Normalize(XMVector3Cross(b - a, c - a));
				XMFLOAT3 normal;
				XMStoreFloat3(&normal, n);
				tri[0].Normal = normal;
				tri[1].Normal = normal;
				tri[2].Normal = normal;
			}

			materialVertices[materialIndex].push_back(tri[0]);
			materialVertices[materialIndex].push_back(tri[1]);
			materialVertices[materialIndex].push_back(tri[2]);

			ExpandBounds(tri[0].Pos);
			ExpandBounds(tri[1].Pos);
			ExpandBounds(tri[2].Pos);

			indexOffset += 3;
		}
	}

	std::vector<Vertex> vertices;
	vertices.reserve(500000);
	for (UINT materialIndex = 0; materialIndex < static_cast<UINT>(materialVertices.size()); ++materialIndex)
	{
		const std::vector<Vertex>& bucket = materialVertices[materialIndex];
		if (bucket.empty()) {
			continue;
		}

		ModelSubset subset = {};
		subset.MaterialIndex = materialIndex;
		subset.StartVertex = static_cast<UINT>(vertices.size());
		subset.VertexCount = static_cast<UINT>(bucket.size());
		m_modelSubsets.push_back(subset);

		vertices.insert(vertices.end(), bucket.begin(), bucket.end());
	}

	if (vertices.empty()) {
		throw std::runtime_error("OBJ loaded but produced 0 vertices.");
	}

	m_modelCenter = {
		0.5f * (minP.x + maxP.x),
		0.5f * (minP.y + maxP.y),
		0.5f * (minP.z + maxP.z)
	};

	float maxDim = maxP.x - minP.x;
	const float dimY = maxP.y - minP.y;
	const float dimZ = maxP.z - minP.z;
	if (dimY > maxDim) maxDim = dimY;
	if (dimZ > maxDim) maxDim = dimZ;
	m_modelScale = (maxDim > 1e-6f) ? (2.0f / maxDim) : 1.0f;

	m_modelVertexCount = static_cast<UINT>(vertices.size());
	const UINT vbByteSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	defaultHeap.CreationNodeMask = 1;
	defaultHeap.VisibleNodeMask = 1;
	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
	uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	uploadHeap.CreationNodeMask = 1;
	uploadHeap.VisibleNodeMask = 1;

	auto MakeBufferDesc = [](UINT64 byteSize) {
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = byteSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		return desc;
	};

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	m_textureResources.reserve(textureImages.size());
	m_textureUploadResources.reserve(textureImages.size());

	for (size_t texIdx = 0; texIdx < textureImages.size(); ++texIdx)
	{
		const RgbaImage& image = textureImages[texIdx];

		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		textureDesc.Width = image.Width;
		textureDesc.Height = image.Height;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		ComPtr<ID3D12Resource> texture;
		ThrowIfFailed(m_device->CreateCommittedResource(
			&defaultHeap,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(texture.GetAddressOf())));

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
		UINT numRows = 0;
		UINT64 uploadSize = 0;
		m_device->GetCopyableFootprints(
			&textureDesc,
			0,
			1,
			0,
			&footprint,
			&numRows,
			nullptr,
			&uploadSize);

		ComPtr<ID3D12Resource> upload;
		D3D12_RESOURCE_DESC uploadDesc = MakeBufferDesc(uploadSize);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&uploadHeap,
			D3D12_HEAP_FLAG_NONE,
			&uploadDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(upload.GetAddressOf())));

		void* mapped = nullptr;
		ThrowIfFailed(upload->Map(0, nullptr, &mapped));
		const UINT srcRowPitch = image.Width * 4;
		for (UINT row = 0; row < numRows; ++row)
		{
			const std::uint8_t* src = image.Pixels.data() + static_cast<size_t>(row) * srcRowPitch;
			std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(mapped)
				+ footprint.Offset
				+ static_cast<size_t>(row) * footprint.Footprint.RowPitch;
			memcpy(dst, src, srcRowPitch);
		}
		upload->Unmap(0, nullptr);

		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = texture.Get();
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLoc.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = upload.Get();
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLoc.PlacedFootprint = footprint;

		m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture.Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		m_commandList->ResourceBarrier(1, &barrier);

		m_textureResources.push_back(texture);
		m_textureUploadResources.push_back(upload);
	}

	const D3D12_RESOURCE_DESC vbDesc = MakeBufferDesc(vbByteSize);
	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(m_modelVB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_modelVBUpload.GetAddressOf())));

	void* vbMapped = nullptr;
	ThrowIfFailed(m_modelVBUpload->Map(0, nullptr, &vbMapped));
	memcpy(vbMapped, vertices.data(), vbByteSize);
	m_modelVBUpload->Unmap(0, nullptr);

	m_commandList->CopyBufferRegion(m_modelVB.Get(), 0, m_modelVBUpload.Get(), 0, vbByteSize);

	D3D12_RESOURCE_BARRIER vbBarrier = {};
	vbBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	vbBarrier.Transition.pResource = m_modelVB.Get();
	vbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	vbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	vbBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &vbBarrier);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdLists);
	FlushCommandQueue();

	m_textureUploadResources.clear();
	m_modelVBUpload.Reset();

	m_modelVBV.BufferLocation = m_modelVB->GetGPUVirtualAddress();
	m_modelVBV.StrideInBytes = sizeof(Vertex);
	m_modelVBV.SizeInBytes = vbByteSize;
#endif

	BuildSceneGeometryUpload();
}

void Framework::BuildSceneGeometryUpload()
{
	using namespace DirectX;

	if (m_sceneDefinitions.empty()) {
		throw std::runtime_error("No scene definitions available.");
	}

	if (m_currentSceneIndex >= m_sceneDefinitions.size()) {
		m_currentSceneIndex = 0;
	}

	const SceneDefinition& scene = m_sceneDefinitions[m_currentSceneIndex];
	if (scene.ModelPaths.empty()) {
		throw std::runtime_error("Scene has no model paths.");
	}

	m_cameraMoveSpeed = scene.CameraMoveSpeed;
	m_tessellationMinDistance = scene.TessellationMinDistance;
	m_tessellationMaxDistance = scene.TessellationMaxDistance;
	m_tessellationMinFactor = scene.TessellationMinFactor;
	m_tessellationMaxFactor = scene.TessellationMaxFactor;

	FlushCommandQueue();

	m_modelMaterials.clear();
	m_modelSubsets.clear();
	m_textureResources.clear();
	m_textureUploadResources.clear();
	m_modelVB.Reset();
	m_modelVBUpload.Reset();
	m_modelVertexCount = 0;

	std::vector<LoadedTextureData> textureImages;
	textureImages.reserve(64);

	const UINT defaultBaseColorIndex = static_cast<UINT>(textureImages.size());
	textureImages.push_back(MakeTextureDataFromRgbaImage(MakeSolidImage(255, 255, 255, 255)));
	const UINT defaultNormalIndex = static_cast<UINT>(textureImages.size());
	textureImages.push_back(MakeTextureDataFromRgbaImage(MakeSolidImage(128, 128, 255, 255)));
	const UINT defaultDisplacementIndex = static_cast<UINT>(textureImages.size());
	textureImages.push_back(MakeTextureDataFromRgbaImage(MakeSolidImage(0, 0, 0, 255)));
	const UINT defaultOpacityIndex = static_cast<UINT>(textureImages.size());
	textureImages.push_back(MakeTextureDataFromRgbaImage(MakeSolidImage(255, 255, 255, 255)));

	std::unordered_map<std::wstring, UINT> textureIndexByPath;
	textureIndexByPath.reserve(256);

	auto AcquireTextureIndex = [&](const std::filesystem::path& texturePath, UINT fallbackIndex) -> UINT
	{
		if (texturePath.empty()) {
			return fallbackIndex;
		}

		const std::filesystem::path resolved = texturePath.lexically_normal();
		const std::wstring key = resolved.wstring();
		auto existing = textureIndexByPath.find(key);
		if (existing != textureIndexByPath.end()) {
			return existing->second;
		}

		LoadedTextureData loadedTexture;
		if (!LoadTextureDataFromFile(resolved, loadedTexture))
		{
			std::wstring warnLine = L"[Texture] Failed to load: ";
			warnLine += resolved.wstring();
			warnLine += L"\n";
			OutputDebugStringW(warnLine.c_str());
			return fallbackIndex;
		}

		const UINT textureIndex = static_cast<UINT>(textureImages.size());
		textureImages.push_back(std::move(loadedTexture));
		textureIndexByPath.emplace(key, textureIndex);
		return textureIndex;
	};

	auto MakeDefaultMaterial = [&]() -> ModelMaterial
	{
		ModelMaterial mat = {};
		mat.TextureIndices = {
			defaultBaseColorIndex,
			defaultNormalIndex,
			defaultDisplacementIndex,
			defaultOpacityIndex
		};
		mat.Flags = 0;
		mat.DisplacementScale = 0.0f;
		mat.DisplacementBias = 0.0f;
		mat.AlphaCutoff = scene.AlphaCutoff;
		mat.SrvBaseIndex = 0;
		return mat;
	};

	auto DisableDisplacement = [&](ModelMaterial& mat)
	{
		mat.Flags &= ~(MaterialFlagHasDisplacementTexture | MaterialFlagDisplacementFromNormal);
		mat.TextureIndices[MaterialTextureDisplacementSlot] = defaultDisplacementIndex;
		mat.DisplacementScale = 0.0f;
		mat.DisplacementBias = 0.0f;
	};

	auto FinalizeMaterial = [&](ModelMaterial& mat, const std::string& materialNameLower)
	{
		mat.AlphaCutoff = scene.AlphaCutoff;

		if (scene.EnableWindAnimation && IsWindMaterial(materialNameLower)) {
			mat.WindParams = { 1.0f, 0.020f, 1.45f, 1.8f };
		}
		else {
			mat.WindParams = { 0.0f, 0.0f, 0.0f, 0.0f };
		}

		if ((mat.Flags & MaterialFlagHasDisplacementTexture) != 0u) {
			mat.DisplacementScale = scene.DefaultDisplacementScale;
			mat.DisplacementBias = scene.DefaultDisplacementBias;
		}
		else {
			DisableDisplacement(mat);
		}

		if (IsWindMaterial(materialNameLower) ||
			IsEmissiveMaterial(materialNameLower) ||
			(mat.Flags & MaterialFlagHasOpacityTexture) != 0u)
		{
			DisableDisplacement(mat);
		}

		if (IsTransparentMaterial(materialNameLower)) {
			mat.AlphaCutoff = 0.0f;
			DisableDisplacement(mat);
		}
	};

	ModelMaterial defaultMaterial = MakeDefaultMaterial();
	defaultMaterial.SrvBaseIndex = 0;
	m_modelMaterials.push_back(defaultMaterial);

	XMFLOAT3 minP = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
	XMFLOAT3 maxP = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	auto ExpandBounds = [&](const XMFLOAT3& p)
	{
		if (p.x < minP.x) minP.x = p.x;
		if (p.y < minP.y) minP.y = p.y;
		if (p.z < minP.z) minP.z = p.z;
		if (p.x > maxP.x) maxP.x = p.x;
		if (p.y > maxP.y) maxP.y = p.y;
		if (p.z > maxP.z) maxP.z = p.z;
	};

	std::vector<std::vector<Vertex>> materialVertices(1);

	for (const std::filesystem::path& modelPath : scene.ModelPaths)
	{
		if (scene.Format == SceneAssetFormat::Obj)
		{
			const std::string objPathUtf8 = modelPath.u8string();
			const std::string baseDirUtf8 = modelPath.parent_path().u8string();

			tinyobj::attrib_t attrib;
			std::vector<tinyobj::shape_t> shapes;
			std::vector<tinyobj::material_t> tinyMaterials;
			std::string warn;
			std::string err;

			const bool ok = tinyobj::LoadObj(
				&attrib, &shapes, &tinyMaterials,
				&warn, &err,
				objPathUtf8.c_str(),
				baseDirUtf8.empty() ? nullptr : baseDirUtf8.c_str(),
				true);

			if (!warn.empty()) {
				OutputDebugStringA(("[tinyobj warn] " + warn + "\n").c_str());
			}
			if (!err.empty()) {
				OutputDebugStringA(("[tinyobj err ] " + err + "\n").c_str());
			}
			if (!ok) {
				throw std::runtime_error("tinyobj::LoadObj failed (see Output window).");
			}

		for (const tinyobj::material_t& srcMaterial : tinyMaterials)
		{
			ModelMaterial mat = MakeDefaultMaterial();
			mat.DiffuseAlbedo = {
				static_cast<float>(srcMaterial.diffuse[0]),
				static_cast<float>(srcMaterial.diffuse[1]),
				static_cast<float>(srcMaterial.diffuse[2]),
				static_cast<float>(srcMaterial.dissolve > 0.0f ? srcMaterial.dissolve : 1.0f)
			};

			float uScale = static_cast<float>(srcMaterial.diffuse_texopt.scale[0]);
			float vScale = static_cast<float>(srcMaterial.diffuse_texopt.scale[1]);
			if (std::fabs(uScale) < 1e-6f) uScale = 1.0f;
			if (std::fabs(vScale) < 1e-6f) vScale = 1.0f;

			mat.UvTiling = { uScale, vScale };
			mat.UvOffset = {
				static_cast<float>(srcMaterial.diffuse_texopt.origin_offset[0]),
				static_cast<float>(srcMaterial.diffuse_texopt.origin_offset[1])
			};

			const std::string materialNameLower = ToLowerCopy(srcMaterial.name);

			const std::filesystem::path baseColorPath = ResolveSceneTexturePathUtf8(modelPath, srcMaterial.diffuse_texname);
			if (!baseColorPath.empty()) {
				mat.TextureIndices[MaterialTextureBaseColorSlot] = AcquireTextureIndex(baseColorPath, defaultBaseColorIndex);
				mat.Flags |= MaterialFlagHasBaseColorTexture;
			}

			const std::filesystem::path explicitNormalPath = ResolveSceneTexturePathUtf8(modelPath, srcMaterial.normal_texname);
			const std::filesystem::path bumpPath = ResolveSceneTexturePathUtf8(modelPath, srcMaterial.bump_texname);
			std::filesystem::path normalPath = explicitNormalPath;
			std::filesystem::path displacementPath = ResolveSceneTexturePathUtf8(modelPath, srcMaterial.displacement_texname);
			const bool bumpActsAsDisplacement =
				scene.AllowKeywordedBumpAsDisplacement &&
				displacementPath.empty() &&
				LooksLikeDisplacementMapPath(bumpPath);

			if (displacementPath.empty() && bumpActsAsDisplacement) {
				displacementPath = bumpPath;
			}

			if (normalPath.empty() && !bumpPath.empty() && !bumpActsAsDisplacement) {
				normalPath = bumpPath;
			}

			const bool displacementLooksLikeNormal = LooksLikeNormalMapPath(displacementPath);
			if (!normalPath.empty()) {
				mat.TextureIndices[MaterialTextureNormalSlot] = AcquireTextureIndex(normalPath, defaultNormalIndex);
				mat.Flags |= MaterialFlagHasNormalTexture;
			}
			else if (!displacementPath.empty() && displacementLooksLikeNormal) {
				mat.TextureIndices[MaterialTextureNormalSlot] = AcquireTextureIndex(displacementPath, defaultNormalIndex);
				mat.Flags |= MaterialFlagHasNormalTexture;
			}

			if (!displacementPath.empty() && !displacementLooksLikeNormal) {
				mat.TextureIndices[MaterialTextureDisplacementSlot] = AcquireTextureIndex(displacementPath, defaultDisplacementIndex);
				mat.Flags |= MaterialFlagHasDisplacementTexture;
			}

			const std::filesystem::path opacityPath = ResolveSceneTexturePathUtf8(modelPath, srcMaterial.alpha_texname);
			if (!opacityPath.empty()) {
				mat.TextureIndices[MaterialTextureOpacitySlot] = AcquireTextureIndex(opacityPath, defaultOpacityIndex);
				mat.Flags |= MaterialFlagHasOpacityTexture;
			}

			FinalizeMaterial(mat, materialNameLower);

			const UINT materialIndex = static_cast<UINT>(m_modelMaterials.size());
			mat.SrvBaseIndex = materialIndex * MaterialTextureSlotCount;
			m_modelMaterials.push_back(mat);
		}

		materialVertices.resize(m_modelMaterials.size());

		const bool hasNormals = !attrib.normals.empty();
		const bool hasTexcoords = !attrib.texcoords.empty();

		auto ReadPos = [&](int vIdx) -> XMFLOAT3
		{
			if (vIdx < 0) {
				return { 0.0f, 0.0f, 0.0f };
			}
			const size_t i = static_cast<size_t>(vIdx);
			return {
				attrib.vertices[3 * i + 0],
				attrib.vertices[3 * i + 1],
				attrib.vertices[3 * i + 2]
			};
		};

		auto ReadNormal = [&](int nIdx) -> XMFLOAT3
		{
			if (!hasNormals || nIdx < 0) {
				return { 0.0f, 0.0f, 0.0f };
			}
			const size_t i = static_cast<size_t>(nIdx);
			return {
				attrib.normals[3 * i + 0],
				attrib.normals[3 * i + 1],
				attrib.normals[3 * i + 2]
			};
		};

		auto ReadTexCoord = [&](int tIdx) -> XMFLOAT2
		{
			if (!hasTexcoords || tIdx < 0) {
				return { 0.0f, 0.0f };
			}
			const size_t i = static_cast<size_t>(tIdx);
			return {
				attrib.texcoords[2 * i + 0],
				1.0f - attrib.texcoords[2 * i + 1]
			};
		};

		for (const tinyobj::shape_t& shape : shapes)
		{
			size_t indexOffset = 0;
			for (size_t faceIndex = 0; faceIndex < shape.mesh.num_face_vertices.size(); ++faceIndex)
			{
				const int fv = shape.mesh.num_face_vertices[faceIndex];
				if (fv != 3) {
					indexOffset += static_cast<size_t>(fv);
					continue;
				}

				UINT materialIndex = 0;
				if (faceIndex < shape.mesh.material_ids.size()) {
					const int tinyMatId = shape.mesh.material_ids[faceIndex];
					if (tinyMatId >= 0 && tinyMatId < static_cast<int>(tinyMaterials.size())) {
						materialIndex = static_cast<UINT>(tinyMatId + 1);
					}
				}
				if (materialIndex >= materialVertices.size()) {
					materialIndex = 0;
				}

				Vertex tri[3] = {};
				for (int vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
				{
					const tinyobj::index_t idx = shape.mesh.indices[indexOffset + static_cast<size_t>(vertexIndex)];
					tri[vertexIndex].Pos = ReadPos(idx.vertex_index);
					tri[vertexIndex].Normal = ReadNormal(idx.normal_index);
					tri[vertexIndex].Color = { 1.0f, 1.0f, 1.0f, 1.0f };
					tri[vertexIndex].TexC = ReadTexCoord(idx.texcoord_index);
				}

				ComputeTriangleNormalAndTangent(tri[0], tri[1], tri[2]);

				materialVertices[materialIndex].push_back(tri[0]);
				materialVertices[materialIndex].push_back(tri[1]);
				materialVertices[materialIndex].push_back(tri[2]);

				ExpandBounds(tri[0].Pos);
				ExpandBounds(tri[1].Pos);
				ExpandBounds(tri[2].Pos);

				indexOffset += 3;
			}
		}
		}
		else
		{
			const std::string modelPathUtf8 = modelPath.u8string();
			ufbx_load_opts opts = {};
			opts.target_axes = ufbx_axes_left_handed_y_up;
			opts.generate_missing_normals = true;
			opts.normalize_normals = true;
			opts.normalize_tangents = true;
			opts.load_external_files = false;
			opts.ignore_missing_external_files = true;
			opts.use_blender_pbr_material = true;

		ufbx_error error = {};
		ufbx_scene* fbxScene = ufbx_load_file_len(modelPathUtf8.c_str(), modelPathUtf8.size(), &opts, &error);
		if (!fbxScene)
		{
			std::string errorText = "ufbx_load_file_len failed";
			if (error.description.data && error.description.length > 0) {
				errorText += ": ";
				errorText.append(error.description.data, error.description.length);
			}
			throw std::runtime_error(errorText);
		}

		std::unordered_map<const ufbx_material*, UINT> materialIndexByPtr;
		materialIndexByPtr.reserve(fbxScene->materials.count);

		auto TryFindNamedTexture = [&](const std::string& materialNameUtf8, const wchar_t* suffix) -> std::filesystem::path
		{
			if (materialNameUtf8.empty()) {
				return {};
			}

			std::filesystem::path candidate = modelPath.parent_path() / L"Textures";
			candidate /= std::filesystem::u8path(materialNameUtf8).wstring() + std::wstring(suffix);
			if (std::filesystem::exists(candidate)) {
				return candidate.lexically_normal();
			}
			return {};
		};

		auto MaterialMapPath = [&](const ufbx_material_map& map) -> std::filesystem::path
		{
			return map.texture ? UfbxTexturePath(modelPath, map.texture) : std::filesystem::path{};
		};

		auto EnsureMaterialIndex = [&](const ufbx_material* srcMaterial) -> UINT
		{
			if (!srcMaterial) {
				return 0;
			}

			auto existing = materialIndexByPtr.find(srcMaterial);
			if (existing != materialIndexByPtr.end()) {
				return existing->second;
			}

			ModelMaterial mat = MakeDefaultMaterial();
			const std::string materialNameUtf8 = (srcMaterial->name.data && srcMaterial->name.length > 0)
				? std::string(srcMaterial->name.data, srcMaterial->name.length)
				: std::string();
			const std::string materialNameLower = ToLowerCopy(materialNameUtf8);

			if (srcMaterial->pbr.base_color.has_value)
			{
				mat.DiffuseAlbedo = {
					static_cast<float>(srcMaterial->pbr.base_color.value_vec4.x),
					static_cast<float>(srcMaterial->pbr.base_color.value_vec4.y),
					static_cast<float>(srcMaterial->pbr.base_color.value_vec4.z),
					(srcMaterial->pbr.base_color.value_components >= 4)
						? static_cast<float>(srcMaterial->pbr.base_color.value_vec4.w)
						: 1.0f
				};
			}
			else if (srcMaterial->fbx.diffuse_color.has_value)
			{
				mat.DiffuseAlbedo = {
					static_cast<float>(srcMaterial->fbx.diffuse_color.value_vec4.x),
					static_cast<float>(srcMaterial->fbx.diffuse_color.value_vec4.y),
					static_cast<float>(srcMaterial->fbx.diffuse_color.value_vec4.z),
					1.0f
				};
			}

			if (srcMaterial->pbr.base_factor.has_value) {
				const float factor = static_cast<float>(srcMaterial->pbr.base_factor.value_real);
				mat.DiffuseAlbedo.x *= factor;
				mat.DiffuseAlbedo.y *= factor;
				mat.DiffuseAlbedo.z *= factor;
			}
			else if (srcMaterial->fbx.diffuse_factor.has_value) {
				const float factor = static_cast<float>(srcMaterial->fbx.diffuse_factor.value_real);
				mat.DiffuseAlbedo.x *= factor;
				mat.DiffuseAlbedo.y *= factor;
				mat.DiffuseAlbedo.z *= factor;
			}

			if (srcMaterial->pbr.opacity.has_value) {
				mat.DiffuseAlbedo.w *= static_cast<float>(srcMaterial->pbr.opacity.value_real);
			}

			std::filesystem::path baseColorPath = MaterialMapPath(srcMaterial->pbr.base_color);
			if (baseColorPath.empty()) baseColorPath = MaterialMapPath(srcMaterial->fbx.diffuse_color);
			if (baseColorPath.empty()) baseColorPath = TryFindNamedTexture(materialNameUtf8, L"_BaseColor.dds");
			if (!baseColorPath.empty()) {
				mat.TextureIndices[MaterialTextureBaseColorSlot] = AcquireTextureIndex(baseColorPath, defaultBaseColorIndex);
				mat.Flags |= MaterialFlagHasBaseColorTexture;
			}

			std::filesystem::path normalPath = MaterialMapPath(srcMaterial->pbr.normal_map);
			if (normalPath.empty()) normalPath = MaterialMapPath(srcMaterial->fbx.normal_map);
			if (normalPath.empty()) normalPath = MaterialMapPath(srcMaterial->fbx.bump);
			if (normalPath.empty()) normalPath = TryFindNamedTexture(materialNameUtf8, L"_Normal.dds");
			if (!normalPath.empty()) {
				mat.TextureIndices[MaterialTextureNormalSlot] = AcquireTextureIndex(normalPath, defaultNormalIndex);
				mat.Flags |= MaterialFlagHasNormalTexture;
			}

			std::filesystem::path displacementPath = MaterialMapPath(srcMaterial->pbr.displacement_map);
			if (displacementPath.empty()) displacementPath = MaterialMapPath(srcMaterial->fbx.displacement);
			const bool displacementLooksLikeNormal = LooksLikeNormalMapPath(displacementPath);
			if (normalPath.empty() && !displacementPath.empty() && displacementLooksLikeNormal) {
				mat.TextureIndices[MaterialTextureNormalSlot] = AcquireTextureIndex(displacementPath, defaultNormalIndex);
				mat.Flags |= MaterialFlagHasNormalTexture;
			}
			if (!displacementPath.empty() && !displacementLooksLikeNormal) {
				mat.TextureIndices[MaterialTextureDisplacementSlot] = AcquireTextureIndex(displacementPath, defaultDisplacementIndex);
				mat.Flags |= MaterialFlagHasDisplacementTexture;
			}

			std::filesystem::path opacityPath = MaterialMapPath(srcMaterial->pbr.opacity);
			if (!opacityPath.empty()) {
				mat.TextureIndices[MaterialTextureOpacitySlot] = AcquireTextureIndex(opacityPath, defaultOpacityIndex);
				mat.Flags |= MaterialFlagHasOpacityTexture;
			}

			FinalizeMaterial(mat, materialNameLower);

			const UINT materialIndex = static_cast<UINT>(m_modelMaterials.size());
			mat.SrvBaseIndex = materialIndex * MaterialTextureSlotCount;
			m_modelMaterials.push_back(mat);
			materialVertices.resize(m_modelMaterials.size());
			materialIndexByPtr.emplace(srcMaterial, materialIndex);
			return materialIndex;
		};

		for (const ufbx_node* node : fbxScene->nodes)
		{
			if (!node->mesh || !node->mesh->vertex_position.exists) {
				continue;
			}

			const ufbx_mesh* mesh = node->mesh;
			const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&node->geometry_to_world);
			std::vector<uint32_t> triIndices(std::max<size_t>(3, mesh->max_face_triangles * 3));

			for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
			{
				const ufbx_face face = mesh->faces[faceIndex];
				const uint32_t numTriangles = ufbx_triangulate_face(triIndices.data(), triIndices.size(), mesh, face);
				if (numTriangles == 0) {
					continue;
				}

				UINT materialIndex = 0;
				if (faceIndex < mesh->face_material.count)
				{
					const uint32_t materialSlot = mesh->face_material[faceIndex];
					if (materialSlot != UFBX_NO_INDEX)
					{
						if (materialSlot < node->materials.count) {
							materialIndex = EnsureMaterialIndex(node->materials[materialSlot]);
						}
						else if (materialSlot < mesh->materials.count) {
							materialIndex = EnsureMaterialIndex(mesh->materials[materialSlot]);
						}
					}
				}

				for (uint32_t triangleIndex = 0; triangleIndex < numTriangles; ++triangleIndex)
				{
					Vertex tri[3] = {};
					for (uint32_t vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
					{
						const uint32_t index = triIndices[triangleIndex * 3 + vertexIndex];
						const ufbx_vec3 posLocal = mesh->vertex_position[index];
						const ufbx_vec3 posWorld = ufbx_transform_position(&node->geometry_to_world, posLocal);

						tri[vertexIndex].Pos = {
							static_cast<float>(posWorld.x),
							static_cast<float>(posWorld.y),
							static_cast<float>(posWorld.z)
						};

						if (mesh->vertex_normal.exists)
						{
							const ufbx_vec3 normalLocal = mesh->vertex_normal[index];
							const ufbx_vec3 normalWorld = ufbx_transform_direction(&normalMatrix, normalLocal);
							tri[vertexIndex].Normal = NormalizeOrFallback(
								{
									static_cast<float>(normalWorld.x),
									static_cast<float>(normalWorld.y),
									static_cast<float>(normalWorld.z)
								},
								{ 0.0f, 1.0f, 0.0f });
						}
						else
						{
							tri[vertexIndex].Normal = { 0.0f, 0.0f, 0.0f };
						}

						if (mesh->vertex_uv.exists)
						{
							const ufbx_vec2 uv = mesh->vertex_uv[index];
							tri[vertexIndex].TexC = {
								static_cast<float>(uv.x),
								1.0f - static_cast<float>(uv.y)
							};
						}

						if (mesh->vertex_color.exists)
						{
							const ufbx_vec4 color = mesh->vertex_color[index];
							tri[vertexIndex].Color = {
								static_cast<float>(color.x),
								static_cast<float>(color.y),
								static_cast<float>(color.z),
								static_cast<float>(color.w)
							};
						}
						else
						{
							tri[vertexIndex].Color = { 1.0f, 1.0f, 1.0f, 1.0f };
						}
					}

					ComputeTriangleNormalAndTangent(tri[0], tri[1], tri[2]);

					materialVertices[materialIndex].push_back(tri[0]);
					materialVertices[materialIndex].push_back(tri[1]);
					materialVertices[materialIndex].push_back(tri[2]);

					ExpandBounds(tri[0].Pos);
					ExpandBounds(tri[1].Pos);
					ExpandBounds(tri[2].Pos);
				}
			}
		}

			ufbx_free_scene(fbxScene);
		}
	}

	std::vector<Vertex> vertices;
	for (UINT materialIndex = 0; materialIndex < static_cast<UINT>(materialVertices.size()); ++materialIndex)
	{
		const std::vector<Vertex>& bucket = materialVertices[materialIndex];
		if (bucket.empty()) {
			continue;
		}

		ModelSubset subset = {};
		subset.MaterialIndex = materialIndex;
		subset.StartVertex = static_cast<UINT>(vertices.size());
		subset.VertexCount = static_cast<UINT>(bucket.size());
		m_modelSubsets.push_back(subset);

		vertices.insert(vertices.end(), bucket.begin(), bucket.end());
	}

	if (vertices.empty()) {
		throw std::runtime_error("Scene loaded but produced 0 vertices.");
	}

	m_modelCenter = {
		0.5f * (minP.x + maxP.x),
		0.5f * (minP.y + maxP.y),
		0.5f * (minP.z + maxP.z)
	};

	float maxDim = maxP.x - minP.x;
	maxDim = (std::max)(maxDim, maxP.y - minP.y);
	maxDim = (std::max)(maxDim, maxP.z - minP.z);
	m_modelScale = (maxDim > 1e-6f) ? (2.0f / maxDim) : 1.0f;

	m_modelVertexCount = static_cast<UINT>(vertices.size());
	const UINT vbByteSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	defaultHeap.CreationNodeMask = 1;
	defaultHeap.VisibleNodeMask = 1;

	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
	uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	uploadHeap.CreationNodeMask = 1;
	uploadHeap.VisibleNodeMask = 1;

	auto MakeBufferDesc = [](UINT64 byteSize)
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = byteSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		return desc;
	};

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	m_textureResources.reserve(textureImages.size());
	m_textureUploadResources.reserve(textureImages.size());

	for (size_t texIdx = 0; texIdx < textureImages.size(); ++texIdx)
	{
		const LoadedTextureData& image = textureImages[texIdx];
		const UINT subresourceCount = static_cast<UINT>(image.Subresources.size());
		if (subresourceCount == 0) {
			continue;
		}

		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		textureDesc.Width = image.Width;
		textureDesc.Height = image.Height;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.MipLevels = static_cast<UINT16>(image.MipLevels);
		textureDesc.Format = image.Format;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		ComPtr<ID3D12Resource> texture;
		ThrowIfFailed(m_device->CreateCommittedResource(
			&defaultHeap,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(texture.GetAddressOf())));

		std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
		std::vector<UINT> numRows(subresourceCount);
		std::vector<UINT64> rowSizes(subresourceCount);
		UINT64 uploadSize = 0;
		m_device->GetCopyableFootprints(
			&textureDesc,
			0,
			subresourceCount,
			0,
			footprints.data(),
			numRows.data(),
			rowSizes.data(),
			&uploadSize);

		ComPtr<ID3D12Resource> upload;
		D3D12_RESOURCE_DESC uploadDesc = MakeBufferDesc(uploadSize);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&uploadHeap,
			D3D12_HEAP_FLAG_NONE,
			&uploadDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(upload.GetAddressOf())));

		void* mapped = nullptr;
		ThrowIfFailed(upload->Map(0, nullptr, &mapped));
		for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
		{
			const D3D12_SUBRESOURCE_DATA& srcData = image.Subresources[subresourceIndex];
			const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& dstFootprint = footprints[subresourceIndex];
			const size_t copyRowBytes = static_cast<size_t>(rowSizes[subresourceIndex]);
			const std::uint8_t* srcBase = static_cast<const std::uint8_t*>(srcData.pData);

			for (UINT row = 0; row < numRows[subresourceIndex]; ++row)
			{
				const std::uint8_t* src = srcBase + static_cast<size_t>(row) * static_cast<size_t>(srcData.RowPitch);
				std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(mapped)
					+ dstFootprint.Offset
					+ static_cast<size_t>(row) * static_cast<size_t>(dstFootprint.Footprint.RowPitch);
				memcpy(dst, src, copyRowBytes);
			}
		}
		upload->Unmap(0, nullptr);

		for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
		{
			D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
			dstLoc.pResource = texture.Get();
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLoc.SubresourceIndex = subresourceIndex;

			D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
			srcLoc.pResource = upload.Get();
			srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLoc.PlacedFootprint = footprints[subresourceIndex];

			m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
		}

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture.Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		m_commandList->ResourceBarrier(1, &barrier);

		m_textureResources.push_back(texture);
		m_textureUploadResources.push_back(upload);
	}

	const D3D12_RESOURCE_DESC vbDesc = MakeBufferDesc(vbByteSize);
	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(m_modelVB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_modelVBUpload.GetAddressOf())));

	void* vbMapped = nullptr;
	ThrowIfFailed(m_modelVBUpload->Map(0, nullptr, &vbMapped));
	memcpy(vbMapped, vertices.data(), vbByteSize);
	m_modelVBUpload->Unmap(0, nullptr);

	D3D12_RESOURCE_BARRIER vbToCopyDest = {};
	vbToCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	vbToCopyDest.Transition.pResource = m_modelVB.Get();
	vbToCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	vbToCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	vbToCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &vbToCopyDest);

	m_commandList->CopyBufferRegion(m_modelVB.Get(), 0, m_modelVBUpload.Get(), 0, vbByteSize);

	D3D12_RESOURCE_BARRIER vbBarrier = {};
	vbBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	vbBarrier.Transition.pResource = m_modelVB.Get();
	vbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	vbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	vbBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &vbBarrier);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdLists);
	FlushCommandQueue();

	m_textureUploadResources.clear();
	m_modelVBUpload.Reset();

	m_modelVBV.BufferLocation = m_modelVB->GetGPUVirtualAddress();
	m_modelVBV.StrideInBytes = sizeof(Vertex);
	m_modelVBV.SizeInBytes = vbByteSize;
}

void Framework::OnMouseDown(HWND hwnd, WPARAM btnState, int x, int y)
{
	if (btnState & MK_RBUTTON)
	{
		m_rmbDown = true;
		m_lastMousePos.x = x;
		m_lastMousePos.y = y;

		// Захват мыши, чтобы события шли даже если вышли за окно
		SetCapture(hwnd);

		// Опционально: скрыть курсор
		// ShowCursor(FALSE);
	}
}

void Framework::OnMouseUp(HWND hwnd, WPARAM btnState, int x, int y)
{
	(void)btnState; (void)x; (void)y;

	if (m_rmbDown)
	{
		m_rmbDown = false;
		ReleaseCapture();

		// Опционально вернуть курсор
		// ShowCursor(TRUE);
	}
}

void Framework::OnMouseMove(HWND hwnd,	WPARAM btnState, int x, int y)
{
	(void)btnState;

	if (!m_rmbDown)
	{
		m_lastMousePos.x = x;
		m_lastMousePos.y = y;
		return;
	}

	int dx = x - m_lastMousePos.x;
	int dy = y - m_lastMousePos.y;

	m_lastMousePos.x = x;
	m_lastMousePos.y = y;

	// yaw: вправо -> положительный
	m_yaw += dx * m_mouseSensitivity;

	// pitch: вверх обычно уменьшает y (dy отрицательный),
	// поэтому делаем "-" чтобы вверх => положительный pitch
	m_pitch -= dy * m_mouseSensitivity;

	// Ограничим pitch, чтобы не переворачивалась камера
	const float limit = DirectX::XM_PIDIV2 - 0.1f; // ~ 89°
	if (m_pitch > limit) m_pitch = limit;
	if (m_pitch < -limit) m_pitch = -limit;

	using namespace DirectX;

	// Вектор "вперёд" из yaw/pitch (LH система)
	XMVECTOR forward = XMVectorSet(
		cosf(m_pitch) * sinf(m_yaw),
		sinf(m_pitch),
		cosf(m_pitch) * cosf(m_yaw),
		0.0f);

	forward = XMVector3Normalize(forward);

	XMVECTOR pos = XMLoadFloat3(&m_camPos);
	XMVECTOR tgt = pos + forward;

	XMStoreFloat3(&m_camTarget, tgt);
}

