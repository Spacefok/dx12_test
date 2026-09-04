#include "Terrain.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void RequireFailure(const std::function<void()>& operation, const char* message) {
    try { operation(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path Directory = std::filesystem::temp_directory_path()
        / ("terrain-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    terrain::Settings Settings;

    Fixture() {
        Settings.TileSamples = 9;
        Settings.PatchCells = 2;
        Settings.WorldSize = 16.0f;
        Settings.HeightScale = 16.0f;
        Settings.SkirtDepth = 1.0f;
        std::filesystem::create_directory(Directory);
        WriteTiles();
    }
    ~Fixture() {
        std::error_code ignored;
        const auto resolved = std::filesystem::weakly_canonical(Directory, ignored);
        const auto temporaryRoot = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), ignored);
        if (!ignored && resolved.parent_path() == temporaryRoot
            && resolved.filename().string().find("terrain-tests-") == 0) {
            std::filesystem::remove_all(resolved, ignored);
        }
    }
    static std::uint16_t Sample(unsigned x, unsigned z) {
        const float height = 0.4f + 0.15f * std::sin(x * 0.7f) * std::cos(z * 0.6f);
        return static_cast<std::uint16_t>(height * 65535.0f);
    }
    void WriteTiles() const {
        for (unsigned tileZ = 0; tileZ < 2; ++tileZ) {
            for (unsigned tileX = 0; tileX < 2; ++tileX) {
                std::ofstream file(Directory / ("height_" + std::to_string(tileX)
                    + "_" + std::to_string(tileZ) + ".r16"), std::ios::binary);
                for (unsigned z = 0; z < 9; ++z) {
                    for (unsigned x = 0; x < 9; ++x) {
                        const auto value = Sample(tileX * 8 + x, tileZ * 8 + z);
                        const char bytes[] = { static_cast<char>(value & 255), static_cast<char>(value >> 8) };
                        file.write(bytes, 2);
                    }
                }
            }
        }
    }
};

void CheckCoverage(const terrain::Terrain& terrain) {
    unsigned area = 0;
    const auto& selection = terrain.SelectedNodes();
    for (std::size_t i = 0; i < selection.size(); ++i) {
        const auto& a = terrain.Nodes()[selection[i]];
        area += a.SampleCells * a.SampleCells;
        for (std::size_t j = i + 1; j < selection.size(); ++j) {
            const auto& b = terrain.Nodes()[selection[j]];
            const bool overlapX = a.SampleX < b.SampleX + b.SampleCells && b.SampleX < a.SampleX + a.SampleCells;
            const bool overlapZ = a.SampleZ < b.SampleZ + b.SampleCells && b.SampleZ < a.SampleZ + a.SampleCells;
            Require(!(overlapX && overlapZ), "LOD selection contains overlapping ancestor/child patches");
        }
    }
    Require(area == 16 * 16, "Unculled LOD selection does not cover the complete heightmap");
}

