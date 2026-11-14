#include "DirectStorageLoader.hpp"
#include <dstorage.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <sstream>

namespace storage {
namespace {

struct EventCloser {
    void operator()(void* event) const { if (event) CloseHandle(event); }
};

// Close also drains/cancels outstanding work on an exceptional exit. The caller
// may release the destination resources only after this guard has run.
struct QueueCloser {
    IDStorageQueue* Queue;
    ~QueueCloser() { if (Queue) Queue->Close(); }
};

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        std::ostringstream message;
        message << "DirectStorage " << operation << " failed (HRESULT 0x"
            << std::hex << static_cast<unsigned long>(result) << ").";
        throw std::runtime_error(message.str());
    }
}

UINT64 FileSize(IDStorageFile* file)
{
    BY_HANDLE_FILE_INFORMATION information = {};
    Check(file->GetFileInformation(&information), "file information");
    return (UINT64(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
}

} // namespace

double Load(ID3D12Device* device, const std::filesystem::path& geometryPath,
    const std::filesystem::path& texturePath, const std::vector<BufferRequest>& buffers,
    ID3D12Resource* texture, std::uint32_t textureBytes)
{
    if (!device || !texture || buffers.empty() || buffers.size() + 3 >= DSTORAGE_MIN_QUEUE_CAPACITY)
        throw std::invalid_argument("Invalid DirectStorage asset batch.");

    ComPtr<IDStorageFactory> factory;
    Check(DStorageGetFactory(IID_PPV_ARGS(&factory)), "factory creation");
    ComPtr<IDStorageFile> geometryFile;
    ComPtr<IDStorageFile> textureFile;
    Check(factory->OpenFile(geometryPath.c_str(), IID_PPV_ARGS(&geometryFile)), "geometry file open");
    Check(factory->OpenFile(texturePath.c_str(), IID_PPV_ARGS(&textureFile)), "texture file open");
    const UINT64 geometryBytes = FileSize(geometryFile.Get());

    UINT32 largestRequest = textureBytes;
    for (const auto& buffer : buffers) {
        if (!buffer.Destination || buffer.Size == 0 || buffer.Offset > geometryBytes
            || buffer.Size > geometryBytes - buffer.Offset)
            throw std::invalid_argument("DirectStorage geometry request exceeds its source file.");
        const auto destination = buffer.Destination->GetDesc();
        if (destination.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || buffer.Size > destination.Width)
            throw std::invalid_argument("DirectStorage geometry destination is too small.");
        largestRequest = (std::max)(largestRequest, buffer.Size);
    }

    const auto textureDesc = texture->GetDesc();
    if (textureDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || textureDesc.DepthOrArraySize != 1 || textureDesc.MipLevels != 1
        || textureDesc.SampleDesc.Count != 1 || textureDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM
        || textureDesc.Width > (std::numeric_limits<UINT>::max)())
        throw std::invalid_argument("DirectStorage demo expects a single RGBA8 texture subresource.");
    UINT64 requiredTextureBytes = 0;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &requiredTextureBytes);
    if (textureBytes != requiredTextureBytes || FileSize(textureFile.Get()) != requiredTextureBytes)
        throw std::invalid_argument("DirectStorage texture file does not match its GPU copy footprint.");

    // Every uncompressed request must fit the runtime's staging allocation.
    Check(factory->SetStagingBufferSize((std::max)(UINT32(DSTORAGE_STAGING_BUFFER_SIZE_32MB), largestRequest)),
        "staging buffer setup");
    DSTORAGE_QUEUE_DESC queueDesc = {};
    queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    queueDesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    queueDesc.Name = "Mesh geometry and texture";
    queueDesc.Device = device;
    ComPtr<IDStorageQueue> queue;
    Check(factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue creation");
    QueueCloser closeQueue = { queue.Get() };
    ComPtr<IDStorageStatusArray> status;
    Check(factory->CreateStatusArray(1, "Mesh load completion", IID_PPV_ARGS(&status)), "status creation");
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "completion fence creation");
    std::unique_ptr<void, EventCloser> complete(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!complete) Check(HRESULT_FROM_WIN32(GetLastError()), "completion event creation");
    Check(fence->SetEventOnCompletion(1, complete.get()), "completion event registration");

    const auto start = std::chrono::steady_clock::now();
    for (const auto& buffer : buffers) {
        DSTORAGE_REQUEST request = {};
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
        request.Source.File.Source = geometryFile.Get();
        request.Source.File.Offset = buffer.Offset;
        request.Source.File.Size = buffer.Size;
        request.UncompressedSize = buffer.Size;
        request.Destination.Buffer.Resource = buffer.Destination;
        request.Destination.Buffer.Size = buffer.Size;
        queue->EnqueueRequest(&request);
    }
    DSTORAGE_REQUEST textureRequest = {};
    textureRequest.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    textureRequest.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
    textureRequest.Source.File.Source = textureFile.Get();
    textureRequest.Source.File.Size = textureBytes;
    textureRequest.UncompressedSize = textureBytes;
    textureRequest.Destination.Texture.Resource = texture;
    textureRequest.Destination.Texture.SubresourceIndex = 0;
    textureRequest.Destination.Texture.Region = { 0, 0, 0,
        static_cast<UINT>(textureDesc.Width), textureDesc.Height, 1 };
    queue->EnqueueRequest(&textureRequest);
    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSignal(fence.Get(), 1);
    queue->Submit();

    const DWORD waitResult = WaitForSingleObject(complete.get(), 30000);
    if (waitResult != WAIT_OBJECT_0) {
        const DWORD waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        queue->Close();
        closeQueue.Queue = nullptr;
        if (waitResult == WAIT_FAILED) Check(HRESULT_FROM_WIN32(waitError), "completion wait");
        throw std::runtime_error("DirectStorage asset load did not complete within 30 seconds.");
    }
    const HRESULT batchResult = status->GetHResult(0);
    if (FAILED(batchResult)) {
        DSTORAGE_ERROR_RECORD error = {};
        if (WaitForSingleObject(queue->GetErrorEvent(), 0) == WAIT_OBJECT_0)
            queue->RetrieveErrorRecord(&error);
        Check(FAILED(error.FirstFailure.HResult) ? error.FirstFailure.HResult : batchResult, "asset batch");
    }
    if (fence->GetCompletedValue() == UINT64_MAX)
        Check(device->GetDeviceRemovedReason(), "device completion");
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace storage
