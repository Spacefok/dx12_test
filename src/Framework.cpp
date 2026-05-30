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
#include <cstring>
#include <random>
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

ObjectConstants MakeObjectConstantsFromWorld(DirectX::FXMMATRIX world)
{
	ObjectConstants obj = {};
	DirectX::XMStoreFloat4x4(&obj.World, DirectX::XMMatrixTranspose(world));
	DirectX::XMStoreFloat4x4(&obj.WorldInvTranspose, DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, world)));
	return obj;
}

void AppendDoubleSidedQuad(
	std::vector<Vertex>& vertices,
	const DirectX::XMFLOAT3& p0,
	const DirectX::XMFLOAT3& p1,
	const DirectX::XMFLOAT3& p2,
	const DirectX::XMFLOAT3& p3)
{
	Vertex v0 = {};
	v0.Pos = p0;
	v0.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
	v0.TexC = { 0.0f, 1.0f };

	Vertex v1 = v0;
	v1.Pos = p1;
	v1.TexC = { 0.0f, 0.0f };

	Vertex v2 = v0;
	v2.Pos = p2;
	v2.TexC = { 1.0f, 0.0f };

	Vertex v3 = v0;
	v3.Pos = p3;
	v3.TexC = { 1.0f, 1.0f };

	Vertex front0 = v0;
	Vertex front1 = v1;
	Vertex front2 = v2;
	ComputeTriangleNormalAndTangent(front0, front1, front2);
	vertices.push_back(front0);
	vertices.push_back(front1);
	vertices.push_back(front2);

	Vertex front3a = v0;
	Vertex front3b = v2;
	Vertex front3c = v3;
	ComputeTriangleNormalAndTangent(front3a, front3b, front3c);
	vertices.push_back(front3a);
	vertices.push_back(front3b);
	vertices.push_back(front3c);

	Vertex back0 = v0;
	Vertex back1 = v2;
	Vertex back2 = v1;
	ComputeTriangleNormalAndTangent(back0, back1, back2);
	vertices.push_back(back0);
	vertices.push_back(back1);
	vertices.push_back(back2);

	Vertex back3a = v0;
	Vertex back3b = v3;
	Vertex back3c = v2;
	ComputeTriangleNormalAndTangent(back3a, back3b, back3c);
	vertices.push_back(back3a);
	vertices.push_back(back3b);
	vertices.push_back(back3c);
}

enum class AabbFrustumRelation {
	Outside,
	Intersects,
	Inside,
};

Framework::Aabb MakeInvalidAabb()
{
	return {
		{ +FLT_MAX, +FLT_MAX, +FLT_MAX },
		{ -FLT_MAX, -FLT_MAX, -FLT_MAX }
	};
}

bool IsAabbValid(const Framework::Aabb& bounds)
{
	return bounds.Min.x <= bounds.Max.x
		&& bounds.Min.y <= bounds.Max.y
		&& bounds.Min.z <= bounds.Max.z;
}

void ExpandAabb(Framework::Aabb& bounds, const DirectX::XMFLOAT3& point)
{
	bounds.Min.x = (std::min)(bounds.Min.x, point.x);
	bounds.Min.y = (std::min)(bounds.Min.y, point.y);
	bounds.Min.z = (std::min)(bounds.Min.z, point.z);
	bounds.Max.x = (std::max)(bounds.Max.x, point.x);
	bounds.Max.y = (std::max)(bounds.Max.y, point.y);
	bounds.Max.z = (std::max)(bounds.Max.z, point.z);
}

void ExpandAabb(Framework::Aabb& bounds, const Framework::Aabb& other)
{
	if (!IsAabbValid(other)) {
		return;
	}

	ExpandAabb(bounds, other.Min);
	ExpandAabb(bounds, other.Max);
}

DirectX::XMFLOAT3 AabbCenter(const Framework::Aabb& bounds)
{
	return {
		0.5f * (bounds.Min.x + bounds.Max.x),
		0.5f * (bounds.Min.y + bounds.Max.y),
		0.5f * (bounds.Min.z + bounds.Max.z)
	};
}

DirectX::XMFLOAT3 AabbExtents(const Framework::Aabb& bounds)
{
	return {
		0.5f * (bounds.Max.x - bounds.Min.x),
		0.5f * (bounds.Max.y - bounds.Min.y),
		0.5f * (bounds.Max.z - bounds.Min.z)
	};
}

float AabbFootprintArea(const Framework::Aabb& bounds)
{
	if (!IsAabbValid(bounds)) {
		return 0.0f;
	}

	const float width = (std::max)(bounds.Max.x - bounds.Min.x, 0.0f);
	const float depth = (std::max)(bounds.Max.z - bounds.Min.z, 0.0f);
	return width * depth;
}

bool TrySelectRainCollisionBounds(
	const std::vector<Framework::SceneObject>& sceneObjects,
	Framework::Aabb& outBounds)
{
	for (const Framework::SceneObject& sceneObject : sceneObjects)
	{
		if (sceneObject.Geometry == Framework::SceneObjectGeometry::SceneModel &&
			IsAabbValid(sceneObject.Bounds))
		{
			outBounds = sceneObject.Bounds;
			return true;
		}
	}

	float bestFootprintArea = 0.0f;
	bool foundBounds = false;
	for (const Framework::SceneObject& sceneObject : sceneObjects)
	{
		if (!IsAabbValid(sceneObject.Bounds)) {
			continue;
		}

		const float footprintArea = AabbFootprintArea(sceneObject.Bounds);
		if (footprintArea > bestFootprintArea)
		{
			bestFootprintArea = footprintArea;
			outBounds = sceneObject.Bounds;
			foundBounds = true;
		}
	}

	return foundBounds;
}

Framework::Aabb MakeAabbFromCenterExtents(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& extents)
{
	return {
		{ center.x - extents.x, center.y - extents.y, center.z - extents.z },
		{ center.x + extents.x, center.y + extents.y, center.z + extents.z }
	};
}

Framework::Aabb BuildChildAabb(const Framework::Aabb& parentBounds, int childIndex)
{
	const DirectX::XMFLOAT3 center = AabbCenter(parentBounds);
	const DirectX::XMFLOAT3 extents = AabbExtents(parentBounds);
	const DirectX::XMFLOAT3 childExtents = {
		extents.x * 0.5f,
		extents.y * 0.5f,
		extents.z * 0.5f
	};

	const float offsetX = (childIndex & 1) ? childExtents.x : -childExtents.x;
	const float offsetY = (childIndex & 2) ? childExtents.y : -childExtents.y;
	const float offsetZ = (childIndex & 4) ? childExtents.z : -childExtents.z;

	return MakeAabbFromCenterExtents(
		{ center.x + offsetX, center.y + offsetY, center.z + offsetZ },
		childExtents);
}

bool AabbFitsInside(const Framework::Aabb& objectBounds, const Framework::Aabb& containerBounds)
{
	return objectBounds.Min.x >= containerBounds.Min.x
		&& objectBounds.Min.y >= containerBounds.Min.y
		&& objectBounds.Min.z >= containerBounds.Min.z
		&& objectBounds.Max.x <= containerBounds.Max.x
		&& objectBounds.Max.y <= containerBounds.Max.y
		&& objectBounds.Max.z <= containerBounds.Max.z;
}

bool AabbContainsPoint(const Framework::Aabb& bounds, const DirectX::XMFLOAT3& point)
{
	return point.x >= bounds.Min.x && point.x <= bounds.Max.x
		&& point.y >= bounds.Min.y && point.y <= bounds.Max.y
		&& point.z >= bounds.Min.z && point.z <= bounds.Max.z;
}

std::array<DirectX::XMFLOAT4, 6> ExtractFrustumPlanes(DirectX::FXMMATRIX viewProj)
{
	using namespace DirectX;

	XMFLOAT4X4 m;
	XMStoreFloat4x4(&m, viewProj);

	std::array<XMFLOAT4, 6> planes = {
		XMFLOAT4{ m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 }, // left
		XMFLOAT4{ m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 }, // right
		XMFLOAT4{ m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 }, // bottom
		XMFLOAT4{ m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 }, // top
		XMFLOAT4{ m._13,          m._23,          m._33,          m._43 },         // near
		XMFLOAT4{ m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 }, // far
	};

	for (DirectX::XMFLOAT4& plane : planes)
	{
		const float lengthSq = plane.x * plane.x + plane.y * plane.y + plane.z * plane.z;
		if (lengthSq <= 1e-8f) {
			continue;
		}

		const float invLength = 1.0f / std::sqrt(lengthSq);
		plane.x *= invLength;
		plane.y *= invLength;
		plane.z *= invLength;
		plane.w *= invLength;
	}

	return planes;
}

AabbFrustumRelation ClassifyAabbAgainstFrustum(
	const Framework::Aabb& bounds,
	const std::array<DirectX::XMFLOAT4, 6>& planes)
{
	const DirectX::XMFLOAT3 center = AabbCenter(bounds);
	const DirectX::XMFLOAT3 extents = AabbExtents(bounds);

	bool intersects = false;

	for (const DirectX::XMFLOAT4& plane : planes)
	{
		const float distanceToCenter =
			plane.x * center.x +
			plane.y * center.y +
			plane.z * center.z +
			plane.w;

		const float projectedRadius =
			std::fabs(plane.x) * extents.x +
			std::fabs(plane.y) * extents.y +
			std::fabs(plane.z) * extents.z;

		if (distanceToCenter + projectedRadius < 0.0f) {
			return AabbFrustumRelation::Outside;
		}

		if (distanceToCenter - projectedRadius < 0.0f) {
			intersects = true;
		}
	}

	return intersects ? AabbFrustumRelation::Intersects : AabbFrustumRelation::Inside;
}

float RandomRange(std::mt19937& rng, float minValue, float maxValue)
{
	if (maxValue <= minValue) {
		return minValue;
	}

	std::uniform_real_distribution<float> distribution(minValue, maxValue);
	return distribution(rng);
}

std::wstring CullingModeLabel(bool enableFrustumCulling, bool useOctreeForCulling)
{
	if (!enableFrustumCulling) {
		return L"Off";
	}

	return useOctreeForCulling ? L"Octree" : L"Linear";
}

struct OcclusionScreenRect {
	int MinX = 0;
	int MinY = 0;
	int MaxX = 0;
	int MaxY = 0;
	float NearDepth = 0.0f;
	float FarDepth = 1.0f;
};

bool ProjectAabbToScreenRect(
	const Framework::Aabb& bounds,
	DirectX::FXMMATRIX viewProj,
	const DirectX::XMFLOAT3& eyePos,
	int rasterWidth,
	int rasterHeight,
	OcclusionScreenRect& outRect)
{
	using namespace DirectX;

	if (AabbContainsPoint(bounds, eyePos))
	{
		outRect.MinX = 0;
		outRect.MinY = 0;
		outRect.MaxX = rasterWidth - 1;
		outRect.MaxY = rasterHeight - 1;
		outRect.NearDepth = 0.0f;
		outRect.FarDepth = 0.02f;
		return true;
	}

	const XMFLOAT3 corners[8] = {
		{ bounds.Min.x, bounds.Min.y, bounds.Min.z },
		{ bounds.Max.x, bounds.Min.y, bounds.Min.z },
		{ bounds.Min.x, bounds.Max.y, bounds.Min.z },
		{ bounds.Max.x, bounds.Max.y, bounds.Min.z },
		{ bounds.Min.x, bounds.Min.y, bounds.Max.z },
		{ bounds.Max.x, bounds.Min.y, bounds.Max.z },
		{ bounds.Min.x, bounds.Max.y, bounds.Max.z },
		{ bounds.Max.x, bounds.Max.y, bounds.Max.z },
	};

	float minScreenX = static_cast<float>(rasterWidth);
	float minScreenY = static_cast<float>(rasterHeight);
	float maxScreenX = 0.0f;
	float maxScreenY = 0.0f;
	float minDepth = 1.0f;
	float maxDepth = 0.0f;
	bool hasProjectedCorner = false;

	for (const XMFLOAT3& corner : corners)
	{
		const XMVECTOR clip = XMVector4Transform(XMVectorSet(corner.x, corner.y, corner.z, 1.0f), viewProj);
		const float w = XMVectorGetW(clip);
		const float safeW = (std::max)(w, 1e-4f);
		const float invW = 1.0f / safeW;
		const float ndcX = XMVectorGetX(clip) * invW;
		const float ndcY = XMVectorGetY(clip) * invW;
		const float ndcZ = XMVectorGetZ(clip) * invW;

		const float screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(rasterWidth);
		const float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(rasterHeight);

		minScreenX = (std::min)(minScreenX, screenX);
		minScreenY = (std::min)(minScreenY, screenY);
		maxScreenX = (std::max)(maxScreenX, screenX);
		maxScreenY = (std::max)(maxScreenY, screenY);
		minDepth = (std::min)(minDepth, std::clamp(ndcZ, 0.0f, 1.0f));
		maxDepth = (std::max)(maxDepth, std::clamp(ndcZ, 0.0f, 1.0f));
		hasProjectedCorner = true;
	}

	if (!hasProjectedCorner) {
		return false;
	}

	const int minX = std::clamp(static_cast<int>(std::floor(minScreenX)), 0, rasterWidth - 1);
	const int minY = std::clamp(static_cast<int>(std::floor(minScreenY)), 0, rasterHeight - 1);
	const int maxX = std::clamp(static_cast<int>(std::ceil(maxScreenX)), 0, rasterWidth - 1);
	const int maxY = std::clamp(static_cast<int>(std::ceil(maxScreenY)), 0, rasterHeight - 1);

	if (minX > maxX || minY > maxY) {
		return false;
	}

	outRect.MinX = minX;
	outRect.MinY = minY;
	outRect.MaxX = maxX;
	outRect.MaxY = maxY;
	outRect.NearDepth = minDepth;
	outRect.FarDepth = maxDepth;
	return true;
}

bool IsOccludedByDepthPyramid(
	const std::vector<float>& depthBuffer,
	int rasterWidth,
	const OcclusionScreenRect& rect)
{
	const float bias = 0.0015f;
	for (int y = rect.MinY; y <= rect.MaxY; ++y)
	{
		const int rowOffset = y * rasterWidth;
		for (int x = rect.MinX; x <= rect.MaxX; ++x)
		{
			if (depthBuffer[rowOffset + x] > rect.NearDepth - bias) {
				return false;
			}
		}
	}

	return true;
}

