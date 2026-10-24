# Vulkan JVM Render Graph

This repository contains the core JNI boundary and memory management implementation for a custom Vulkan render graph, designed to run inside a managed JVM environment.

## Architecture Highlights
* **Zero-Copy JNI:** Passes mapped GPU memory pointers directly to the native layer.
* **Off-Heap Lifetime & Memory Ordering:** Combines `sun.misc.Unsafe` store fences with `Reference.reachabilityFence` so C2 scalar replacement and OSR deoptimizations cannot prematurely trigger `Cleaner` deallocation on off-heap buffers during native staging.
* **Vulkan Synchronization:** Handles cross-boundary memory barriers prior to command buffer submission.
