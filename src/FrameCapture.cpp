#include "Framework.hpp"
#include <fstream>

void Framework::CaptureFrame(const std::filesystem::path& path)
{
    if (path.empty()) {
        throw std::invalid_argument("A frame capture requires an output path.");
    }
    m_pendingFrameCapture = path;
}

void Framework::SaveFrameCapture(const std::filesystem::path& path)
{
    ID3D12Resource* source = CurrentBackBuffer();
    const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
    if (sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        throw std::runtime_error("Frame capture requires an RGBA8 back buffer.");
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bufferBytes = 0;
    m_device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &footprint,
        nullptr, nullptr, &bufferBytes);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator)));
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = source;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION origin = {};
    origin.pResource = source;
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commands->ResourceBarrier(1, &barrier);
    ThrowIfFailed(commands->Close());
    ID3D12CommandList* lists[] = { commands.Get() };
    // The drawing list precedes this copy on the same queue. Capture finishes
    // before Present, which may discard the contents of a flip-model buffer.
    m_commandQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    const UINT width = static_cast<UINT>(sourceDesc.Width);
    const UINT height = sourceDesc.Height;
    const UINT imageBytes = width * height * 4;
    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
    BITMAPINFOHEADER imageHeader = {};
    imageHeader.biSize = sizeof(BITMAPINFOHEADER);
    imageHeader.biWidth = static_cast<LONG>(width);
    imageHeader.biHeight = static_cast<LONG>(height);
    imageHeader.biPlanes = 1;
    imageHeader.biBitCount = 32;
    imageHeader.biCompression = BI_RGB;
    imageHeader.biSizeImage = imageBytes;

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open frame capture output: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file.write(reinterpret_cast<const char*>(&imageHeader), sizeof(imageHeader));
    std::vector<unsigned char> row(static_cast<size_t>(width) * 4);
    void* mapped = nullptr;
    const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(bufferBytes) };
    ThrowIfFailed(readback->Map(0, &readRange, &mapped));
    const auto* pixels = static_cast<const unsigned char*>(mapped) + footprint.Offset;
    for (UINT y = 0; y < height; ++y)
    {
        const auto* sourceRow = pixels + static_cast<size_t>(height - 1 - y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < width; ++x)
        {
            row[x * 4] = sourceRow[x * 4 + 2];
            row[x * 4 + 1] = sourceRow[x * 4 + 1];
            row[x * 4 + 2] = sourceRow[x * 4];
            row[x * 4 + 3] = 255;
        }
        file.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
    const D3D12_RANGE writtenRange = { 0, 0 };
    readback->Unmap(0, &writtenRange);
    file.close();
    if (!file) {
        throw std::runtime_error("Failed to write frame capture output: " + path.string());
    }
}