void RasterizeOccluderToDepthPyramid(
	std::vector<float>& depthBuffer,
	int rasterWidth,
	const OcclusionScreenRect& rect)
{
	int minX = rect.MinX;
	int minY = rect.MinY;
	int maxX = rect.MaxX;
	int maxY = rect.MaxY;

	const int padX = (maxX - minX + 1) / 6;
	const int padY = (maxY - minY + 1) / 6;
	if (minX + padX <= maxX - padX) {
		minX += padX;
		maxX -= padX;
	}
	if (minY + padY <= maxY - padY) {
		minY += padY;
		maxY -= padY;
	}

	for (int y = minY; y <= maxY; ++y)
	{
		const int rowOffset = y * rasterWidth;
		for (int x = minX; x <= maxX; ++x) {
			depthBuffer[rowOffset + x] = (std::min)(depthBuffer[rowOffset + x], rect.FarDepth);
		}
	}
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
	BuildShadowResources();
	BuildShaders();
	BuildConstantBuffers();
	BuildBoxGeometry();
	InitializeSceneDefinitions();
	BuildSceneGeometryUpload();
	BuildParticleSystem();
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
				UpdateWindowTitle();
			}
			else if (vk == VK_F2)
			{
				m_enableFrustumCulling = !m_enableFrustumCulling;
				if (!m_enableFrustumCulling) {
					m_useOctreeForCulling = false;
				}
				UpdateWindowTitle();
			}
			else if (vk == VK_F3)
			{
				m_useOctreeForCulling = !m_useOctreeForCulling;
				if (m_useOctreeForCulling) {
					m_enableFrustumCulling = true;
				}
				UpdateWindowTitle();
			}
			else if (vk == VK_F4)
			{
				m_enableOcclusionCulling = !m_enableOcclusionCulling;
				UpdateWindowTitle();
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
	dsvHeapDesc.NumDescriptors = 1 + ShadowCascadeCount;
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
	XMMATRIX proj = XMMatrixPerspectiveFovLH(CameraFovY, aspect, CameraNearZ, CameraFarZ);

	XMMATRIX viewProj = view * proj;
	UpdateDynamicSceneObjects();
	UpdateVisibleObjects(viewProj);
	UpdateCascadedShadowMaps(view);

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
	XMStoreFloat4x4(&deferred.View, XMMatrixTranspose(view));
	for (UINT cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex) {
		deferred.ShadowViewProj[cascadeIndex] = m_shadowViewProj[cascadeIndex];
	}
	deferred.ShadowCascadeSplits = {
		m_shadowCascadeSplits[0],
		m_shadowCascadeSplits[1],
		m_shadowCascadeSplits[2],
		m_shadowCascadeSplits[3]
	};
	deferred.ShadowParams = {
		1.0f / static_cast<float>(ShadowMapSize),
		(m_shadowMap && m_directionalLightCount > 0) ? 1.0f : 0.0f,
		0.0007f,
		static_cast<float>(ShadowCascadeCount)
	};

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
	UpdateParticleSimConstants(dt);
	UpdateWindowTitle();
}

void Framework::UpdateDynamicSceneObjects()
{
	using namespace DirectX;

	if (!m_objectCB || m_sceneObjects.empty()) {
		return;
	}

	for (UINT objectIndex = 0; objectIndex < static_cast<UINT>(m_sceneObjects.size()); ++objectIndex)
	{
		SceneObject& sceneObject = m_sceneObjects[objectIndex];
		if (sceneObject.Geometry != SceneObjectGeometry::TreeBillboard) {
			continue;
		}

		XMFLOAT3 toCamera = {
			m_camPos.x - sceneObject.Anchor.x,
			0.0f,
			m_camPos.z - sceneObject.Anchor.z
		};

		const float lengthSq = toCamera.x * toCamera.x + toCamera.z * toCamera.z;
		if (lengthSq <= 1e-6f) {
			toCamera = { 0.0f, 0.0f, 1.0f };
		}

		const float yaw = std::atan2(toCamera.x, toCamera.z);
		const XMMATRIX world =
			XMMatrixScaling(sceneObject.BillboardScale.x, sceneObject.BillboardScale.y, sceneObject.BillboardScale.x) *
			XMMatrixRotationY(yaw) *
			XMMatrixTranslation(sceneObject.Anchor.x, sceneObject.Anchor.y, sceneObject.Anchor.z);

		sceneObject.Constants = MakeObjectConstantsFromWorld(world);
		m_objectCB->CopyData(objectIndex, sceneObject.Constants);
	}
}

void Framework::Draw()
{
	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	m_commandList->RSSetViewports(1, &m_screenViewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);

	ID3D12DescriptorHeap* heaps[] = { m_cbvHeap.Get() };
	m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);

	SimulateParticles();
	SortParticlesOnGpu();
	RenderSceneToShadowMap();

	m_commandList->RSSetViewports(1, &m_screenViewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);
	m_gbuffer.TransitionToRenderTargets(m_commandList.Get());

	m_commandList->SetGraphicsRootSignature(m_renderingSystem.GeometryRootSignature());

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
		mat.WaterParams = srcMaterial.WaterParams;

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

	if (!m_sceneObjects.empty())
	{
		bool boxPipelineBound = false;
		bool boxGeometryBound = false;

		for (UINT objectIndex : m_visibleOpaqueObjectIndices)
		{
			if (objectIndex >= m_sceneObjects.size()) {
				continue;
			}

			const SceneObject& sceneObject = m_sceneObjects[objectIndex];
			m_commandList->SetGraphicsRootDescriptorTable(0, ObjectPassGpuHandle(objectIndex));

			if (sceneObject.Geometry == SceneObjectGeometry::SceneModel && m_modelVB && !m_modelSubsets.empty())
			{
				m_commandList->IASetVertexBuffers(0, 1, &m_modelVBV);
				bool tessellationPipelineActive = false;
				bool pipelineInitialized = false;

				for (const ModelSubset& subset : m_modelSubsets)
				{
					const UINT materialIndex = (subset.MaterialIndex < m_modelMaterials.size()) ? subset.MaterialIndex : 0;
					const ModelMaterial& material = m_modelMaterials[materialIndex];
					const bool usesTessellation = (material.Flags & MaterialFlagUseTessellation) != 0u;

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
			else if (sceneObject.Geometry == SceneObjectGeometry::TreeModel && m_treeVB && !m_treeModelSubsets.empty())
			{
				m_commandList->IASetVertexBuffers(0, 1, &m_treeVBV);
				bool tessellationPipelineActive = false;
				bool pipelineInitialized = false;

				for (const ModelSubset& subset : m_treeModelSubsets)
				{
					const UINT materialIndex = (subset.MaterialIndex < m_modelMaterials.size()) ? subset.MaterialIndex : 0;
					const ModelMaterial& material = m_modelMaterials[materialIndex];
					const bool usesTessellation = (material.Flags & MaterialFlagUseTessellation) != 0u;

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
			else if (sceneObject.Geometry == SceneObjectGeometry::TreeBillboard && m_treeBillboardVB && !m_treeBillboardSubsets.empty())
			{
				m_commandList->SetPipelineState(m_renderingSystem.GeometryBasicPSO());
				m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				m_commandList->IASetVertexBuffers(0, 1, &m_treeBillboardVBV);

				for (const ModelSubset& subset : m_treeBillboardSubsets)
				{
					const UINT materialIndex = (subset.MaterialIndex < m_modelMaterials.size()) ? subset.MaterialIndex : 0;
					const ModelMaterial& material = m_modelMaterials[materialIndex];
					BindMaterial(material);
					m_commandList->DrawInstanced(subset.VertexCount, 1, subset.StartVertex, 0);
				}
			}
			else if (sceneObject.Geometry == SceneObjectGeometry::Box && m_boxVB && m_boxIB)
			{
				const UINT materialIndex = (sceneObject.MaterialIndex < m_modelMaterials.size()) ? sceneObject.MaterialIndex : 0;
				const ModelMaterial& material = m_modelMaterials.empty() ? ModelMaterial{} : m_modelMaterials[materialIndex];

				if (!boxPipelineBound)
				{
					m_commandList->SetPipelineState(m_renderingSystem.GeometryBasicPSO());
					m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					boxPipelineBound = true;
				}

				if (!boxGeometryBound)
				{
					m_commandList->IASetVertexBuffers(0, 1, &m_boxVBView);
					m_commandList->IASetIndexBuffer(&m_boxIBView);
					boxGeometryBound = true;
				}

				BindMaterial(material);
				m_commandList->DrawIndexedInstanced(m_boxIndexCount, 1, 0, 0, 0);
			}
		}
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

	if (!m_visibleTransparentObjectIndices.empty() && m_boxVB && m_boxIB)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE depthView = DepthStencilView();
		m_commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, &depthView);
		m_commandList->SetGraphicsRootSignature(m_renderingSystem.GeometryRootSignature());
		m_commandList->SetPipelineState(m_renderingSystem.ForwardTransparentPSO());
		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_commandList->IASetVertexBuffers(0, 1, &m_boxVBView);
		m_commandList->IASetIndexBuffer(&m_boxIBView);

		for (UINT objectIndex : m_visibleTransparentObjectIndices)
		{
			if (objectIndex >= m_sceneObjects.size()) {
				continue;
			}

			const SceneObject& sceneObject = m_sceneObjects[objectIndex];
			if (sceneObject.Geometry != SceneObjectGeometry::Box) {
				continue;
			}

			const UINT materialIndex = (sceneObject.MaterialIndex < m_modelMaterials.size()) ? sceneObject.MaterialIndex : 0;
			const ModelMaterial& material = m_modelMaterials.empty() ? ModelMaterial{} : m_modelMaterials[materialIndex];
			m_commandList->SetGraphicsRootDescriptorTable(0, ObjectPassGpuHandle(objectIndex));
			BindMaterial(material);
			m_commandList->DrawIndexedInstanced(m_boxIndexCount, 1, 0, 0, 0);
		}
	}

	{
		const D3D12_CPU_DESCRIPTOR_HANDLE depthView = DepthStencilView();
		m_commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, &depthView);
		DrawTransparentParticles();
	}

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
	const UINT objectCount = std::max<UINT>(1u, static_cast<UINT>(m_sceneObjects.size()));
	m_objectCB = std::make_unique<UploadBuffer<ObjectConstants>>(m_device.Get(), objectCount, true);
	m_passCB = std::make_unique<UploadBuffer<PassConstants>>(m_device.Get(), 1, true);
	m_deferredPassCB = std::make_unique<UploadBuffer<DeferredPassConstants>>(m_device.Get(), 1, true);
	m_shadowPassCB = std::make_unique<UploadBuffer<PassConstants>>(m_device.Get(), ShadowCascadeCount, true);
	m_directionalLightSB = std::make_unique<UploadBuffer<GpuDirectionalLight>>(m_device.Get(), MaxDirectionalLights, false);
	m_pointLightSB = std::make_unique<UploadBuffer<GpuPointLight>>(m_device.Get(), MaxPointLights, false);
	m_spotLightSB = std::make_unique<UploadBuffer<GpuSpotLight>>(m_device.Get(), MaxSpotLights, false);
	m_particleSimCB = std::make_unique<UploadBuffer<ParticleSimConstants>>(m_device.Get(), 1, true);
}

void Framework::BuildCbvHeap()
{
	m_objectPassCbvPairCount = std::max<UINT>(1u, static_cast<UINT>(m_sceneObjects.size()));
	m_textureSrvBaseIndex = m_objectPassCbvPairCount * 2;
	const UINT materialBlockCount = std::max<UINT>(1u, static_cast<UINT>(m_modelMaterials.size()));
	m_textureSrvCount = materialBlockCount * MaterialTextureSlotCount;
	m_deferredPassCbvIndex = m_textureSrvBaseIndex + m_textureSrvCount;
	m_gbufferSrvBaseIndex = m_deferredPassCbvIndex + 1;
	m_depthSrvIndex = m_gbufferSrvBaseIndex + Gbuffer::kTargetCount;
	m_directionalLightSrvIndex = m_depthSrvIndex + 1;
	m_pointLightSrvIndex = m_directionalLightSrvIndex + 1;
	m_spotLightSrvIndex = m_pointLightSrvIndex + 1;
	m_shadowSrvIndex = m_spotLightSrvIndex + 1;
	m_particleUavBaseIndex = m_shadowSrvIndex + 1;
	m_particleSortUavIndex = m_particleUavBaseIndex + ParticleBufferCount;
	m_particleSrvBaseIndex = m_particleSortUavIndex + 1;
	m_particleSortSrvIndex = m_particleSrvBaseIndex + ParticleBufferCount * 2;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = m_particleSortSrvIndex + 1;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_cbvHeap.GetAddressOf())));
}

void Framework::BuildCbvViews()
{
	const UINT objectCbByteSize = CalcConstantBufferByteSize(sizeof(ObjectConstants));
	const UINT passCbByteSize = CalcConstantBufferByteSize(sizeof(PassConstants));
	const D3D12_GPU_VIRTUAL_ADDRESS objectCbBaseAddress = m_objectCB->Resource()->GetGPUVirtualAddress();
	const D3D12_GPU_VIRTUAL_ADDRESS passCbAddress = m_passCB->Resource()->GetGPUVirtualAddress();

	for (UINT objectIndex = 0; objectIndex < m_objectPassCbvPairCount; ++objectIndex)
	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC objectCbvDesc = {};
		objectCbvDesc.BufferLocation = objectCbBaseAddress + static_cast<UINT64>(objectIndex) * objectCbByteSize;
		objectCbvDesc.SizeInBytes = objectCbByteSize;
		m_device->CreateConstantBufferView(&objectCbvDesc, CbvSrvCpuHandle(objectIndex * 2));

		D3D12_CONSTANT_BUFFER_VIEW_DESC passCbvDesc = {};
		passCbvDesc.BufferLocation = passCbAddress;
		passCbvDesc.SizeInBytes = passCbByteSize;
		m_device->CreateConstantBufferView(&passCbvDesc, CbvSrvCpuHandle(objectIndex * 2 + 1));
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

	{
		D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
		shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		shadowSrvDesc.Format = m_shadowMapSrvFormat;
		shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		shadowSrvDesc.Texture2DArray.MostDetailedMip = 0;
		shadowSrvDesc.Texture2DArray.MipLevels = 1;
		shadowSrvDesc.Texture2DArray.FirstArraySlice = 0;
		shadowSrvDesc.Texture2DArray.ArraySize = ShadowCascadeCount;
		shadowSrvDesc.Texture2DArray.PlaneSlice = 0;
		shadowSrvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
		m_device->CreateShaderResourceView(m_shadowMap.Get(), &shadowSrvDesc, CbvSrvCpuHandle(m_shadowSrvIndex));
	}

	BuildParticleDescriptors();
}

void Framework::BuildShadowResources()
{
	m_shadowMap.Reset();

	for (auto& matrix : m_shadowViewProj) {
		XMStoreFloat4x4(&matrix, XMMatrixIdentity());
	}
	m_shadowCascadeSplits.fill(0.0f);

	D3D12_RESOURCE_DESC shadowDesc = {};
	shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	shadowDesc.Alignment = 0;
	shadowDesc.Width = ShadowMapSize;
	shadowDesc.Height = ShadowMapSize;
	shadowDesc.DepthOrArraySize = ShadowCascadeCount;
	shadowDesc.MipLevels = 1;
	shadowDesc.Format = m_shadowMapResourceFormat;
	shadowDesc.SampleDesc.Count = 1;
	shadowDesc.SampleDesc.Quality = 0;
	shadowDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = m_shadowMapDsvFormat;
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	ThrowIfFailed(m_device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&shadowDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&m_shadowMap)));

	for (UINT cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Format = m_shadowMapDsvFormat;
		dsvDesc.Texture2DArray.MipSlice = 0;
		dsvDesc.Texture2DArray.FirstArraySlice = cascadeIndex;
		dsvDesc.Texture2DArray.ArraySize = 1;
		m_device->CreateDepthStencilView(
			m_shadowMap.Get(),
			&dsvDesc,
			ShadowCascadeDepthStencilView(cascadeIndex));
	}

	m_shadowViewport.TopLeftX = 0.0f;
	m_shadowViewport.TopLeftY = 0.0f;
	m_shadowViewport.Width = static_cast<float>(ShadowMapSize);
	m_shadowViewport.Height = static_cast<float>(ShadowMapSize);
	m_shadowViewport.MinDepth = 0.0f;
	m_shadowViewport.MaxDepth = 1.0f;

	m_shadowScissorRect = { 0, 0, static_cast<LONG>(ShadowMapSize), static_cast<LONG>(ShadowMapSize) };
}

