# Vulkan JVM Render Graph

This repository contains the core JNI boundary and memory management implementation for a custom Vulkan render graph, designed to run inside a managed JVM environment.

## Architecture Highlights
* **Zero-Copy JNI:** Passes mapped GPU memory pointers directly to the native layer.
* **Off-Heap Memory Pinning:** Utilizes `sun.misc.Unsafe` to pin memory regions, preventing ZGC/G1GC from compacting or sweeping buffers during active C2 JIT execution and on-stack replacement (OSR).
* **Vulkan Synchronization:** Handles cross-boundary memory barriers prior to command buffer submission.
