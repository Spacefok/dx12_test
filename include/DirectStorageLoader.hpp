#ifndef DIRECT_STORAGE_LOADER_HPP
#define DIRECT_STORAGE_LOADER_HPP

#include "Dx12Common.hpp"
#include <cstdint>
#include <filesystem>
#include <vector>

namespace storage {

struct BufferRequest {
    std::uint64_t Offset = 0;
    std::uint32_t Size = 0;
    ID3D12Resource* Destination = nullptr;
};

// Read prepared geometry and one RGBA8 texture directly into default-heap resources.
// Destinations must be idle and in COMMON. They remain in COMMON on completion.
// The texture file uses GetCopyableFootprints layout, including row padding.
// Returns milliseconds for the submitted batch and completion wait, not CPU validation.
double Load(ID3D12Device* device, const std::filesystem::path& geometryPath,
    const std::filesystem::path& texturePath, const std::vector<BufferRequest>& buffers,
    ID3D12Resource* texture, std::uint32_t textureBytes);

} // namespace storage

#endif // DIRECT_STORAGE_LOADER_HPP