void Framework::UpdateCascadedShadowMaps(const DirectX::XMMATRIX& view)
{
	for (auto& matrix : m_shadowViewProj) {
		XMStoreFloat4x4(&matrix, XMMatrixIdentity());
	}
	m_shadowCascadeSplits.fill(0.0f);

	if (!m_shadowPassCB || !m_shadowMap || m_directionalLightCount == 0) {
		return;
	}

	XMFLOAT3 lightDir = {
		m_directionalLights[0].DirectionIntensity.x,
		m_directionalLights[0].DirectionIntensity.y,
		m_directionalLights[0].DirectionIntensity.z
	};

	if ((lightDir.x * lightDir.x + lightDir.y * lightDir.y + lightDir.z * lightDir.z) <= 1e-6f) {
		lightDir = { 0.45f, -1.0f, 0.20f };
	}

	const XMVECTOR lightDirV = XMVector3Normalize(XMLoadFloat3(&lightDir));
	const XMVECTOR defaultUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	const float lightUpDot = std::fabs(XMVectorGetX(XMVector3Dot(lightDirV, defaultUp)));
	const XMVECTOR lightUp = (lightUpDot > 0.95f)
		? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
		: defaultUp;

	const float aspect = (m_clientHeight > 0)
		? static_cast<float>(m_clientWidth) / static_cast<float>(m_clientHeight)
		: 1.0f;

	XMFLOAT3 sceneCenter = { 0.0f, 0.0f, 0.0f };
	float sceneRadius = 8.0f;
	if (IsAabbValid(m_normalizedSceneBounds))
	{
		sceneCenter = AabbCenter(m_normalizedSceneBounds);
		const XMFLOAT3 extents = AabbExtents(m_normalizedSceneBounds);
		sceneRadius = std::sqrt(extents.x * extents.x + extents.y * extents.y + extents.z * extents.z);
		sceneRadius = (std::max)(sceneRadius, 1.0f);
	}

	const float camToSceneX = sceneCenter.x - m_camPos.x;
	const float camToSceneY = sceneCenter.y - m_camPos.y;
	const float camToSceneZ = sceneCenter.z - m_camPos.z;
	const float camToSceneDistance = std::sqrt(
		camToSceneX * camToSceneX +
		camToSceneY * camToSceneY +
		camToSceneZ * camToSceneZ);
	const float shadowFar = std::clamp(
		(std::max)(20.0f, camToSceneDistance + sceneRadius + 4.0f),
		8.0f,
		CameraFarZ);

	auto MakeFrustumCornersWorld = [&](float nearZ, float farZ)
	{
		std::array<XMVECTOR, 8> corners = {};
		const XMMATRIX cascadeProj = XMMatrixPerspectiveFovLH(CameraFovY, aspect, nearZ, farZ);
		const XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * cascadeProj);

		const XMVECTOR ndcCorners[8] = {
			XMVectorSet(-1.0f, -1.0f, 0.0f, 1.0f),
			XMVectorSet(-1.0f,  1.0f, 0.0f, 1.0f),
			XMVectorSet( 1.0f,  1.0f, 0.0f, 1.0f),
			XMVectorSet( 1.0f, -1.0f, 0.0f, 1.0f),
			XMVectorSet(-1.0f, -1.0f, 1.0f, 1.0f),
			XMVectorSet(-1.0f,  1.0f, 1.0f, 1.0f),
			XMVectorSet( 1.0f,  1.0f, 1.0f, 1.0f),
			XMVectorSet( 1.0f, -1.0f, 1.0f, 1.0f),
		};

		for (size_t i = 0; i < corners.size(); ++i) {
			corners[i] = XMVector3TransformCoord(ndcCorners[i], invViewProj);
		}

		return corners;
	};

	auto MakeAabbCorners = [](const Aabb& bounds)
	{
		return std::array<XMVECTOR, 8>{
			XMVectorSet(bounds.Min.x, bounds.Min.y, bounds.Min.z, 1.0f),
			XMVectorSet(bounds.Min.x, bounds.Min.y, bounds.Max.z, 1.0f),
			XMVectorSet(bounds.Min.x, bounds.Max.y, bounds.Min.z, 1.0f),
			XMVectorSet(bounds.Min.x, bounds.Max.y, bounds.Max.z, 1.0f),
			XMVectorSet(bounds.Max.x, bounds.Min.y, bounds.Min.z, 1.0f),
			XMVectorSet(bounds.Max.x, bounds.Min.y, bounds.Max.z, 1.0f),
			XMVectorSet(bounds.Max.x, bounds.Max.y, bounds.Min.z, 1.0f),
			XMVectorSet(bounds.Max.x, bounds.Max.y, bounds.Max.z, 1.0f),
		};
	};

	const std::array<XMVECTOR, 8> sceneCorners = IsAabbValid(m_normalizedSceneBounds)
		? MakeAabbCorners(m_normalizedSceneBounds)
		: std::array<XMVECTOR, 8>{};

	const SceneDefinition* scene = m_sceneDefinitions.empty()
		? nullptr
		: &m_sceneDefinitions[m_currentSceneIndex];

	float previousSplit = CameraNearZ;
	for (UINT cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
	{
		const float splitPortion = static_cast<float>(cascadeIndex + 1u) / static_cast<float>(ShadowCascadeCount);
		const float logarithmicSplit = CameraNearZ * std::pow(shadowFar / CameraNearZ, splitPortion);
		const float uniformSplit = CameraNearZ + (shadowFar - CameraNearZ) * splitPortion;
		const float cascadeFar = ShadowCascadeLambda * logarithmicSplit + (1.0f - ShadowCascadeLambda) * uniformSplit;

		const std::array<XMVECTOR, 8> cascadeCorners = MakeFrustumCornersWorld(previousSplit, cascadeFar);

		XMVECTOR cascadeCenter = XMVectorZero();
		for (const XMVECTOR& corner : cascadeCorners) {
			cascadeCenter += corner;
		}
		cascadeCenter /= static_cast<float>(cascadeCorners.size());

		float cascadeRadius = 0.0f;
		for (const XMVECTOR& corner : cascadeCorners)
		{
			const float distance = XMVectorGetX(XMVector3Length(corner - cascadeCenter));
			cascadeRadius = (std::max)(cascadeRadius, distance);
		}
		cascadeRadius = (std::max)(cascadeRadius, 0.25f);

		const float lightDistance = (std::max)(10.0f, sceneRadius + cascadeRadius + 4.0f);
		const XMVECTOR lightPos = cascadeCenter - lightDirV * lightDistance;
		const XMMATRIX lightView = XMMatrixLookAtLH(lightPos, cascadeCenter, lightUp);

		float minX = FLT_MAX;
		float minY = FLT_MAX;
		float minZ = FLT_MAX;
		float maxX = -FLT_MAX;
		float maxY = -FLT_MAX;
		float maxZ = -FLT_MAX;

		for (const XMVECTOR& corner : cascadeCorners)
		{
			XMFLOAT3 lightCorner = {};
			XMStoreFloat3(&lightCorner, XMVector3TransformCoord(corner, lightView));
			minX = (std::min)(minX, lightCorner.x);
			minY = (std::min)(minY, lightCorner.y);
			maxX = (std::max)(maxX, lightCorner.x);
			maxY = (std::max)(maxY, lightCorner.y);
		}

		const auto& zCorners = IsAabbValid(m_normalizedSceneBounds) ? sceneCorners : cascadeCorners;
		for (const XMVECTOR& corner : zCorners)
		{
			XMFLOAT3 lightCorner = {};
			XMStoreFloat3(&lightCorner, XMVector3TransformCoord(corner, lightView));
			minZ = (std::min)(minZ, lightCorner.z);
			maxZ = (std::max)(maxZ, lightCorner.z);
		}

		float centerX = 0.5f * (minX + maxX);
		float centerY = 0.5f * (minY + maxY);
		float halfExtent = 0.5f * (std::max)(maxX - minX, maxY - minY);
		halfExtent = (std::max)(halfExtent, 0.05f);

		const float unitsPerTexel = (halfExtent * 2.0f) / static_cast<float>(ShadowMapSize);
		centerX = std::floor(centerX / unitsPerTexel) * unitsPerTexel;
		centerY = std::floor(centerY / unitsPerTexel) * unitsPerTexel;

		minX = centerX - halfExtent;
		maxX = centerX + halfExtent;
		minY = centerY - halfExtent;
		maxY = centerY + halfExtent;

		const float zPadding = (std::max)(4.0f, sceneRadius * 2.0f);
		const float nearZ = (std::max)(0.0f, minZ - zPadding);
		const float farZ = (std::max)(nearZ + 1.0f, maxZ + zPadding);
		const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, nearZ, farZ);
		const XMMATRIX lightViewProj = lightView * lightProj;

		PassConstants shadowPass = {};
		XMStoreFloat4x4(&shadowPass.ViewProj, XMMatrixTranspose(lightViewProj));
		shadowPass.EyePosW = m_camPos;
		shadowPass.LightDirW = lightDir;
		shadowPass.Ambient = scene ? scene->ForwardAmbient : DirectX::XMFLOAT4{ 0.2f, 0.2f, 0.2f, 1.0f };
		shadowPass.Diffuse = scene ? scene->ForwardDiffuse : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
		shadowPass.Specular = scene ? scene->ForwardSpecular : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
		shadowPass.SpecPower = scene ? scene->ForwardSpecPower : 32.0f;
		shadowPass.UvScroll = scene && scene->EnableUvScroll ? m_uvAnimation : DirectX::XMFLOAT2{ 0.0f, 0.0f };
		shadowPass.UvTiling = scene ? scene->GlobalUvTiling : m_uvGlobalTiling;
		shadowPass.Time = static_cast<float>(m_timer.TotalTime());
		shadowPass.TessellationParams = {
			m_tessellationMinDistance,
			m_tessellationMaxDistance,
			m_tessellationMinFactor,
			m_tessellationMaxFactor
		};

		m_shadowPassCB->CopyData(cascadeIndex, shadowPass);
		m_shadowViewProj[cascadeIndex] = shadowPass.ViewProj;
		m_shadowCascadeSplits[cascadeIndex] = cascadeFar;

		previousSplit = cascadeFar;
	}
}

void Framework::RenderSceneToShadowMap()
{
	if (!m_shadowMap || !m_shadowPassCB || m_directionalLightCount == 0 || m_sceneObjects.empty()) {
		return;
	}

	D3D12_RESOURCE_BARRIER shadowToDepthWrite = {};
	shadowToDepthWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	shadowToDepthWrite.Transition.pResource = m_shadowMap.Get();
	shadowToDepthWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	shadowToDepthWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	shadowToDepthWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &shadowToDepthWrite);

	m_commandList->RSSetViewports(1, &m_shadowViewport);
	m_commandList->RSSetScissorRects(1, &m_shadowScissorRect);
	m_commandList->SetGraphicsRootSignature(m_renderingSystem.ShadowRootSignature());

	const UINT objectCbByteSize = CalcConstantBufferByteSize(sizeof(ObjectConstants));
	const UINT passCbByteSize = CalcConstantBufferByteSize(sizeof(PassConstants));
	const D3D12_GPU_VIRTUAL_ADDRESS objectCbBaseAddress = m_objectCB->Resource()->GetGPUVirtualAddress();
	const D3D12_GPU_VIRTUAL_ADDRESS shadowPassCbBaseAddress = m_shadowPassCB->Resource()->GetGPUVirtualAddress();

	auto MaterialCastsShadow = [](const ModelMaterial& material)
	{
		return material.Occluder && !material.Transparent && material.DiffuseAlbedo.w > 1e-3f;
	};

	auto BindShadowMaterial = [&](const ModelMaterial& srcMaterial)
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
		mat.WaterParams = srcMaterial.WaterParams;

		m_commandList->SetGraphicsRoot32BitConstants(
			2,
			static_cast<UINT>(sizeof(MaterialConstants) / sizeof(UINT32)),
			&mat,
			0);

		const D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = CbvSrvGpuHandle(m_textureSrvBaseIndex + srcMaterial.SrvBaseIndex);
		m_commandList->SetGraphicsRootDescriptorTable(3, textureHandle);
	};

	for (UINT cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE cascadeDsv = ShadowCascadeDepthStencilView(cascadeIndex);
		m_commandList->ClearDepthStencilView(
			cascadeDsv,
			D3D12_CLEAR_FLAG_DEPTH,
			1.0f,
			0,
			0,
			nullptr);
		m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &cascadeDsv);
		m_commandList->SetGraphicsRootConstantBufferView(
			1,
			shadowPassCbBaseAddress + static_cast<UINT64>(cascadeIndex) * passCbByteSize);

		bool boxGeometryBound = false;
		bool boxPipelineBound = false;

		for (UINT objectIndex = 0; objectIndex < static_cast<UINT>(m_sceneObjects.size()); ++objectIndex)
		{
			const SceneObject& sceneObject = m_sceneObjects[objectIndex];
			if (!sceneObject.Occluder) {
				continue;
			}

			m_commandList->SetGraphicsRootConstantBufferView(
				0,
				objectCbBaseAddress + static_cast<UINT64>(objectIndex) * objectCbByteSize);

			if (sceneObject.Geometry == SceneObjectGeometry::SceneModel && m_modelVB && !m_modelSubsets.empty())
			{
				m_commandList->IASetVertexBuffers(0, 1, &m_modelVBV);
				bool tessellationPipelineActive = false;
				bool pipelineInitialized = false;

				for (const ModelSubset& subset : m_modelSubsets)
				{
					const UINT materialIndex = (subset.MaterialIndex < m_modelMaterials.size()) ? subset.MaterialIndex : 0;
					const ModelMaterial& material = m_modelMaterials[materialIndex];
					if (!MaterialCastsShadow(material)) {
						continue;
					}

					const bool usesTessellation = (material.Flags & MaterialFlagUseTessellation) != 0u;
					if (!pipelineInitialized || tessellationPipelineActive != usesTessellation)
					{
						tessellationPipelineActive = usesTessellation;
						pipelineInitialized = true;

						if (tessellationPipelineActive)
						{
							m_commandList->SetPipelineState(m_renderingSystem.ShadowTessellationPSO());
							m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
						}
						else
						{
							m_commandList->SetPipelineState(m_renderingSystem.ShadowBasicPSO());
							m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
						}
					}

					BindShadowMaterial(material);
					m_commandList->DrawInstanced(subset.VertexCount, 1, subset.StartVertex, 0);
				}
			}
			else if (sceneObject.Geometry == SceneObjectGeometry::TreeModel && m_treeVB && !m_treeModelSubsets.empty())
			{
				m_commandList->IASetVertexBuffers(0, 1, &m_treeVBV);
				bool tessellationPipelineActive = false;
				bool pipelineInitialized = false;

				for (const ModelSubset& subset : m_treeModelSubsets)
				{
					const UINT materialIndex = (subset.MaterialIndex < m_modelMaterials.size()) ? subset.MaterialIndex : 0;
					const ModelMaterial& material = m_modelMaterials[materialIndex];
					if (!MaterialCastsShadow(material)) {
						continue;
					}

					const bool usesTessellation = (material.Flags & MaterialFlagUseTessellation) != 0u;
					if (!pipelineInitialized || tessellationPipelineActive != usesTessellation)
					{
						tessellationPipelineActive = usesTessellation;
						pipelineInitialized = true;

						if (tessellationPipelineActive)
						{
							m_commandList->SetPipelineState(m_renderingSystem.ShadowTessellationPSO());
							m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
						}
						else
						{
							m_commandList->SetPipelineState(m_renderingSystem.ShadowBasicPSO());
							m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
						}
					}

					BindShadowMaterial(material);
					m_commandList->DrawInstanced(subset.VertexCount, 1, subset.StartVertex, 0);
				}
			}
			else if (sceneObject.Geometry == SceneObjectGeometry::TreeBillboard && m_treeBillboardVB && !m_treeBillboardSubsets.empty())
			{
				m_commandList->SetPipelineState(m_renderingSystem.ShadowBasicPSO());
				m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				m_commandList->IASetVertexBuffers(0, 1, &m_treeBillboardVBV);

				for (const ModelSubset& subset : m_treeBillboardSubsets)
				{
					const UINT materialIndex = (subset.MaterialIndex < m_modelMaterials.size()) ? subset.MaterialIndex : 0;
					const ModelMaterial& material = m_modelMaterials[materialIndex];
					if (!MaterialCastsShadow(material)) {
						continue;
					}

					BindShadowMaterial(material);
					m_commandList->DrawInstanced(subset.VertexCount, 1, subset.StartVertex, 0);
				}
			}
			else if (sceneObject.Geometry == SceneObjectGeometry::Box && m_boxVB && m_boxIB)
			{
				const UINT materialIndex = (sceneObject.MaterialIndex < m_modelMaterials.size()) ? sceneObject.MaterialIndex : 0;
				const ModelMaterial& material = m_modelMaterials.empty() ? ModelMaterial{} : m_modelMaterials[materialIndex];
				if (!MaterialCastsShadow(material)) {
					continue;
				}

				if (!boxPipelineBound)
				{
					m_commandList->SetPipelineState(m_renderingSystem.ShadowBasicPSO());
					m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					boxPipelineBound = true;
				}

				if (!boxGeometryBound)
				{
					m_commandList->IASetVertexBuffers(0, 1, &m_boxVBView);
					m_commandList->IASetIndexBuffer(&m_boxIBView);
					boxGeometryBound = true;
				}

				BindShadowMaterial(material);
				m_commandList->DrawIndexedInstanced(m_boxIndexCount, 1, 0, 0, 0);
			}
		}
	}

	D3D12_RESOURCE_BARRIER shadowToShaderResource = {};
	shadowToShaderResource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	shadowToShaderResource.Transition.pResource = m_shadowMap.Get();
	shadowToShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	shadowToShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	shadowToShaderResource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &shadowToShaderResource);
}

