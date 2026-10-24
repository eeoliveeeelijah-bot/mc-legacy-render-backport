#include <jni.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>

#include "RenderGraph.h"

namespace {

using renderbackport::RenderGraph;
using renderbackport::StagingSlice;
using renderbackport::kIndexStrideBytes;
using renderbackport::kIndexType;
using renderbackport::kVertexStrideBytes;

constexpr const char* kIllegalArgument = "java/lang/IllegalArgumentException";
constexpr const char* kIllegalState    = "java/lang/IllegalStateException";
constexpr const char* kIndexOutOfBounds = "java/lang/IndexOutOfBoundsException";
constexpr const char* kOutOfMemory     = "java/lang/OutOfMemoryError";
constexpr const char* kRuntime         = "java/lang/RuntimeException";

// Raises a Java exception. Keeps the first one if an exception is already pending.
void ThrowJava(JNIEnv* env, const char* className, const char* message) noexcept {
    if (env->ExceptionCheck()) {
        return;
    }
    jclass exClass = env->FindClass(className);
    if (exClass == nullptr) {
        return; // FindClass left NoClassDefFoundError pending.
    }
    env->ThrowNew(exClass, message);
    env->DeleteLocalRef(exClass);
}

// jlong <-> pointer conversions that are well-defined on 32- and 64-bit targets.
template <typename T>
T* FromHandle(jlong handle) noexcept {
    return reinterpret_cast<T*>(static_cast<std::intptr_t>(handle));
}

jlong ToHandle(const void* ptr) noexcept {
    return static_cast<jlong>(reinterpret_cast<std::intptr_t>(ptr));
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_dev_renderbackport_vulkan_RenderGraphPass_nGetDirectBufferAddress(
    JNIEnv* env,
    jclass /* cls */,
    jobject buffer)
{
    if (buffer == nullptr) {
        return 0;
    }
    // Returns nullptr if the object is not a direct buffer or the JVM does not
    // support direct buffer access; Java treats 0 as failure.
    return ToHandle(env->GetDirectBufferAddress(buffer));
}

extern "C" JNIEXPORT void JNICALL
Java_dev_renderbackport_vulkan_RenderGraphPass_nSubmitDrawBatch(
    JNIEnv* env,
    jclass /* cls */,
    jlong graphPtr,
    jlong bufferAddr,
    jlong bufferCapacity,
    jint vertexOffsetBytes,
    jint vertexCount,
    jint indexOffsetBytes,
    jint indexCount)
{
    auto* renderGraph = FromHandle<RenderGraph>(graphPtr);
    const auto* srcBytes = FromHandle<const std::uint8_t>(bufferAddr);

    // Defence in depth: Java validates these too, but this entry point must
    // never turn a bad argument into undefined behaviour.
    if (renderGraph == nullptr || srcBytes == nullptr) {
        ThrowJava(env, kIllegalArgument, "Null native graph or direct buffer pointer across JNI boundary");
        return;
    }
    if (vertexCount <= 0 || indexCount <= 0) {
        ThrowJava(env, kIllegalArgument, "vertexCount and indexCount must be positive");
        return;
    }
    if (vertexOffsetBytes < 0 || indexOffsetBytes < 0 || bufferCapacity <= 0) {
        ThrowJava(env, kIllegalArgument, "Negative buffer offset or non-positive buffer capacity");
        return;
    }

    // All values are non-negative and < 2^31 here, so 64-bit math cannot overflow.
    const VkDeviceSize vertexByteSize = static_cast<VkDeviceSize>(vertexCount) * kVertexStrideBytes;
    const VkDeviceSize indexByteSize  = static_cast<VkDeviceSize>(indexCount) * kIndexStrideBytes;
    const VkDeviceSize srcVertexOffset = static_cast<VkDeviceSize>(vertexOffsetBytes);
    const VkDeviceSize srcIndexOffset  = static_cast<VkDeviceSize>(indexOffsetBytes);
    const VkDeviceSize capacity        = static_cast<VkDeviceSize>(bufferCapacity);

    if (srcVertexOffset + vertexByteSize > capacity || srcIndexOffset + indexByteSize > capacity) {
        ThrowJava(env, kIndexOutOfBounds, "Draw batch exceeds direct buffer capacity");
        return;
    }

    // No C++ exception may unwind through this JNI frame into the JVM.
    try {
        StagingSlice slice{};
        const VkResult result = renderGraph->StageGeometry(
            srcBytes, srcVertexOffset, vertexByteSize, srcIndexOffset, indexByteSize, slice);

        if (result != VK_SUCCESS) {
            char message[96];
            std::snprintf(message, sizeof(message),
                          "StageGeometry failed (VkResult %d)", static_cast<int>(result));
            ThrowJava(env, kIllegalState, message);
            return;
        }
        if (slice.buffer == VK_NULL_HANDLE) {
            ThrowJava(env, kIllegalState, "StageGeometry returned VK_SUCCESS with a null buffer");
            return;
        }
        if (slice.vertexOffset % kVertexStrideBytes != 0 || slice.indexOffset % kIndexStrideBytes != 0) {
            ThrowJava(env, kIllegalState, "StageGeometry returned a misaligned ring offset");
            return;
        }

        VkCommandBuffer cmd = renderGraph->GetActiveCommandBuffer();
        if (cmd == VK_NULL_HANDLE) {
            ThrowJava(env, kIllegalState, "No active command buffer");
            return;
        }

        // Draw straight from the persistently mapped ring slice. Host writes made
        // before vkQueueSubmit are visible to the device (after a flush for
        // non-coherent memory), so no copy and no pipeline barrier are needed,
        // which keeps this legal inside a render pass instance.
        const VkDeviceSize vertexBindOffset = slice.vertexOffset;
        vkCmdBindVertexBuffers(cmd, 0, 1, &slice.buffer, &vertexBindOffset);
        vkCmdBindIndexBuffer(cmd, slice.buffer, slice.indexOffset, kIndexType);
        vkCmdDrawIndexed(cmd, static_cast<std::uint32_t>(indexCount), 1, 0, 0, 0);
    } catch (const std::bad_alloc&) {
        ThrowJava(env, kOutOfMemory, "Native allocation failed while submitting draw batch");
    } catch (const std::exception& e) {
        ThrowJava(env, kRuntime, e.what());
    } catch (...) {
        ThrowJava(env, kRuntime, "Unknown native exception while submitting draw batch");
    }
}
