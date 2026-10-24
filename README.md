# mc-legacy-render-backport

[![CI](https://github.com/eeoliveeeelijah-bot/mc-legacy-render-backport/actions/workflows/ci.yml/badge.svg)](https://github.com/eeoliveeeelijah-bot/mc-legacy-render-backport/actions/workflows/ci.yml)

The JNI boundary between a JVM client (targeting Java 16, as used by Minecraft 1.17.1) and a native Vulkan render graph. Java fills a direct `ByteBuffer` with vertex and index data; the native side stages each batch into a per-frame ring buffer and records an indexed draw from it.

## How a draw batch flows

1. Java writes vertices (32 bytes each) and 32-bit indices into a direct `ByteBuffer`.
2. `RenderGraphPass.submitDrawBatch` validates counts and bounds, takes the submit lock, and calls into native code with the buffer's address.
3. `RenderGraph::StageGeometry` `memcpy`s both ranges into the current frame-in-flight slot of a persistently mapped ring buffer and returns aligned offsets.
4. The bridge binds the ring buffer as both vertex and index buffer at those offsets and records `vkCmdDrawIndexed`.
5. The call returns. The Java buffer is free to be overwritten immediately, because the GPU only ever reads the ring copy.

## Design decisions

**Draw directly from the ring buffer.** An earlier version copied each batch into a single device-local buffer with `vkCmdCopyBuffer` plus a pipeline barrier, then drew. That is invalid: copies must be recorded outside a render pass and draws inside one. It also made every batch overwrite offset 0 of the same buffer while earlier draws could still be reading it. Binding the host-visible ring slice directly needs no copy and no barrier, is legal inside a render pass, and gives every batch its own memory. Host writes made before `vkQueueSubmit` are visible to the device, with a `vkFlushMappedMemoryRanges` required only for non-coherent memory.

**Buffer lifetime.** Native code reads the Java buffer through a raw address, so `Reference.reachabilityFence` keeps the `ByteBuffer` strongly reachable until the native call returns. That stops a `Cleaner`-managed buffer from being freed mid-copy. Nothing more is needed, since the data is copied before the call returns.

**Threading.** Vulkan command buffers and the ring allocator are externally synchronised, so submissions are serialised with a `ReentrantLock`. `close()` takes the same lock, so it waits for an in-flight submission, and once it returns the owner can safely destroy the graph. Reentrant `submitDrawBatch` or `close()` from a native callback on the same thread throws instead of deadlocking or corrupting state.

**Errors never cross the boundary as undefined behaviour.** `StageGeometry` reports failure (for example, a full ring) as a `VkResult`, which becomes a Java exception. Every C++ exception is caught inside the JNI function and rethrown as a Java exception, so none can unwind into the JVM. The native entry point also re-validates its arguments and bounds rather than trusting the caller.

**Layout contract.** `RenderGraph.h` defines the `Vertex` struct with `static_assert`s on its size and field offsets. If the C++ layout drifts from the Java constants, the build fails.

## Repository layout

```
java/src/main/java/.../RenderGraphPass.java   Java API
java/src/test/java/.../BridgeTest.java        end-to-end JNI test suite
native/include/RenderGraph.h                  RenderGraph interface and layout contract
native/src/RenderGraphPassJni.cpp             JNI bridge
native/test/MockGraph.cpp                     mock RenderGraph and vkCmd* stubs for tests
CMakeLists.txt                                builds the Java jar, the runtime library and the tests
```

## Building and testing

Requirements: CMake 3.21+, a JDK (16 or newer), a C++17 compiler, and the Vulkan SDK (`libvulkan-dev` on Debian/Ubuntu).

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite builds the bridge against a mock `RenderGraph` and stub `vkCmd*` functions, then drives it from Java under `-Xcheck:jni`. It covers data copied exactly into aligned ring slots, per-batch slots, argument and bounds validation, ring-full and C++ exception reporting, misaligned slices, reentrancy, close semantics, and 16,000 submissions from 8 threads with no concurrent native entry.

To build only the tests without a Vulkan SDK (headers are then fetched automatically):

```sh
cmake -S . -B build -DRENDER_BRIDGE_BUILD_RUNTIME=OFF
```

## Status

This repository contains the boundary layer. A concrete `RenderGraph` (device setup, ring allocation, frame fences) is not included, and the tests use stand-ins for the three Vulkan draw calls. The code has not yet been exercised on a GPU, and the next step is running it inside a renderer with the Vulkan validation layers enabled.