void Framework::TransitionParticleResource(
	ID3D12Resource* resource,
	D3D12_RESOURCE_STATES& currentState,
	D3D12_RESOURCE_STATES targetState)
{
	if (!resource || currentState == targetState) {
		return;
	}

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.StateBefore = currentState;
	barrier.Transition.StateAfter = targetState;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &barrier);
	currentState = targetState;
}

void Framework::BuildParticleSystem()
{
	if (!m_device || !m_directCmdListAlloc || !m_commandList) {
		return;
	}

	DirectX::XMFLOAT3 rainCenter = { 0.0f, 0.0f, 0.0f };
	float rainHalfX = 2.0f;
	float rainHalfZ = 2.0f;
	float rainTopY = 2.0f;
	float rainFloorY = -1.0f;

	if (IsAabbValid(m_normalizedSceneBounds))
	{
		rainCenter = AabbCenter(m_normalizedSceneBounds);
		const float width = (std::max)(m_normalizedSceneBounds.Max.x - m_normalizedSceneBounds.Min.x, 0.5f);
		const float height = (std::max)(m_normalizedSceneBounds.Max.y - m_normalizedSceneBounds.Min.y, 0.5f);
		const float depth = (std::max)(m_normalizedSceneBounds.Max.z - m_normalizedSceneBounds.Min.z, 0.5f);
		rainHalfX = (std::max)(width * 0.85f, 1.6f);
		rainHalfZ = (std::max)(depth * 0.85f, 1.6f);
		rainTopY = m_normalizedSceneBounds.Max.y + height * 0.45f;
		rainFloorY = m_normalizedSceneBounds.Min.y - height * 0.12f;
	}

	std::mt19937 rng(20260425u);
	std::uniform_real_distribution<float> unitDist(0.0f, 1.0f);
	std::vector<GpuParticle> initialParticles(MaxParticles);

	for (UINT i = 0; i < MaxParticles; ++i)
	{
		const float x = rainCenter.x + (unitDist(rng) * 2.0f - 1.0f) * rainHalfX;
		const float z = rainCenter.z + (unitDist(rng) * 2.0f - 1.0f) * rainHalfZ;
		const float y = rainFloorY + unitDist(rng) * (rainTopY - rainFloorY);
		const float fallSpeed = 2.8f + unitDist(rng) * 2.0f;
		const float lifetime = (rainTopY - rainFloorY) / fallSpeed;
		const float age = (rainTopY - y) / fallSpeed;
		const float windX = -0.20f + unitDist(rng) * 0.40f;
		const float windZ = -0.08f + unitDist(rng) * 0.16f;

		GpuParticle& particle = initialParticles[i];
		particle.PositionAge = { x, y, z, age };
		particle.VelocityLifetime = { windX, -fallSpeed, windZ, lifetime };
		particle.ColorAlpha = {
			0.56f + unitDist(rng) * 0.10f,
			0.70f + unitDist(rng) * 0.12f,
			1.0f,
			0.18f + unitDist(rng) * 0.16f
		};
		particle.SizeSeed = {
			0.0020f + unitDist(rng) * 0.0022f,
			0.045f + unitDist(rng) * 0.050f,
			unitDist(rng),
			unitDist(rng)
		};
	}

	auto MakeBufferDesc = [](UINT64 byteSize, D3D12_RESOURCE_FLAGS flags)
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Alignment = 0;
		desc.Width = byteSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = flags;
		return desc;
	};

	auto CreateUploadResource = [&](const void* data, UINT64 byteSize, ComPtr<ID3D12Resource>& upload)
	{
		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		heapProps.CreationNodeMask = 1;
		heapProps.VisibleNodeMask = 1;

		const D3D12_RESOURCE_DESC desc = MakeBufferDesc(byteSize, D3D12_RESOURCE_FLAG_NONE);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&upload)));

		void* mappedData = nullptr;
		ThrowIfFailed(upload->Map(0, nullptr, &mappedData));
		std::memcpy(mappedData, data, static_cast<size_t>(byteSize));
		upload->Unmap(0, nullptr);
	};

	ComPtr<ID3D12Resource> particleUpload;
	const UINT64 particleBufferBytes = static_cast<UINT64>(sizeof(GpuParticle)) * MaxParticles;
	CreateUploadResource(initialParticles.data(), particleBufferBytes, particleUpload);

	const UINT zeroCounter = 0;
	const UINT initialCounter = MaxParticles;
	CreateUploadResource(&zeroCounter, sizeof(zeroCounter), m_particleCounterResetUpload);
	CreateUploadResource(&initialCounter, sizeof(initialCounter), m_particleCounterInitialUpload);

	D3D12_DRAW_ARGUMENTS initialDrawArgs = {};
	initialDrawArgs.VertexCountPerInstance = 0;
	initialDrawArgs.InstanceCount = 1;
	initialDrawArgs.StartVertexLocation = 0;
	initialDrawArgs.StartInstanceLocation = 0;
	CreateUploadResource(&initialDrawArgs, sizeof(initialDrawArgs), m_particleDrawArgsUpload);

	{
		D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
		argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

		D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc = {};
		commandSignatureDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
		commandSignatureDesc.NumArgumentDescs = 1;
		commandSignatureDesc.pArgumentDescs = &argDesc;
		commandSignatureDesc.NodeMask = 0;

		ThrowIfFailed(m_device->CreateCommandSignature(
			&commandSignatureDesc,
			nullptr,
			IID_PPV_ARGS(&m_particleDrawCommandSignature)));
	}

	D3D12_HEAP_PROPERTIES defaultHeapProps = {};
	defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	defaultHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	defaultHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	defaultHeapProps.CreationNodeMask = 1;
	defaultHeapProps.VisibleNodeMask = 1;

	const D3D12_RESOURCE_DESC particleDesc = MakeBufferDesc(
		particleBufferBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	const D3D12_RESOURCE_DESC counterDesc = MakeBufferDesc(
		D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	const D3D12_RESOURCE_DESC sortDesc = MakeBufferDesc(
		static_cast<UINT64>(sizeof(UINT) * 2u) * MaxParticles,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	const D3D12_RESOURCE_DESC drawArgsDesc = MakeBufferDesc(
		sizeof(D3D12_DRAW_ARGUMENTS),
		D3D12_RESOURCE_FLAG_NONE);

	for (UINT i = 0; i < ParticleBufferCount; ++i)
	{
		ThrowIfFailed(m_device->CreateCommittedResource(
			&defaultHeapProps,
			D3D12_HEAP_FLAG_NONE,
			&particleDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&m_particleBuffers[i])));

		ThrowIfFailed(m_device->CreateCommittedResource(
			&defaultHeapProps,
			D3D12_HEAP_FLAG_NONE,
			&counterDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&m_particleCounters[i])));

		m_particleBufferStates[i] = D3D12_RESOURCE_STATE_COMMON;
		m_particleCounterStates[i] = D3D12_RESOURCE_STATE_COMMON;
	}

	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&sortDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&m_particleSortBuffer)));
	m_particleSortBufferState = D3D12_RESOURCE_STATE_COMMON;

	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&drawArgsDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&m_particleDrawArgs)));
	m_particleDrawArgsState = D3D12_RESOURCE_STATE_COMMON;

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	for (UINT i = 0; i < ParticleBufferCount; ++i) {
		TransitionParticleResource(m_particleBuffers[i].Get(), m_particleBufferStates[i], D3D12_RESOURCE_STATE_COPY_DEST);
		m_commandList->CopyBufferRegion(m_particleBuffers[i].Get(), 0, particleUpload.Get(), 0, particleBufferBytes);
	}

	TransitionParticleResource(m_particleCounters[0].Get(), m_particleCounterStates[0], D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionParticleResource(m_particleCounters[1].Get(), m_particleCounterStates[1], D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionParticleResource(m_particleDrawArgs.Get(), m_particleDrawArgsState, D3D12_RESOURCE_STATE_COPY_DEST);
	m_commandList->CopyBufferRegion(m_particleCounters[0].Get(), 0, m_particleCounterInitialUpload.Get(), 0, sizeof(UINT));
	m_commandList->CopyBufferRegion(m_particleCounters[1].Get(), 0, m_particleCounterResetUpload.Get(), 0, sizeof(UINT));
	m_commandList->CopyBufferRegion(m_particleDrawArgs.Get(), 0, m_particleDrawArgsUpload.Get(), 0, sizeof(D3D12_DRAW_ARGUMENTS));

	for (UINT i = 0; i < ParticleBufferCount; ++i)
	{
		TransitionParticleResource(m_particleBuffers[i].Get(), m_particleBufferStates[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		TransitionParticleResource(m_particleCounters[i].Get(), m_particleCounterStates[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	TransitionParticleResource(m_particleDrawArgs.Get(), m_particleDrawArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdLists);
	FlushCommandQueue();

	m_particleReadBufferIndex = 0;
}

void Framework::BuildParticleDescriptors()
{
	if (!m_cbvHeap) {
		return;
	}

	for (UINT i = 0; i < ParticleBufferCount; ++i)
	{
		ID3D12Resource* particleBuffer = m_particleBuffers[i].Get();
		ID3D12Resource* particleCounter = m_particleCounters[i].Get();

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.NumElements = MaxParticles;
		uavDesc.Buffer.StructureByteStride = static_cast<UINT>(sizeof(GpuParticle));
		uavDesc.Buffer.CounterOffsetInBytes = 0;
		uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
		m_device->CreateUnorderedAccessView(
			particleBuffer,
			particleCounter,
			&uavDesc,
			CbvSrvCpuHandle(m_particleUavBaseIndex + i));

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MaxParticles;
		srvDesc.Buffer.StructureByteStride = static_cast<UINT>(sizeof(GpuParticle));
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		m_device->CreateShaderResourceView(
			particleBuffer,
			&srvDesc,
			CbvSrvCpuHandle(m_particleSrvBaseIndex + i * 2u));
	}

	D3D12_UNORDERED_ACCESS_VIEW_DESC sortUavDesc = {};
	sortUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	sortUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	sortUavDesc.Buffer.FirstElement = 0;
	sortUavDesc.Buffer.NumElements = MaxParticles;
	sortUavDesc.Buffer.StructureByteStride = sizeof(UINT) * 2u;
	sortUavDesc.Buffer.CounterOffsetInBytes = 0;
	sortUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	m_device->CreateUnorderedAccessView(
		m_particleSortBuffer.Get(),
		nullptr,
		&sortUavDesc,
		CbvSrvCpuHandle(m_particleSortUavIndex));

	D3D12_SHADER_RESOURCE_VIEW_DESC sortSrvDesc = {};
	sortSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sortSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
	sortSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	sortSrvDesc.Buffer.FirstElement = 0;
	sortSrvDesc.Buffer.NumElements = MaxParticles;
	sortSrvDesc.Buffer.StructureByteStride = sizeof(UINT) * 2u;
	sortSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	for (UINT i = 0; i < ParticleBufferCount; ++i)
	{
		m_device->CreateShaderResourceView(
			m_particleSortBuffer.Get(),
			&sortSrvDesc,
			CbvSrvCpuHandle(m_particleSrvBaseIndex + i * 2u + 1u));
	}

	m_device->CreateShaderResourceView(
		m_particleSortBuffer.Get(),
		&sortSrvDesc,
		CbvSrvCpuHandle(m_particleSortSrvIndex));
}

void Framework::UpdateParticleSimConstants(double dt)
{
	if (!m_particleSimCB) {
		return;
	}

	ParticleSimConstants sim = {};
	sim.DeltaTime = (std::min)(static_cast<float>(dt), 0.05f);
	sim.TotalTime = static_cast<float>(m_timer.TotalTime());
	sim.MaxParticles = MaxParticles;
	sim.Acceleration = { 0.0f, -0.10f, 0.0f, 0.0f };

	if (IsAabbValid(m_normalizedSceneBounds))
	{
		const DirectX::XMFLOAT3 center = AabbCenter(m_normalizedSceneBounds);
		const float width = (std::max)(m_normalizedSceneBounds.Max.x - m_normalizedSceneBounds.Min.x, 0.5f);
		const float height = (std::max)(m_normalizedSceneBounds.Max.y - m_normalizedSceneBounds.Min.y, 0.25f);
		const float depth = (std::max)(m_normalizedSceneBounds.Max.z - m_normalizedSceneBounds.Min.z, 0.5f);
		sim.RainArea = {
			(std::max)(width * 0.85f, 1.6f),
			m_normalizedSceneBounds.Max.y + height * 0.45f,
			(std::max)(depth * 0.85f, 1.6f),
			m_normalizedSceneBounds.Min.y - height * 0.12f
		};
		sim.RainCenter = { center.x, center.y, center.z, 0.0f };
	}
	else
	{
		sim.RainArea = { 2.0f, 2.0f, 2.0f, -1.0f };
		sim.RainCenter = { 0.0f, 0.0f, 0.0f, 0.0f };
	}

	Aabb collisionBounds = {};
	if (TrySelectRainCollisionBounds(m_sceneObjects, collisionBounds))
	{
		sim.CollisionBoundsMin = {
			collisionBounds.Min.x,
			collisionBounds.Min.y,
			collisionBounds.Min.z,
			1.0f
		};
		sim.CollisionBoundsMax = {
			collisionBounds.Max.x,
			collisionBounds.Max.y,
			collisionBounds.Max.z,
			0.035f
		};
	}
	else
	{
		sim.CollisionBoundsMin = { 0.0f, 0.0f, 0.0f, 0.0f };
		sim.CollisionBoundsMax = { 0.0f, 0.0f, 0.0f, 0.035f };
	}

	m_particleSimCB->CopyData(0, sim);
}

void Framework::SimulateParticles()
{
	if (!m_particleSimCB || !m_particleCounterResetUpload) {
		return;
	}
	if (!m_particleBuffers[0] || !m_particleBuffers[1] || !m_particleCounters[0] || !m_particleCounters[1]) {
		return;
	}

	const UINT sourceIndex = m_particleReadBufferIndex;
	const UINT destIndex = (sourceIndex + 1u) % ParticleBufferCount;

	TransitionParticleResource(
		m_particleBuffers[sourceIndex].Get(),
		m_particleBufferStates[sourceIndex],
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	TransitionParticleResource(
		m_particleBuffers[destIndex].Get(),
		m_particleBufferStates[destIndex],
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	TransitionParticleResource(
		m_particleCounters[destIndex].Get(),
		m_particleCounterStates[destIndex],
		D3D12_RESOURCE_STATE_COPY_DEST);
	m_commandList->CopyBufferRegion(
		m_particleCounters[destIndex].Get(),
		0,
		m_particleCounterResetUpload.Get(),
		0,
		sizeof(UINT));
	TransitionParticleResource(
		m_particleCounters[destIndex].Get(),
		m_particleCounterStates[destIndex],
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	m_commandList->SetComputeRootSignature(m_renderingSystem.ParticleComputeRootSignature());
	m_commandList->SetPipelineState(m_renderingSystem.ParticleComputePSO());
	m_commandList->SetComputeRootConstantBufferView(0, m_particleSimCB->Resource()->GetGPUVirtualAddress());
	m_commandList->SetComputeRootDescriptorTable(1, CbvSrvGpuHandle(m_particleUavBaseIndex + sourceIndex));
	m_commandList->SetComputeRootDescriptorTable(2, CbvSrvGpuHandle(m_particleUavBaseIndex + destIndex));

	const UINT groupCount = (MaxParticles + ParticleThreadGroupSize - 1u) / ParticleThreadGroupSize;
	m_commandList->Dispatch(groupCount, 1, 1);

	D3D12_RESOURCE_BARRIER uavBarriers[2] = {};
	uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[0].UAV.pResource = m_particleBuffers[destIndex].Get();
	uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[1].UAV.pResource = m_particleCounters[destIndex].Get();
	m_commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

	if (m_particleDrawArgs)
	{
		TransitionParticleResource(
			m_particleCounters[destIndex].Get(),
			m_particleCounterStates[destIndex],
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		TransitionParticleResource(
			m_particleDrawArgs.Get(),
			m_particleDrawArgsState,
			D3D12_RESOURCE_STATE_COPY_DEST);
		m_commandList->CopyBufferRegion(
			m_particleDrawArgs.Get(),
			0,
			m_particleCounters[destIndex].Get(),
			0,
			sizeof(UINT));
		TransitionParticleResource(
			m_particleCounters[destIndex].Get(),
			m_particleCounterStates[destIndex],
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		TransitionParticleResource(
			m_particleDrawArgs.Get(),
			m_particleDrawArgsState,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	}

	TransitionParticleResource(
		m_particleBuffers[destIndex].Get(),
		m_particleBufferStates[destIndex],
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	m_particleReadBufferIndex = destIndex;
}

void Framework::SortParticlesOnGpu()
{
	if (!m_particleSortBuffer || !m_passCB || !m_particleBuffers[m_particleReadBufferIndex]) {
		return;
	}

	TransitionParticleResource(
		m_particleBuffers[m_particleReadBufferIndex].Get(),
		m_particleBufferStates[m_particleReadBufferIndex],
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	TransitionParticleResource(
		m_particleSortBuffer.Get(),
		m_particleSortBufferState,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	m_commandList->SetComputeRootSignature(m_renderingSystem.ParticleComputeRootSignature());
	m_commandList->SetComputeRootDescriptorTable(4, CbvSrvGpuHandle(m_particleSrvBaseIndex + m_particleReadBufferIndex * 2u));
	m_commandList->SetComputeRootDescriptorTable(5, CbvSrvGpuHandle(m_particleSortUavIndex));

	const UINT groupCount = (MaxParticles + ParticleThreadGroupSize - 1u) / ParticleThreadGroupSize;
	m_commandList->SetPipelineState(m_renderingSystem.ParticleSortInitPSO());
	m_commandList->SetComputeRootConstantBufferView(3, m_passCB->Resource()->GetGPUVirtualAddress());
	m_commandList->Dispatch(groupCount, 1, 1);

	D3D12_RESOURCE_BARRIER sortBarrier = {};
	sortBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	sortBarrier.UAV.pResource = m_particleSortBuffer.Get();
	m_commandList->ResourceBarrier(1, &sortBarrier);

	m_commandList->SetPipelineState(m_renderingSystem.ParticleSortStepPSO());
	for (UINT level = 2u; level <= MaxParticles; level <<= 1u)
	{
		for (UINT levelMask = level >> 1u; levelMask > 0u; levelMask >>= 1u)
		{
			const UINT sortConstants[2] = { level, levelMask };
			m_commandList->SetComputeRoot32BitConstants(6, _countof(sortConstants), sortConstants, 0);
			m_commandList->Dispatch(groupCount, 1, 1);
			m_commandList->ResourceBarrier(1, &sortBarrier);
		}
	}

	TransitionParticleResource(
		m_particleSortBuffer.Get(),
		m_particleSortBufferState,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Framework::DrawTransparentParticles()
{
	if (!m_particleBuffers[m_particleReadBufferIndex] || !m_passCB) {
		return;
	}

	TransitionParticleResource(
		m_particleBuffers[m_particleReadBufferIndex].Get(),
		m_particleBufferStates[m_particleReadBufferIndex],
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	TransitionParticleResource(
		m_particleSortBuffer.Get(),
		m_particleSortBufferState,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	m_commandList->SetGraphicsRootSignature(m_renderingSystem.ParticleGraphicsRootSignature());
	m_commandList->SetPipelineState(m_renderingSystem.ParticleGraphicsPSO());
	m_commandList->SetGraphicsRootConstantBufferView(0, m_passCB->Resource()->GetGPUVirtualAddress());
	m_commandList->SetGraphicsRootDescriptorTable(1, CbvSrvGpuHandle(m_particleSrvBaseIndex + m_particleReadBufferIndex * 2u));
	m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
	m_commandList->IASetVertexBuffers(0, 0, nullptr);
	m_commandList->IASetIndexBuffer(nullptr);
	if (m_particleDrawCommandSignature && m_particleDrawArgs) {
		m_commandList->ExecuteIndirect(m_particleDrawCommandSignature.Get(), 1, m_particleDrawArgs.Get(), 0, nullptr, 0);
	}
	else {
		m_commandList->DrawInstanced(MaxParticles, 1, 0, 0);
	}
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
	sponza.TessellationMaxDistance = 4.0f;
	sponza.TessellationMinFactor = 2.0f;
	sponza.TessellationMaxFactor = 16.0f;
	sponza.DefaultDisplacementScale = 0.055f;
	sponza.DefaultDisplacementBias = 0.0f;
	sponza.AlphaCutoff = 0.33f;
	sponza.EnableWindAnimation = true;
	sponza.EnableUvScroll = true;
	sponza.LightingPreset = SceneLightingPreset::Default;
	sponza.EnableWaterPlane = true;
	sponza.WaterPlaneSize = { 1.58f, 1.34f };
	sponza.WaterPlaneHeight = -0.081f;
	sponza.WaterPlaneUvScale = 1.0f;
	sponza.WaterPlaneColor = { 0.18f, 0.56f, 0.84f, 1.0f };
	sponza.WaterWaveParams = { 0.010f, 10.75f, 5.20f, 2.48f };
	sponza.ScatterBoxCount = 2048;
	sponza.ScatterBoxScaleRange = { 0.016f, 0.040f };
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
	bistro.ScatterBoxCount = 1536;
	bistro.ScatterBoxScaleRange = { 0.015f, 0.038f };
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
	sanMiguel.ScatterBoxCount = 1280;
	sanMiguel.ScatterBoxScaleRange = { 0.018f, 0.042f };
	m_sceneDefinitions.push_back(sanMiguel);

	SceneDefinition cullingLab = {};
	cullingLab.Name = L"Culling Lab";
	cullingLab.Format = SceneAssetFormat::Procedural;
	cullingLab.CameraPos = { 0.0f, 5.0f, -16.0f };
	cullingLab.CameraTarget = { 0.0f, 2.5f, 0.0f };
	cullingLab.CameraMoveSpeed = 7.0f;
	cullingLab.EnableUvScroll = false;
	cullingLab.EnableWindAnimation = false;
	cullingLab.EnableScatterField = true;
	cullingLab.ScatterFieldHalfExtents = { 18.0f, 6.0f, 18.0f };
	cullingLab.ScatterOccluderCount = 28;
	cullingLab.ScatterBoxCount = 1800;
	cullingLab.ScatterBoxScaleRange = { 0.30f, 1.20f };
	cullingLab.ScatterTreeCount = 28;
	cullingLab.ScatterTreeScaleRange = { 0.040f, 0.058f };
	cullingLab.ScatterTreeBillboardDistance = 24.0f;
	cullingLab.ForwardAmbient = { 0.16f, 0.18f, 0.22f, 1.0f };
	cullingLab.ForwardDiffuse = { 0.98f, 0.96f, 0.92f, 1.0f };
	cullingLab.ForwardSpecular = { 0.95f, 0.96f, 1.00f, 1.0f };
	cullingLab.ForwardSpecPower = 42.0f;
	cullingLab.DeferredAmbientIntensity = 0.20f;
	cullingLab.DeferredAmbientColor = { 0.82f, 0.86f, 0.92f, 1.0f };
	m_sceneDefinitions.push_back(cullingLab);
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

	std::wstring title = m_title ? m_title : L"DX12 Scene Renderer";
	if (!m_sceneDefinitions.empty()) {
		title += L" | Scene: ";
		title += m_sceneDefinitions[m_currentSceneIndex].Name;
		title += L" | 1-";
		title += std::to_wstring(m_sceneDefinitions.size());
		title += L" switch | F1 debug | F2 frustum | F3 octree | F4 occlusion";
	}

	title += L" | Cull: ";
	title += CullingModeLabel(m_enableFrustumCulling, m_useOctreeForCulling);
	title += L" | Occlusion: ";
	title += m_enableOcclusionCulling ? L"On" : L"Off";
	title += L" | Submitted: ";
	title += std::to_wstring(m_visibleObjectCount);
	title += L"/";
	title += std::to_wstring(m_sceneObjects.size());
	title += L" | OccCulled: ";
	title += std::to_wstring(m_occlusionCulledObjectCount);
	title += L" | Props: ";
	title += std::to_wstring(m_boxObjectCount);
	title += L" | Particles: ";
	title += std::to_wstring(MaxParticles);

	SetWindowTextW(MainWnd(), title.c_str());
}

void Framework::BuildSceneObjects()
{
	using namespace DirectX;

	m_sceneObjects.clear();
	m_visibleObjectIndices.clear();
	m_visibleOpaqueObjectIndices.clear();
	m_visibleTransparentObjectIndices.clear();
	m_octreeNodes.clear();
	m_boxObjectCount = 0;
	m_visibleObjectCount = 0;
	m_occlusionCulledObjectCount = 0;

	if (m_sceneDefinitions.empty()) {
		return;
	}

	const SceneDefinition& scene = m_sceneDefinitions[m_currentSceneIndex];

	auto ComputeBoxBoundsFromWorld = [](FXMMATRIX world) -> Aabb
	{
		const XMFLOAT3 localCorners[8] = {
			{ -1.0f, -1.0f, -1.0f },
			{  1.0f, -1.0f, -1.0f },
			{ -1.0f,  1.0f, -1.0f },
			{  1.0f,  1.0f, -1.0f },
			{ -1.0f, -1.0f,  1.0f },
			{  1.0f, -1.0f,  1.0f },
			{ -1.0f,  1.0f,  1.0f },
			{  1.0f,  1.0f,  1.0f },
		};

		Aabb bounds = MakeInvalidAabb();
		for (const XMFLOAT3& localCorner : localCorners)
		{
			const XMVECTOR worldCorner = XMVector3TransformCoord(
				XMVectorSet(localCorner.x, localCorner.y, localCorner.z, 1.0f),
				world);
			XMFLOAT3 point;
			XMStoreFloat3(&point, worldCorner);
			ExpandAabb(bounds, point);
		}
		return bounds;
	};

	auto AddBoxObject = [&](const XMFLOAT3& scale, const XMFLOAT3& position, float yaw, UINT materialIndex, bool occluder)
	{
		const XMMATRIX world =
			XMMatrixScaling(scale.x, scale.y, scale.z) *
			XMMatrixRotationY(yaw) *
			XMMatrixTranslation(position.x, position.y, position.z);

		SceneObject boxObject = {};
		boxObject.Geometry = SceneObjectGeometry::Box;
		boxObject.MaterialIndex = materialIndex;
		boxObject.Constants = MakeObjectConstantsFromWorld(world);
		boxObject.Bounds = ComputeBoxBoundsFromWorld(world);
		boxObject.Occluder = occluder;
		m_sceneObjects.push_back(boxObject);
		++m_boxObjectCount;
	};

	auto AddTreeLodObjects = [&](const XMFLOAT3& position, float scale, float yaw)
	{
		if (!IsAabbValid(m_treeLocalBounds) ||
			m_treeModelSubsets.empty() ||
			m_treeBillboardSubsets.empty() ||
			m_treeLocalHeight <= 1e-4f ||
			m_treeLocalRadius <= 1e-4f)
		{
			return;
		}

		const float treeHeight = m_treeLocalHeight * scale;
		const float treeRadius = m_treeLocalRadius * scale;
		const float paddedRadius = treeRadius * 1.20f;
		const float paddedHeight = treeHeight * 1.06f;
		const Aabb treeBounds = MakeAabbFromCenterExtents(
			{ position.x, position.y + paddedHeight * 0.5f, position.z },
			{ paddedRadius, paddedHeight * 0.5f, paddedRadius });

		SceneObject treeModelObject = {};
		treeModelObject.Geometry = SceneObjectGeometry::TreeModel;
		treeModelObject.MaterialIndex = m_treeBarkMaterialIndex;
		treeModelObject.Constants = MakeObjectConstantsFromWorld(
			XMMatrixScaling(scale, scale, scale) *
			XMMatrixRotationY(yaw) *
			XMMatrixTranslation(position.x, position.y, position.z));
		treeModelObject.Bounds = treeBounds;
		treeModelObject.Occluder = false;
		treeModelObject.LodMaxDistance = scene.ScatterTreeBillboardDistance;
		m_sceneObjects.push_back(treeModelObject);

		SceneObject billboardObject = {};
		billboardObject.Geometry = SceneObjectGeometry::TreeBillboard;
		billboardObject.MaterialIndex = m_treeLeafMaterialIndex;
		billboardObject.Constants = MakeObjectConstantsFromWorld(
			XMMatrixScaling(treeRadius * 1.15f, treeHeight, treeRadius * 1.15f) *
			XMMatrixTranslation(position.x, position.y, position.z));
		billboardObject.Bounds = treeBounds;
		billboardObject.Occluder = false;
		billboardObject.Anchor = position;
		billboardObject.BillboardScale = { treeRadius * 1.15f, treeHeight };
		billboardObject.LodMinDistance = scene.ScatterTreeBillboardDistance;
		m_sceneObjects.push_back(billboardObject);
	};

	if (!scene.EnableScatterField && m_modelVertexCount > 0 && IsAabbValid(m_normalizedSceneBounds))
	{
		const XMFLOAT3 modelTranslation = {
			-m_modelCenter.x * m_modelScale,
			-m_modelCenter.y * m_modelScale,
			-m_modelCenter.z * m_modelScale
		};

		SceneObject modelObject = {};
		modelObject.Geometry = SceneObjectGeometry::SceneModel;
		modelObject.MaterialIndex = 0;
		modelObject.Constants = MakeObjectConstantsFromWorld(
			XMMatrixScaling(m_modelScale, m_modelScale, m_modelScale) *
			XMMatrixTranslation(modelTranslation.x, modelTranslation.y, modelTranslation.z));
		modelObject.Bounds = m_normalizedSceneBounds;
		m_sceneObjects.push_back(modelObject);
	}

	if (scene.EnableScatterField && IsAabbValid(m_normalizedSceneBounds) && scene.ScatterBoxCount > 0)
	{
		std::mt19937 rng(0xC0FFEEu + static_cast<unsigned int>(m_currentSceneIndex * 131u));

		const float width = (std::max)(m_normalizedSceneBounds.Max.x - m_normalizedSceneBounds.Min.x, 0.25f);
		const float height = (std::max)(m_normalizedSceneBounds.Max.y - m_normalizedSceneBounds.Min.y, 0.25f);
		const float depth = (std::max)(m_normalizedSceneBounds.Max.z - m_normalizedSceneBounds.Min.z, 0.25f);
		const float paddingX = width * 0.05f;
		const float paddingZ = depth * 0.05f;

		float scatterMinX = m_normalizedSceneBounds.Min.x + paddingX;
		float scatterMaxX = m_normalizedSceneBounds.Max.x - paddingX;
		float scatterMinZ = m_normalizedSceneBounds.Min.z + paddingZ;
		float scatterMaxZ = m_normalizedSceneBounds.Max.z - paddingZ;

		if (scatterMinX >= scatterMaxX)
		{
			scatterMinX = m_normalizedSceneBounds.Min.x;
			scatterMaxX = m_normalizedSceneBounds.Max.x;
		}

		if (scatterMinZ >= scatterMaxZ)
		{
			scatterMinZ = m_normalizedSceneBounds.Min.z;
			scatterMaxZ = m_normalizedSceneBounds.Max.z;
		}

		const UINT boxCount = scene.ScatterBoxCount;
		const float areaAspect = width / (std::max)(depth, 0.001f);
		const UINT gridX = (std::max)(1u, static_cast<UINT>(std::ceil(std::sqrt(static_cast<double>(boxCount) * areaAspect))));
		const UINT gridZ = (std::max)(1u, (boxCount + gridX - 1) / gridX);
		const float cellX = (scatterMaxX - scatterMinX) / static_cast<float>((std::max)(1u, gridX));
		const float cellZ = (scatterMaxZ - scatterMinZ) / static_cast<float>((std::max)(1u, gridZ));
		const float minScale = scene.ScatterBoxScaleRange.x;
		const float maxScale = (std::max)(scene.ScatterBoxScaleRange.x, scene.ScatterBoxScaleRange.y);
		const float groundY = m_normalizedSceneBounds.Min.y;
		const UINT opaqueMaterialCount = static_cast<UINT>(m_scatterMaterialIndices.size() >= 5 ? 5 : m_scatterMaterialIndices.size());

		m_sceneObjects.reserve(m_sceneObjects.size() + boxCount + scene.ScatterOccluderCount + scene.ScatterTreeCount * 2u + 1u);

		if (!m_scatterMaterialIndices.empty())
		{
			AddBoxObject(
				{ width * 0.5f, 0.25f, depth * 0.5f },
				{ 0.0f, groundY + 0.25f, 0.0f },
				0.0f,
				m_scatterMaterialIndices[0],
				false);
		}

		for (UINT occluderIndex = 0; occluderIndex < scene.ScatterOccluderCount; ++occluderIndex)
		{
			const float sx = RandomRange(rng, width * 0.018f, width * 0.055f);
			const float sy = RandomRange(rng, height * 0.14f, height * 0.32f);
			const float sz = RandomRange(rng, depth * 0.018f, depth * 0.045f);
			const float centerX = RandomRange(rng, scatterMinX + sx, scatterMaxX - sx);
			const float centerZ = RandomRange(rng, scatterMinZ + sz, scatterMaxZ - sz);
			const float yaw = RandomRange(rng, 0.0f, XM_2PI);
			UINT materialIndex = 0;
			if (!m_scatterMaterialIndices.empty())
			{
				if (opaqueMaterialCount > 1u) {
					materialIndex = m_scatterMaterialIndices[1u + (occluderIndex % (opaqueMaterialCount - 1u))];
				}
				else {
					materialIndex = m_scatterMaterialIndices[0];
				}
			}

			AddBoxObject(
				{ sx, sy, sz },
				{ centerX, groundY + sy, centerZ },
				yaw,
				materialIndex,
				true);
		}

		for (UINT boxIndex = 0; boxIndex < boxCount; ++boxIndex)
		{
			const UINT gx = boxIndex % gridX;
			const UINT gz = boxIndex / gridX;

			float centerX = scatterMinX + (static_cast<float>(gx) + 0.5f) * cellX;
			float centerZ = scatterMinZ + (static_cast<float>(gz) + 0.5f) * cellZ;
			centerX += RandomRange(rng, -0.35f, 0.35f) * cellX;
			centerZ += RandomRange(rng, -0.35f, 0.35f) * cellZ;

			const float scale = RandomRange(rng, minScale, maxScale);
			const UINT profile = boxIndex % 4u;
			XMFLOAT3 objectScale = { scale, scale, scale };
			if (profile == 1u) {
				objectScale = { scale * 0.55f, scale * 1.90f, scale * 0.55f };
			}
			else if (profile == 2u) {
				objectScale = { scale * 1.75f, scale * 0.40f, scale * 1.10f };
			}
			else if (profile == 3u) {
				objectScale = { scale * 1.90f, scale * 0.65f, scale * 0.45f };
			}

			centerX = std::clamp(centerX, scatterMinX + objectScale.x, scatterMaxX - objectScale.x);
			centerZ = std::clamp(centerZ, scatterMinZ + objectScale.z, scatterMaxZ - objectScale.z);

			const bool transparent = !m_scatterMaterialIndices.empty() && (boxIndex % 9u == 0u);
			UINT materialIndex = 0;
			if (!m_scatterMaterialIndices.empty())
			{
				if (transparent && m_scatterMaterialIndices.size() > opaqueMaterialCount) {
					const UINT glassBase = opaqueMaterialCount;
					const UINT glassCount = static_cast<UINT>(m_scatterMaterialIndices.size()) - glassBase;
					materialIndex = m_scatterMaterialIndices[glassBase + (boxIndex % glassCount)];
				}
				else
				{
					const UINT usableOpaqueCount = (std::max)(1u, opaqueMaterialCount);
					materialIndex = m_scatterMaterialIndices[boxIndex % usableOpaqueCount];
				}
			}

			AddBoxObject(
				objectScale,
				{ centerX, groundY + objectScale.y, centerZ },
				RandomRange(rng, 0.0f, XM_2PI),
				materialIndex,
				!transparent);
		}

		if (scene.ScatterTreeCount > 0 && !m_treeModelSubsets.empty() && !m_treeBillboardSubsets.empty())
		{
			const float treeMinScale = scene.ScatterTreeScaleRange.x;
			const float treeMaxScale = (std::max)(scene.ScatterTreeScaleRange.x, scene.ScatterTreeScaleRange.y);
			const float ringRadiusMin = (std::min)(width, depth) * 0.40f;
			const float ringRadiusMax = (std::min)(width, depth) * 0.495f;

			for (UINT treeIndex = 0; treeIndex < scene.ScatterTreeCount; ++treeIndex)
			{
				const float scale = RandomRange(rng, treeMinScale, treeMaxScale);
				const float treeRadius = m_treeLocalRadius * scale * 1.25f;

				float centerX = 0.0f;
				float centerZ = 0.0f;

				for (UINT attempt = 0; attempt < 24; ++attempt)
				{
					const float angle = RandomRange(rng, 0.0f, XM_2PI);
					const float radius = RandomRange(rng, ringRadiusMin, ringRadiusMax);
					centerX = std::cos(angle) * radius + RandomRange(rng, -0.08f, 0.08f) * width;
					centerZ = std::sin(angle) * radius + RandomRange(rng, -0.08f, 0.08f) * depth;
					centerX = std::clamp(centerX, scatterMinX + treeRadius, scatterMaxX - treeRadius);
					centerZ = std::clamp(centerZ, scatterMinZ + treeRadius, scatterMaxZ - treeRadius);

					const float radialDistanceSq = centerX * centerX + centerZ * centerZ;
					if (radialDistanceSq >= ringRadiusMin * ringRadiusMin * 0.70f) {
						break;
					}
				}

				AddTreeLodObjects(
					{ centerX, groundY, centerZ },
					scale,
					RandomRange(rng, 0.0f, XM_2PI));
			}
		}
	}

	m_visibleObjectIndices.reserve(m_sceneObjects.size());
	m_visibleOpaqueObjectIndices.reserve(m_sceneObjects.size());
	m_visibleTransparentObjectIndices.reserve(m_sceneObjects.size());
	m_visibleObjectCount = static_cast<UINT>(m_sceneObjects.size());
}

void Framework::BuildObjectConstantBuffer()
{
	const UINT objectCount = std::max<UINT>(1u, static_cast<UINT>(m_sceneObjects.size()));
	m_objectCB = std::make_unique<UploadBuffer<ObjectConstants>>(m_device.Get(), objectCount, true);

	if (m_sceneObjects.empty())
	{
		m_objectCB->CopyData(0, ObjectConstants{});
		return;
	}

	for (UINT objectIndex = 0; objectIndex < static_cast<UINT>(m_sceneObjects.size()); ++objectIndex) {
		m_objectCB->CopyData(objectIndex, m_sceneObjects[objectIndex].Constants);
	}
}

void Framework::BuildOctree()
{
	m_octreeNodes.clear();
	if (m_sceneObjects.empty()) {
		return;
	}

	Framework::Aabb rootBounds = MakeInvalidAabb();
	for (const SceneObject& object : m_sceneObjects) {
		ExpandAabb(rootBounds, object.Bounds);
	}

	if (!IsAabbValid(rootBounds)) {
		return;
	}

	constexpr UINT kLeafObjectThreshold = 32;
	constexpr UINT kMaxOctreeDepth = 6;

	std::vector<UINT> rootObjectIndices(m_sceneObjects.size());
	for (UINT objectIndex = 0; objectIndex < static_cast<UINT>(m_sceneObjects.size()); ++objectIndex) {
		rootObjectIndices[objectIndex] = objectIndex;
	}

	auto BuildNodeRecursive = [&](auto&& self, const Framework::Aabb& nodeBounds, const std::vector<UINT>& objectIndices, UINT depth) -> int
	{
		const int nodeIndex = static_cast<int>(m_octreeNodes.size());
		m_octreeNodes.push_back({});
		m_octreeNodes[nodeIndex].Bounds = nodeBounds;

		if (depth >= kMaxOctreeDepth || objectIndices.size() <= kLeafObjectThreshold)
		{
			m_octreeNodes[nodeIndex].ObjectIndices = objectIndices;
			return nodeIndex;
		}

		std::array<Framework::Aabb, 8> childBounds = {};
		for (int childIndex = 0; childIndex < 8; ++childIndex) {
			childBounds[childIndex] = BuildChildAabb(nodeBounds, childIndex);
		}

		std::array<std::vector<UINT>, 8> childObjectLists = {};
		std::vector<UINT> residentObjects;
		residentObjects.reserve(objectIndices.size());

		for (UINT objectIndex : objectIndices)
		{
			const Framework::Aabb& objectBounds = m_sceneObjects[objectIndex].Bounds;
			int fittingChildIndex = -1;

			for (int childIndex = 0; childIndex < 8; ++childIndex)
			{
				if (AabbFitsInside(objectBounds, childBounds[childIndex]))
				{
					fittingChildIndex = childIndex;
					break;
				}
			}

			if (fittingChildIndex >= 0) {
				childObjectLists[fittingChildIndex].push_back(objectIndex);
			}
			else {
				residentObjects.push_back(objectIndex);
			}
		}

		m_octreeNodes[nodeIndex].ObjectIndices = std::move(residentObjects);

		for (int childIndex = 0; childIndex < 8; ++childIndex)
		{
			if (!childObjectLists[childIndex].empty()) {
				m_octreeNodes[nodeIndex].Children[childIndex] =
					self(self, childBounds[childIndex], childObjectLists[childIndex], depth + 1);
			}
		}

		return nodeIndex;
	};

	BuildNodeRecursive(BuildNodeRecursive, rootBounds, rootObjectIndices, 0);
}

void Framework::UpdateVisibleObjects(const DirectX::XMMATRIX& viewProj)
{
	m_visibleObjectIndices.clear();
	m_visibleOpaqueObjectIndices.clear();
	m_visibleTransparentObjectIndices.clear();
	m_occlusionCulledObjectCount = 0;

	if (m_sceneObjects.empty())
	{
		m_visibleObjectCount = 0;
		return;
	}

	const SceneDefinition& scene = m_sceneDefinitions[m_currentSceneIndex];
	const XMFLOAT3 eyePos = m_camPos;
	std::vector<UINT> candidateObjectIndices;

	if (!m_enableFrustumCulling)
	{
		candidateObjectIndices.reserve(m_sceneObjects.size());
		for (UINT objectIndex = 0; objectIndex < static_cast<UINT>(m_sceneObjects.size()); ++objectIndex) {
			candidateObjectIndices.push_back(objectIndex);
		}
	}
	else
	{
		const std::array<DirectX::XMFLOAT4, 6> frustumPlanes = ExtractFrustumPlanes(viewProj);

		if (m_useOctreeForCulling && !m_octreeNodes.empty())
		{
			auto AppendNodeSubtree = [&](auto&& self, int nodeIndex) -> void
			{
				if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_octreeNodes.size())) {
					return;
				}

				const OctreeNode& node = m_octreeNodes[nodeIndex];
				candidateObjectIndices.insert(
					candidateObjectIndices.end(),
					node.ObjectIndices.begin(),
					node.ObjectIndices.end());

				for (int childIndex : node.Children) {
					self(self, childIndex);
				}
			};

			auto CollectVisibleNodeObjects = [&](auto&& self, int nodeIndex, bool acceptAll) -> void
			{
				if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_octreeNodes.size())) {
					return;
				}

				const OctreeNode& node = m_octreeNodes[nodeIndex];
				bool subtreeAccepted = acceptAll;

				if (!subtreeAccepted)
				{
					const AabbFrustumRelation nodeRelation = ClassifyAabbAgainstFrustum(node.Bounds, frustumPlanes);
					if (nodeRelation == AabbFrustumRelation::Outside) {
						return;
					}
					subtreeAccepted = (nodeRelation == AabbFrustumRelation::Inside);
				}

				if (subtreeAccepted)
				{
					AppendNodeSubtree(AppendNodeSubtree, nodeIndex);
					return;
				}

				for (UINT objectIndex : node.ObjectIndices)
				{
					if (ClassifyAabbAgainstFrustum(m_sceneObjects[objectIndex].Bounds, frustumPlanes) != AabbFrustumRelation::Outside) {
						candidateObjectIndices.push_back(objectIndex);
					}
				}

				for (int childIndex : node.Children) {
					self(self, childIndex, false);
				}
			};

			CollectVisibleNodeObjects(CollectVisibleNodeObjects, 0, false);
			std::sort(candidateObjectIndices.begin(), candidateObjectIndices.end());
			candidateObjectIndices.erase(
				std::unique(candidateObjectIndices.begin(), candidateObjectIndices.end()),
				candidateObjectIndices.end());
		}
		else
		{
			candidateObjectIndices.reserve(m_sceneObjects.size());
			for (UINT objectIndex = 0; objectIndex < static_cast<UINT>(m_sceneObjects.size()); ++objectIndex)
			{
				if (ClassifyAabbAgainstFrustum(m_sceneObjects[objectIndex].Bounds, frustumPlanes) != AabbFrustumRelation::Outside) {
					candidateObjectIndices.push_back(objectIndex);
				}
			}
		}
	}

	candidateObjectIndices.erase(
		std::remove_if(
			candidateObjectIndices.begin(),
			candidateObjectIndices.end(),
			[&](UINT objectIndex)
			{
				if (objectIndex >= m_sceneObjects.size()) {
					return true;
				}

				const SceneObject& object = m_sceneObjects[objectIndex];
				if (object.LodMinDistance <= 0.0f &&
					object.LodMaxDistance >= (std::numeric_limits<float>::max)() * 0.5f)
				{
					return false;
				}

				const XMFLOAT3 center = AabbCenter(object.Bounds);
				const float dx = center.x - eyePos.x;
				const float dy = center.y - eyePos.y;
				const float dz = center.z - eyePos.z;
				const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
				return distance < object.LodMinDistance || distance >= object.LodMaxDistance;
			}),
		candidateObjectIndices.end());

	auto IsTransparentObject = [&](UINT objectIndex) -> bool
	{
		const SceneObject& object = m_sceneObjects[objectIndex];
		if (object.Geometry == SceneObjectGeometry::SceneModel) {
			return false;
		}

		if (object.MaterialIndex >= m_modelMaterials.size()) {
			return false;
		}

		const ModelMaterial& material = m_modelMaterials[object.MaterialIndex];
		return material.Transparent || material.DiffuseAlbedo.w < 0.999f;
	};

	if (scene.EnableScatterField && m_enableOcclusionCulling)
	{
		std::sort(
			candidateObjectIndices.begin(),
			candidateObjectIndices.end(),
			[&](UINT lhs, UINT rhs)
			{
				const XMFLOAT3 lhsCenter = AabbCenter(m_sceneObjects[lhs].Bounds);
				const XMFLOAT3 rhsCenter = AabbCenter(m_sceneObjects[rhs].Bounds);
				const float lhsDx = lhsCenter.x - eyePos.x;
				const float lhsDy = lhsCenter.y - eyePos.y;
				const float lhsDz = lhsCenter.z - eyePos.z;
				const float rhsDx = rhsCenter.x - eyePos.x;
				const float rhsDy = rhsCenter.y - eyePos.y;
				const float rhsDz = rhsCenter.z - eyePos.z;
				return (lhsDx * lhsDx + lhsDy * lhsDy + lhsDz * lhsDz)
					< (rhsDx * rhsDx + rhsDy * rhsDy + rhsDz * rhsDz);
			});

		const int rasterWidth = 160;
		const int rasterHeight = 90;
		std::vector<float> occlusionDepth(static_cast<size_t>(rasterWidth) * rasterHeight, 1.0f);

		for (UINT objectIndex : candidateObjectIndices)
		{
			const bool transparent = IsTransparentObject(objectIndex);
			OcclusionScreenRect screenRect = {};
			const bool hasScreenRect = ProjectAabbToScreenRect(
				m_sceneObjects[objectIndex].Bounds,
				viewProj,
				eyePos,
				rasterWidth,
				rasterHeight,
				screenRect);

			if (hasScreenRect && IsOccludedByDepthPyramid(occlusionDepth, rasterWidth, screenRect))
			{
				++m_occlusionCulledObjectCount;
				continue;
			}

			m_visibleObjectIndices.push_back(objectIndex);
			if (transparent) {
				m_visibleTransparentObjectIndices.push_back(objectIndex);
			}
			else {
				m_visibleOpaqueObjectIndices.push_back(objectIndex);
			}

			const bool canOcclude =
				!transparent &&
				m_sceneObjects[objectIndex].Occluder &&
				(m_sceneObjects[objectIndex].MaterialIndex >= m_modelMaterials.size()
					? true
					: m_modelMaterials[m_sceneObjects[objectIndex].MaterialIndex].Occluder);

			if (canOcclude && hasScreenRect) {
				RasterizeOccluderToDepthPyramid(occlusionDepth, rasterWidth, screenRect);
			}
		}
	}
	else
	{
		for (UINT objectIndex : candidateObjectIndices)
		{
			m_visibleObjectIndices.push_back(objectIndex);
			if (IsTransparentObject(objectIndex)) {
				m_visibleTransparentObjectIndices.push_back(objectIndex);
			}
			else {
				m_visibleOpaqueObjectIndices.push_back(objectIndex);
			}
		}
	}

	std::sort(
		m_visibleTransparentObjectIndices.begin(),
		m_visibleTransparentObjectIndices.end(),
		[&](UINT lhs, UINT rhs)
		{
			const XMFLOAT3 lhsCenter = AabbCenter(m_sceneObjects[lhs].Bounds);
			const XMFLOAT3 rhsCenter = AabbCenter(m_sceneObjects[rhs].Bounds);
			const float lhsDx = lhsCenter.x - m_camPos.x;
			const float lhsDy = lhsCenter.y - m_camPos.y;
			const float lhsDz = lhsCenter.z - m_camPos.z;
			const float rhsDx = rhsCenter.x - m_camPos.x;
			const float rhsDy = rhsCenter.y - m_camPos.y;
			const float rhsDz = rhsCenter.z - m_camPos.z;
			return (lhsDx * lhsDx + lhsDy * lhsDy + lhsDz * lhsDz)
				> (rhsDx * rhsDx + rhsDy * rhsDy + rhsDz * rhsDz);
		});

	m_visibleObjectCount = static_cast<UINT>(m_visibleObjectIndices.size());
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
	if (scene.ModelPaths.empty() && !scene.EnableScatterField && scene.Format != SceneAssetFormat::Procedural) {
		throw std::runtime_error("Scene has no model paths.");
	}

	m_cameraMoveSpeed = scene.CameraMoveSpeed;
	m_tessellationMinDistance = scene.TessellationMinDistance;
	m_tessellationMaxDistance = scene.TessellationMaxDistance;
	m_tessellationMinFactor = scene.TessellationMinFactor;
	m_tessellationMaxFactor = scene.TessellationMaxFactor;

	FlushCommandQueue();

	m_modelMaterials.clear();
	m_scatterMaterialIndices.clear();
	m_modelSubsets.clear();
	m_textureResources.clear();
	m_textureUploadResources.clear();
	m_modelVB.Reset();
	m_modelVBUpload.Reset();
	m_modelVertexCount = 0;
	m_treeVB.Reset();
	m_treeVBUpload.Reset();
	m_treeVBV = {};
	m_treeVertexCount = 0;
	m_treeModelSubsets.clear();
	m_treeBillboardVB.Reset();
	m_treeBillboardVBUpload.Reset();
	m_treeBillboardVBV = {};
	m_treeBillboardVertexCount = 0;
	m_treeBillboardSubsets.clear();
	m_treeLocalBounds = {};
	m_treeLocalRadius = 0.0f;
	m_treeLocalHeight = 0.0f;
	m_treeBarkMaterialIndex = 0;
	m_treeLeafMaterialIndex = 0;

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
		mat.Transparent = false;
		mat.Occluder = true;
		mat.SrvBaseIndex = 0;
		return mat;
	};

	auto DisableDisplacement = [&](ModelMaterial& mat)
	{
		mat.Flags &= ~(MaterialFlagHasDisplacementTexture | MaterialFlagDisplacementFromNormal | MaterialFlagUseTessellation);
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
			mat.Flags |= MaterialFlagUseTessellation;
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
			mat.Transparent = true;
			mat.Occluder = false;
			DisableDisplacement(mat);
		}
	};

	ModelMaterial defaultMaterial = MakeDefaultMaterial();
	defaultMaterial.SrvBaseIndex = 0;
	m_modelMaterials.push_back(defaultMaterial);

	if (scene.EnableScatterField)
	{
		auto AppendScatterMaterial = [&](const DirectX::XMFLOAT4& albedo, bool transparent, bool occluder) -> UINT
		{
			ModelMaterial mat = MakeDefaultMaterial();
			mat.DiffuseAlbedo = albedo;
			mat.AlphaCutoff = 0.0f;
			mat.Transparent = transparent;
			mat.Occluder = occluder;
			mat.SrvBaseIndex = static_cast<UINT>(m_modelMaterials.size()) * MaterialTextureSlotCount;

			const UINT materialIndex = static_cast<UINT>(m_modelMaterials.size());
			m_modelMaterials.push_back(mat);
			m_scatterMaterialIndices.push_back(materialIndex);
			return materialIndex;
		};

		AppendScatterMaterial({ 0.16f, 0.18f, 0.22f, 1.0f }, false, true);  // floor
		AppendScatterMaterial({ 0.72f, 0.30f, 0.18f, 1.0f }, false, true);  // warm matte
		AppendScatterMaterial({ 0.18f, 0.55f, 0.72f, 1.0f }, false, true);  // cool matte
		AppendScatterMaterial({ 0.78f, 0.80f, 0.84f, 1.0f }, false, true);  // steel
		AppendScatterMaterial({ 0.78f, 0.63f, 0.24f, 1.0f }, false, true);  // brass-like
		AppendScatterMaterial({ 0.48f, 0.82f, 0.96f, 0.34f }, true, false); // cyan glass
		AppendScatterMaterial({ 0.98f, 0.76f, 0.42f, 0.28f }, true, false); // amber glass
	}

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

	auto AppendTriangle = [&](std::vector<Vertex>& bucket, Vertex v0, Vertex v1, Vertex v2)
	{
		ComputeTriangleNormalAndTangent(v0, v1, v2);
		bucket.push_back(v0);
		bucket.push_back(v1);
		bucket.push_back(v2);
		ExpandBounds(v0.Pos);
		ExpandBounds(v1.Pos);
		ExpandBounds(v2.Pos);
	};

	std::vector<std::vector<Vertex>> materialVertices(m_modelMaterials.size());

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

	if (scene.EnableWaterPlane)
	{
		ModelMaterial waterMaterial = MakeDefaultMaterial();
		waterMaterial.DiffuseAlbedo = scene.WaterPlaneColor;
		waterMaterial.UvTiling = { scene.WaterPlaneUvScale, scene.WaterPlaneUvScale };
		waterMaterial.AlphaCutoff = 0.0f;
		waterMaterial.Flags = MaterialFlagUseTessellation | MaterialFlagProceduralWater;
		waterMaterial.WaterParams = scene.WaterWaveParams;
		waterMaterial.Occluder = false;

		const UINT materialIndex = static_cast<UINT>(m_modelMaterials.size());
		waterMaterial.SrvBaseIndex = materialIndex * MaterialTextureSlotCount;
		m_modelMaterials.push_back(waterMaterial);
		materialVertices.resize(m_modelMaterials.size());

		const float centerX = 0.5f * (minP.x + maxP.x);
		const float centerZ = 0.5f * (minP.z + maxP.z);
		const float planeY = minP.y + (maxP.y - minP.y) * scene.WaterPlaneHeight;
		const float halfWidth = 0.5f * (maxP.x - minP.x) * scene.WaterPlaneSize.x;
		const float halfDepth = 0.5f * (maxP.z - minP.z) * scene.WaterPlaneSize.y;

		Vertex v0 = {};
		v0.Pos = { centerX - halfWidth, planeY, centerZ - halfDepth };
		v0.Normal = { 0.0f, 1.0f, 0.0f };
		v0.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		v0.TexC = { 0.0f, 0.0f };

		Vertex v1 = v0;
		v1.Pos = { centerX - halfWidth, planeY, centerZ + halfDepth };
		v1.TexC = { 0.0f, 1.0f };

		Vertex v2 = v0;
		v2.Pos = { centerX + halfWidth, planeY, centerZ + halfDepth };
		v2.TexC = { 1.0f, 1.0f };

		Vertex v3 = v0;
		v3.Pos = { centerX + halfWidth, planeY, centerZ - halfDepth };
		v3.TexC = { 1.0f, 0.0f };

		std::vector<Vertex>& bucket = materialVertices[materialIndex];
		AppendTriangle(bucket, v0, v1, v2);
		AppendTriangle(bucket, v0, v2, v3);
	}

	std::vector<Vertex> treeVertices;
	std::vector<Vertex> treeBillboardVertices;

	if (scene.EnableScatterField)
	{
		const std::filesystem::path treeModelPath = std::filesystem::path(L"assets") / L"chestnut" / L"AL05m.obj";
		if (std::filesystem::exists(treeModelPath))
		{
			const std::string objPathUtf8 = treeModelPath.u8string();
			const std::string baseDirUtf8 = treeModelPath.parent_path().u8string();

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
				OutputDebugStringA(("[tinyobj chestnut warn] " + warn + "\n").c_str());
			}
			if (!err.empty()) {
				OutputDebugStringA(("[tinyobj chestnut err ] " + err + "\n").c_str());
			}
			if (!ok) {
				throw std::runtime_error("Failed to load chestnut OBJ.");
			}

			std::vector<UINT> treeMaterialIndices;
			treeMaterialIndices.reserve(tinyMaterials.size());
			m_treeBarkMaterialIndex = 0;
			m_treeLeafMaterialIndex = 0;

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

				const std::filesystem::path baseColorPath = ResolveSceneTexturePathUtf8(treeModelPath, srcMaterial.diffuse_texname);
				if (!baseColorPath.empty()) {
					mat.TextureIndices[MaterialTextureBaseColorSlot] = AcquireTextureIndex(baseColorPath, defaultBaseColorIndex);
					mat.Flags |= MaterialFlagHasBaseColorTexture;
				}

				const std::filesystem::path explicitNormalPath = ResolveSceneTexturePathUtf8(treeModelPath, srcMaterial.normal_texname);
				const std::filesystem::path bumpPath = ResolveSceneTexturePathUtf8(treeModelPath, srcMaterial.bump_texname);
				std::filesystem::path normalPath = explicitNormalPath;
				std::filesystem::path displacementPath = ResolveSceneTexturePathUtf8(treeModelPath, srcMaterial.displacement_texname);
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

				const std::filesystem::path opacityPath = ResolveSceneTexturePathUtf8(treeModelPath, srcMaterial.alpha_texname);
				if (!opacityPath.empty()) {
					mat.TextureIndices[MaterialTextureOpacitySlot] = AcquireTextureIndex(opacityPath, defaultOpacityIndex);
					mat.Flags |= MaterialFlagHasOpacityTexture;
				}

				FinalizeMaterial(mat, materialNameLower);

				const UINT materialIndex = static_cast<UINT>(m_modelMaterials.size());
				mat.SrvBaseIndex = materialIndex * MaterialTextureSlotCount;
				m_modelMaterials.push_back(mat);
				treeMaterialIndices.push_back(materialIndex);

				if (m_treeBarkMaterialIndex == 0 && ContainsAny(materialNameLower, { "bark", "twig" })) {
					m_treeBarkMaterialIndex = materialIndex;
				}
				if (m_treeLeafMaterialIndex == 0 && ContainsAny(materialNameLower, { "leaf" })) {
					m_treeLeafMaterialIndex = materialIndex;
				}
			}

			if (m_treeBarkMaterialIndex == 0 && !treeMaterialIndices.empty()) {
				m_treeBarkMaterialIndex = treeMaterialIndices.front();
			}
			if (m_treeLeafMaterialIndex == 0) {
				m_treeLeafMaterialIndex = m_treeBarkMaterialIndex;
			}

			std::vector<std::vector<Vertex>> treeMaterialVertices(m_modelMaterials.size());
			XMFLOAT3 treeMinP = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
			XMFLOAT3 treeMaxP = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

			auto ExpandTreeBounds = [&](const XMFLOAT3& p)
			{
				treeMinP.x = (std::min)(treeMinP.x, p.x);
				treeMinP.y = (std::min)(treeMinP.y, p.y);
				treeMinP.z = (std::min)(treeMinP.z, p.z);
				treeMaxP.x = (std::max)(treeMaxP.x, p.x);
				treeMaxP.y = (std::max)(treeMaxP.y, p.y);
				treeMaxP.z = (std::max)(treeMaxP.z, p.z);
			};

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

					UINT materialIndex = m_treeBarkMaterialIndex;
					if (faceIndex < shape.mesh.material_ids.size())
					{
						const int tinyMatId = shape.mesh.material_ids[faceIndex];
						if (tinyMatId >= 0 && tinyMatId < static_cast<int>(treeMaterialIndices.size())) {
							materialIndex = treeMaterialIndices[static_cast<size_t>(tinyMatId)];
						}
					}
					if (materialIndex >= treeMaterialVertices.size()) {
						materialIndex = m_treeBarkMaterialIndex;
					}

					Vertex tri[3] = {};
					for (int vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
					{
						const tinyobj::index_t idx = shape.mesh.indices[indexOffset + static_cast<size_t>(vertexIndex)];
						tri[vertexIndex].Pos = ReadPos(idx.vertex_index);
						tri[vertexIndex].Normal = ReadNormal(idx.normal_index);
						tri[vertexIndex].Color = { 1.0f, 1.0f, 1.0f, 1.0f };
						tri[vertexIndex].TexC = ReadTexCoord(idx.texcoord_index);
						ExpandTreeBounds(tri[vertexIndex].Pos);
					}

					ComputeTriangleNormalAndTangent(tri[0], tri[1], tri[2]);
					treeMaterialVertices[materialIndex].push_back(tri[0]);
					treeMaterialVertices[materialIndex].push_back(tri[1]);
					treeMaterialVertices[materialIndex].push_back(tri[2]);
					indexOffset += 3;
				}
			}

			if (treeMinP.x <= treeMaxP.x && treeMinP.y <= treeMaxP.y && treeMinP.z <= treeMaxP.z)
			{
				const float centerX = 0.5f * (treeMinP.x + treeMaxP.x);
				const float centerZ = 0.5f * (treeMinP.z + treeMaxP.z);
				const float minY = treeMinP.y;

				for (std::vector<Vertex>& bucket : treeMaterialVertices)
				{
					for (Vertex& vertex : bucket)
					{
						vertex.Pos.x -= centerX;
						vertex.Pos.y -= minY;
						vertex.Pos.z -= centerZ;
					}
				}

				m_treeLocalBounds = {
					{ treeMinP.x - centerX, 0.0f, treeMinP.z - centerZ },
					{ treeMaxP.x - centerX, treeMaxP.y - minY, treeMaxP.z - centerZ }
				};
				m_treeLocalHeight = m_treeLocalBounds.Max.y - m_treeLocalBounds.Min.y;
				m_treeLocalRadius = (std::max)(
					0.5f * (m_treeLocalBounds.Max.x - m_treeLocalBounds.Min.x),
					0.5f * (m_treeLocalBounds.Max.z - m_treeLocalBounds.Min.z));
			}

			for (UINT materialIndex = 0; materialIndex < static_cast<UINT>(treeMaterialVertices.size()); ++materialIndex)
			{
				const std::vector<Vertex>& bucket = treeMaterialVertices[materialIndex];
				if (bucket.empty()) {
					continue;
				}

				ModelSubset subset = {};
				subset.MaterialIndex = materialIndex;
				subset.StartVertex = static_cast<UINT>(treeVertices.size());
				subset.VertexCount = static_cast<UINT>(bucket.size());
				m_treeModelSubsets.push_back(subset);
				treeVertices.insert(treeVertices.end(), bucket.begin(), bucket.end());
			}

			if (m_treeLocalHeight > 1e-4f && m_treeLocalRadius > 1e-4f)
			{
				auto RotatePointY = [](const XMFLOAT3& p, float yaw) -> XMFLOAT3
				{
					const float c = std::cos(yaw);
					const float s = std::sin(yaw);
					return {
						p.x * c - p.z * s,
						p.y,
						p.x * s + p.z * c
					};
				};

				auto AppendBillboardQuad = [&](std::vector<Vertex>& bucket, float halfWidth, float minY, float maxY, float zOffset, float yaw)
				{
					const XMFLOAT3 p0 = RotatePointY({ -halfWidth, minY, zOffset }, yaw);
					const XMFLOAT3 p1 = RotatePointY({ -halfWidth, maxY, zOffset }, yaw);
					const XMFLOAT3 p2 = RotatePointY({  halfWidth, maxY, zOffset }, yaw);
					const XMFLOAT3 p3 = RotatePointY({  halfWidth, minY, zOffset }, yaw);
					AppendDoubleSidedQuad(bucket, p0, p1, p2, p3);
				};

				std::vector<Vertex> barkBucket;
				std::vector<Vertex> leafBucket;
				barkBucket.reserve(12);
				leafBucket.reserve(72);

				AppendBillboardQuad(barkBucket, 0.10f, 0.00f, 0.40f, 0.0f, 0.0f);
				AppendBillboardQuad(leafBucket, 0.58f, 0.20f, 0.96f, 0.00f, 0.0f);
				AppendBillboardQuad(leafBucket, 0.52f, 0.26f, 0.92f, 0.08f, XM_PIDIV4 * 0.65f);
				AppendBillboardQuad(leafBucket, 0.52f, 0.26f, 0.92f, -0.08f, -XM_PIDIV4 * 0.65f);
				AppendBillboardQuad(leafBucket, 0.40f, 0.34f, 1.02f, 0.02f, XM_PIDIV4 * 1.25f);

				if (!barkBucket.empty())
				{
					ModelSubset subset = {};
					subset.MaterialIndex = m_treeBarkMaterialIndex;
					subset.StartVertex = static_cast<UINT>(treeBillboardVertices.size());
					subset.VertexCount = static_cast<UINT>(barkBucket.size());
					m_treeBillboardSubsets.push_back(subset);
					treeBillboardVertices.insert(treeBillboardVertices.end(), barkBucket.begin(), barkBucket.end());
				}

				if (!leafBucket.empty())
				{
					ModelSubset subset = {};
					subset.MaterialIndex = m_treeLeafMaterialIndex;
					subset.StartVertex = static_cast<UINT>(treeBillboardVertices.size());
					subset.VertexCount = static_cast<UINT>(leafBucket.size());
					m_treeBillboardSubsets.push_back(subset);
					treeBillboardVertices.insert(treeBillboardVertices.end(), leafBucket.begin(), leafBucket.end());
				}
			}
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

	if (vertices.empty())
	{
		if (!scene.EnableScatterField) {
			throw std::runtime_error("Scene loaded but produced 0 vertices.");
		}

		m_modelCenter = { 0.0f, 0.0f, 0.0f };
		m_modelScale = 1.0f;
		m_normalizedSceneBounds = {
			{ -scene.ScatterFieldHalfExtents.x, -scene.ScatterFieldHalfExtents.y, -scene.ScatterFieldHalfExtents.z },
			{  scene.ScatterFieldHalfExtents.x,  scene.ScatterFieldHalfExtents.y,  scene.ScatterFieldHalfExtents.z }
		};
	}
	else
	{
		m_modelCenter = {
			0.5f * (minP.x + maxP.x),
			0.5f * (minP.y + maxP.y),
			0.5f * (minP.z + maxP.z)
		};

		float maxDim = maxP.x - minP.x;
		maxDim = (std::max)(maxDim, maxP.y - minP.y);
		maxDim = (std::max)(maxDim, maxP.z - minP.z);
		m_modelScale = (maxDim > 1e-6f) ? (2.0f / maxDim) : 1.0f;
		m_normalizedSceneBounds = {
			{
				(minP.x - m_modelCenter.x) * m_modelScale,
				(minP.y - m_modelCenter.y) * m_modelScale,
				(minP.z - m_modelCenter.z) * m_modelScale
			},
			{
				(maxP.x - m_modelCenter.x) * m_modelScale,
				(maxP.y - m_modelCenter.y) * m_modelScale,
				(maxP.z - m_modelCenter.z) * m_modelScale
			}
		};
	}

	m_modelVertexCount = static_cast<UINT>(vertices.size());
	m_treeVertexCount = static_cast<UINT>(treeVertices.size());
	m_treeBillboardVertexCount = static_cast<UINT>(treeBillboardVertices.size());

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

	auto UploadVertexBuffer = [&](const std::vector<Vertex>& srcVertices,
		ComPtr<ID3D12Resource>& dstBuffer,
		ComPtr<ID3D12Resource>& uploadBuffer,
		D3D12_VERTEX_BUFFER_VIEW& outView)
	{
		dstBuffer.Reset();
		uploadBuffer.Reset();
		outView = {};

		if (srcVertices.empty()) {
			return;
		}

		const UINT byteSize = static_cast<UINT>(srcVertices.size() * sizeof(Vertex));
		const D3D12_RESOURCE_DESC vbDesc = MakeBufferDesc(byteSize);

		ThrowIfFailed(m_device->CreateCommittedResource(
			&defaultHeap,
			D3D12_HEAP_FLAG_NONE,
			&vbDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(dstBuffer.GetAddressOf())));

		ThrowIfFailed(m_device->CreateCommittedResource(
			&uploadHeap,
			D3D12_HEAP_FLAG_NONE,
			&vbDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(uploadBuffer.GetAddressOf())));

		void* mapped = nullptr;
		ThrowIfFailed(uploadBuffer->Map(0, nullptr, &mapped));
		memcpy(mapped, srcVertices.data(), byteSize);
		uploadBuffer->Unmap(0, nullptr);

		D3D12_RESOURCE_BARRIER toCopyDest = {};
		toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toCopyDest.Transition.pResource = dstBuffer.Get();
		toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		m_commandList->ResourceBarrier(1, &toCopyDest);

		m_commandList->CopyBufferRegion(dstBuffer.Get(), 0, uploadBuffer.Get(), 0, byteSize);

		D3D12_RESOURCE_BARRIER toVertexBuffer = {};
		toVertexBuffer.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toVertexBuffer.Transition.pResource = dstBuffer.Get();
		toVertexBuffer.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		toVertexBuffer.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		toVertexBuffer.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		m_commandList->ResourceBarrier(1, &toVertexBuffer);

		outView.BufferLocation = dstBuffer->GetGPUVirtualAddress();
		outView.StrideInBytes = sizeof(Vertex);
		outView.SizeInBytes = byteSize;
	};

	UploadVertexBuffer(vertices, m_modelVB, m_modelVBUpload, m_modelVBV);
	UploadVertexBuffer(treeVertices, m_treeVB, m_treeVBUpload, m_treeVBV);
	UploadVertexBuffer(treeBillboardVertices, m_treeBillboardVB, m_treeBillboardVBUpload, m_treeBillboardVBV);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdLists);
	FlushCommandQueue();

	m_textureUploadResources.clear();
	m_modelVBUpload.Reset();
	m_treeVBUpload.Reset();
	m_treeBillboardVBUpload.Reset();

	BuildSceneObjects();
	BuildObjectConstantBuffer();
	BuildOctree();
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

