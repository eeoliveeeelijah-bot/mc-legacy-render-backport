// Test-only: mock RenderGraph, Vulkan command stubs, and JNI helpers.
#include <jni.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "RenderGraph.h"

using namespace renderbackport;

namespace {
JavaVM* gVm = nullptr;

struct DrawRecord {
    std::atomic<long> draws{0};
    VkBuffer vb = VK_NULL_HANDLE;
    VkDeviceSize vbOffset = 0;
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize ibOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM;
    uint32_t indexCount = 0;
} gRec;

std::atomic<int> gInside{0};
std::atomic<bool> gOverlap{false};

VkBuffer FakeBuffer() { return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(0xB0F0)); }
VkCommandBuffer FakeCmd() { return reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0xC0DE)); }

enum Mode { kNormal = 0, kThrow = 1, kMisaligned = 2, kCallbackClose = 3, kNullCmd = 4 };

class MockGraph final : public RenderGraph {
public:
    MockGraph(size_t ringBytes, int mode) : ring_(ringBytes), mode_(mode) {}
    VkCommandBuffer GetActiveCommandBuffer() const override {
        return mode_ == kNullCmd ? VK_NULL_HANDLE : FakeCmd();
    }
    VkResult StageGeometry(const uint8_t* src, VkDeviceSize sv, VkDeviceSize vb,
                           VkDeviceSize si, VkDeviceSize ib, StagingSlice& out) override {
        if (gInside.fetch_add(1) != 0) gOverlap = true;
        struct Exit { ~Exit() { gInside.fetch_sub(1); } } exitGuard;

        if (mode_ == kThrow) throw std::runtime_error("mock staging exploded");
        if (mode_ == kCallbackClose) {
            JNIEnv* env = nullptr;
            gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
            jclass c = env->FindClass("dev/renderbackport/vulkan/BridgeTest");
            jmethodID m = env->GetStaticMethodID(c, "closeFromCallback", "()V");
            env->CallStaticVoidMethod(c, m);
            env->DeleteLocalRef(c);
            if (env->ExceptionCheck()) return VK_ERROR_UNKNOWN;
        }

        auto alignUp = [](VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; };
        VkDeviceSize vOff = alignUp(head_, kVertexStrideBytes);
        VkDeviceSize iOff = alignUp(vOff + vb, kIndexStrideBytes);
        if (iOff + ib > ring_.size()) {               // wrap like a real ring would per frame
            vOff = 0;
            iOff = alignUp(vb, kIndexStrideBytes);
            if (iOff + ib > ring_.size()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        std::memcpy(ring_.data() + vOff, src + sv, vb);
        std::memcpy(ring_.data() + iOff, src + si, ib);
        head_ = iOff + ib;
        out.buffer = FakeBuffer();
        out.vertexOffset = (mode_ == kMisaligned) ? vOff + 4 : vOff;
        out.indexOffset = iOff;
        return VK_SUCCESS;
    }
    std::vector<uint8_t> ring_;
    VkDeviceSize head_ = 0;
    int mode_;
};
} // namespace

// ---- Vulkan command stubs (replace the loader for the test) ----
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(
    VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* bufs, const VkDeviceSize* offs) {
    if (cmd != FakeCmd() || first != 0 || count != 1) std::abort();
    gRec.vb = bufs[0];
    gRec.vbOffset = offs[0];
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(
    VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, VkIndexType type) {
    if (cmd != FakeCmd()) std::abort();
    gRec.ib = buf;
    gRec.ibOffset = off;
    gRec.indexType = type;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(
    VkCommandBuffer cmd, uint32_t indexCount, uint32_t inst, uint32_t firstIndex, int32_t vOff, uint32_t firstInst) {
    if (cmd != FakeCmd() || inst != 1 || firstIndex != 0 || vOff != 0 || firstInst != 0) std::abort();
    gRec.indexCount = indexCount;
    gRec.draws++;
}

// ---- JNI helpers for the Java test ----
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) { gVm = vm; return JNI_VERSION_1_8; }

extern "C" JNIEXPORT jlong JNICALL
Java_dev_renderbackport_vulkan_BridgeTest_nCreate(JNIEnv*, jclass, jint ringBytes, jint mode) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(
        static_cast<RenderGraph*>(new MockGraph(static_cast<size_t>(ringBytes), mode))));
}
extern "C" JNIEXPORT void JNICALL
Java_dev_renderbackport_vulkan_BridgeTest_nDestroy(JNIEnv*, jclass, jlong p) {
    delete reinterpret_cast<RenderGraph*>(static_cast<intptr_t>(p));
}
extern "C" JNIEXPORT jlongArray JNICALL
Java_dev_renderbackport_vulkan_BridgeTest_nLastDraw(JNIEnv* env, jclass) {
    jlong v[7] = {gRec.draws.load(), static_cast<jlong>(gRec.vbOffset), static_cast<jlong>(gRec.ibOffset),
                  static_cast<jlong>(gRec.indexCount), gRec.indexType == VK_INDEX_TYPE_UINT32 ? 1 : 0,
                  (gRec.vb == FakeBuffer() && gRec.ib == FakeBuffer()) ? 1 : 0, gOverlap.load() ? 1 : 0};
    jlongArray a = env->NewLongArray(7);
    env->SetLongArrayRegion(a, 0, 7, v);
    return a;
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_dev_renderbackport_vulkan_BridgeTest_nRing(JNIEnv* env, jclass, jlong p, jint off, jint len) {
    auto* g = static_cast<MockGraph*>(reinterpret_cast<RenderGraph*>(static_cast<intptr_t>(p)));
    jbyteArray a = env->NewByteArray(len);
    env->SetByteArrayRegion(a, 0, len, reinterpret_cast<const jbyte*>(g->ring_.data() + off));
    return a;
}
