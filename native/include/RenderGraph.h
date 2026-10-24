#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan.h>

namespace renderbackport {

// ---------------------------------------------------------------------------
// Vertex / index layout shared with dev.renderbackport.vulkan.RenderGraphPass.
// If any of these change, the Java constants must change with them.
// ---------------------------------------------------------------------------

struct Vertex {
    float         position[3]; // 12 bytes  VK_FORMAT_R32G32B32_SFLOAT
    float         uv[2];       //  8 bytes  VK_FORMAT_R32G32_SFLOAT
    std::uint32_t color;       //  4 bytes  VK_FORMAT_R8G8B8A8_UNORM
    std::int16_t  normal[4];   //  8 bytes  VK_FORMAT_R16G16B16A16_SNORM (w unused)
};

inline constexpr VkDeviceSize kVertexStrideBytes = 32;                    // RenderGraphPass.VERTEX_STRIDE_BYTES
inline constexpr VkDeviceSize kIndexStrideBytes  = sizeof(std::uint32_t); // RenderGraphPass.INDEX_STRIDE_BYTES
inline constexpr VkIndexType  kIndexType         = VK_INDEX_TYPE_UINT32;

static_assert(std::is_standard_layout_v<Vertex>, "Vertex must be standard-layout");
static_assert(sizeof(Vertex) == kVertexStrideBytes, "Vertex size must equal Java VERTEX_STRIDE_BYTES (32)");
static_assert(offsetof(Vertex, position) == 0,  "position offset");
static_assert(offsetof(Vertex, uv)       == 12, "uv offset");
static_assert(offsetof(Vertex, color)    == 20, "color offset");
static_assert(offsetof(Vertex, normal)   == 24, "normal offset");
static_assert(kIndexStrideBytes == 4, "Index stride must equal Java INDEX_STRIDE_BYTES (4)");

// ---------------------------------------------------------------------------
// Result of staging one batch into the frame-in-flight ring buffer.
//
// Contract for implementations of RenderGraph::StageGeometry on success:
//   * buffer was created with VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
//     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, so the draw reads straight from it
//     (no copy, no barrier: legal inside a render pass instance).
//   * vertexOffset is a multiple of kVertexStrideBytes.
//   * indexOffset is a multiple of kIndexStrideBytes (required by
//     vkCmdBindIndexBuffer for VK_INDEX_TYPE_UINT32).
// ---------------------------------------------------------------------------
struct StagingSlice {
    VkBuffer     buffer       = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    VkDeviceSize indexOffset  = 0;
};

// ---------------------------------------------------------------------------
// Threading: not thread-safe. All calls for one RenderGraph must come from one
// thread at a time (RenderGraphPass serialises its submissions). Vulkan command
// buffers are externally synchronised, and so is the ring allocator.
// ---------------------------------------------------------------------------
class RenderGraph {
public:
    virtual ~RenderGraph() = default;

    RenderGraph(const RenderGraph&)            = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&)                 = delete;
    RenderGraph& operator=(RenderGraph&&)      = delete;

    // Precondition: the returned command buffer is in the recording state,
    // inside a render pass instance (or dynamic rendering), with a graphics
    // pipeline bound whose vertex input matches `Vertex` and binding 0.
    virtual VkCommandBuffer GetActiveCommandBuffer() const = 0;

    // Copies a client off-heap batch into the current frame-in-flight
    // persistently mapped ring buffer.
    //
    //   srcBasePtr + srcVertexOffset : vertexBytes of vertex data
    //   srcBasePtr + srcIndexOffset  : indexBytes  of 32-bit indices
    //
    // Source offsets carry no alignment guarantee: copy with std::memcpy,
    // never by dereferencing typed pointers into the source.
    //
    // Must return VK_SUCCESS and fill outSlice (per the StagingSlice contract),
    // or return an error code (e.g. VK_ERROR_OUT_OF_DEVICE_MEMORY when the ring
    // slot is full) and leave nothing staged.
    //
    // The implementation is responsible for:
    //   * vkFlushMappedMemoryRanges before queue submit if the ring memory is
    //     not VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    //   * never reusing a ring slot until the fence of the frame that last
    //     used it has signalled.
    virtual VkResult StageGeometry(
        const std::uint8_t* srcBasePtr,
        VkDeviceSize srcVertexOffset,
        VkDeviceSize vertexBytes,
        VkDeviceSize srcIndexOffset,
        VkDeviceSize indexBytes,
        StagingSlice& outSlice) = 0;

protected:
    RenderGraph() = default;
};

} // namespace renderbackport