void RunTests() {
    using namespace DirectX;
    Fixture fixture;
    terrain::Terrain terrain;
    terrain.Load(fixture.Directory, fixture.Settings);
    Require(terrain.Nodes().size() == 85, "Unexpected quadtree size for a 16-cell map with 2-cell patches");
    Require(terrain.Nodes().front().Error > 0.0f, "A curved heightfield must have nonzero coarse LOD error");

    for (const auto& node : terrain.Nodes()) {
        Require(std::size_t(node.StartVertex) + node.VertexCount <= terrain.Vertices().size(), "Invalid patch vertex range");
        for (unsigned i = 0; i < node.VertexCount; ++i) {
            const auto& vertex = terrain.Vertices()[node.StartVertex + i];
            Require(vertex.Pos.x >= node.Min.x - 1e-4f && vertex.Pos.x <= node.Max.x + 1e-4f
                && vertex.Pos.y >= node.Min.y - 1e-4f && vertex.Pos.y <= node.Max.y + 1e-4f
                && vertex.Pos.z >= node.Min.z - 1e-4f && vertex.Pos.z <= node.Max.z + 1e-4f,
                "Patch bounds fail to enclose the mesh or its skirt");
            const float length = XMVectorGetX(XMVector3Length(XMLoadFloat3(&vertex.Normal)));
            Require(std::isfinite(length) && std::abs(length - 1.0f) < 1e-4f && vertex.Normal.y > 0.0f,
                "Invalid terrain normal");
        }
        const unsigned surfaceVertexCount = fixture.Settings.PatchCells * fixture.Settings.PatchCells * 6;
        for (unsigned i = 0; i < surfaceVertexCount; i += 3) {
            const auto& a = terrain.Vertices()[node.StartVertex + i];
            const auto& b = terrain.Vertices()[node.StartVertex + i + 1];
            const auto& c = terrain.Vertices()[node.StartVertex + i + 2];
            const auto normal = XMVector3Cross(XMLoadFloat3(&b.Pos) - XMLoadFloat3(&a.Pos),
                XMLoadFloat3(&c.Pos) - XMLoadFloat3(&a.Pos));
            Require(XMVectorGetY(normal) > 0.0f, "Terrain surface triangle faces downward");
            for (const auto* vertex : { &a, &b, &c }) {
                const auto x = static_cast<unsigned>(std::lround(vertex->Pos.x + 8.0f));
                const auto z = static_cast<unsigned>(std::lround(vertex->Pos.z + 8.0f));
                const float expected = Fixture::Sample(x, z) / 65535.0f * fixture.Settings.HeightScale;
                Require(std::abs(vertex->Pos.y - expected) < 1e-4f,
                    "A tile edge or LOD vertex does not match its shared height sample");
            }
        }
    }
    std::cout << "PASS mesh winding, normals, shared height samples, and skirt bounds\n";

    terrain.Select({ 0, 12, -10 }, XMMatrixIdentity(), 800, 0.5f, false);
    CheckCoverage(terrain);
    const auto nearCount = terrain.Statistics().SelectedPatches;
    terrain.Select({ 0, 10000, -10 }, XMMatrixIdentity(), 800, 0.5f, false);
    CheckCoverage(terrain);
    Require(nearCount > terrain.Statistics().SelectedPatches, "Moving far from the terrain does not coarsen its LOD");
    terrain.Select({ 0, 12, -10 }, XMMatrixIdentity(), 800, 8.0f, false);
    CheckCoverage(terrain);
    Require(terrain.Statistics().SelectedPatches <= nearCount, "Looser error tolerance increases patch count");
    std::cout << "PASS non-overlapping coverage, distance LOD, and error tolerance\n";

    const auto view = XMMatrixLookAtLH(XMVectorSet(0, 10, -40, 1), XMVectorSet(0, 10, 0, 1), XMVectorSet(0, 1, 0, 0));
    // All terrain is before z=60, but lies inside the incorrect OpenGL-style z>=-w plane.
    const auto nearProjection = XMMatrixOrthographicLH(32, 100, 60, 100);
    terrain.Select({ 0, 10, -40 }, view * nearProjection, 800, 2, true);
    Require(terrain.SelectedNodes().empty(), "D3D near-plane rejection incorrectly accepts negative clip-space Z");
    terrain.Select({ 0, 10, -40 }, view * nearProjection, 800, 2, false);
    CheckCoverage(terrain);
    const auto visibleProjection = XMMatrixOrthographicLH(32, 100, 1, 100);
    terrain.Select({ 0, 10, -40 }, view * visibleProjection, 800, 2, true);
    Require(!terrain.SelectedNodes().empty(), "In-frustum terrain was rejected");
    const auto awayView = XMMatrixLookAtLH(XMVectorSet(0, 10, -40, 1), XMVectorSet(0, 10, -80, 1), XMVectorSet(0, 1, 0, 0));
    terrain.Select({ 0, 10, -40 }, awayView * XMMatrixPerspectiveFovLH(XM_PIDIV4, 1.7f, 0.1f, 100), 800, 2, true);
    Require(terrain.SelectedNodes().empty(), "Terrain behind the camera was not rejected");
    std::cout << "PASS D3D near plane, visible frustum, and behind-camera rejection\n";

    const auto vertexCount = terrain.Vertices().size();
    {
        std::fstream tile(fixture.Directory / "height_1_0.r16", std::ios::binary | std::ios::in | std::ios::out);
        const char mismatch[] = { 0, 0 };
        tile.write(mismatch, 2);
    }
    RequireFailure([&] { terrain.Load(fixture.Directory, fixture.Settings); }, "Mismatched tile borders were accepted");
    Require(terrain.Vertices().size() == vertexCount, "Failed tile load destroyed the current terrain");
    fixture.WriteTiles();
    std::ofstream(fixture.Directory / "height_1_0.r16", std::ios::binary | std::ios::trunc).put(0);
    RequireFailure([&] { terrain.Load(fixture.Directory, fixture.Settings); }, "Truncated tile was accepted");
    auto invalid = fixture.Settings;
    invalid.PatchCells = 3;
    RequireFailure([&] { terrain.Load(fixture.Directory, invalid); }, "Non-power-of-two patches were accepted");
    RequireFailure([&] { terrain.Select({ 0, 0, 0 }, XMMatrixIdentity(), 800, 0, true); }, "Zero LOD error tolerance was accepted");
    std::cout << "PASS invalid input, truncated tiles, seam rejection, and load exception safety\n";
    terrain.Clear();
    terrain.Select({ 0, 0, 0 }, XMMatrixIdentity(), 800, 1, true);
    Require(terrain.Empty() && terrain.Statistics().SelectedPatches == 0, "Clear did not reset terrain selection");
}

} // namespace

int main() {
    try { RunTests(); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
