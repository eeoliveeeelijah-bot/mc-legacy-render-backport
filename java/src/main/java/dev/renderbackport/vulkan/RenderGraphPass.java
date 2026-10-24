package dev.renderbackport.vulkan;

import java.lang.ref.Reference;
import java.nio.ByteBuffer;
import java.util.Objects;
import java.util.concurrent.locks.ReentrantLock;

/**
 * Submits draw batches held in a client-side direct {@link ByteBuffer} to a native
 * Vulkan render graph. Each submission is copied by the native side into the current
 * frame-in-flight ring buffer and drawn directly from there, so the Java buffer may be
 * reused as soon as {@link #submitDrawBatch} returns.
 *
 * <p><b>Ownership:</b> this class owns neither the native graph nor the buffer.
 * {@link #close()} marks the pass closed and waits for an in-flight submission to
 * return. Only after {@code close()} returns may the owner destroy the graph or
 * release the buffer.
 *
 * <p><b>Threading:</b> submissions are serialised. Vulkan command buffers and the
 * native ring allocator are externally synchronised, so at most one thread is ever
 * inside the native call. Vertex/index data written by another thread must be
 * published to the submitting thread through a happens-before edge (a lock, a
 * concurrent queue handoff, a volatile write/read).
 *
 * <p><b>Reentrancy:</b> calling {@link #submitDrawBatch} or {@link #close()} from a
 * native callback that runs during a submission on the same thread throws
 * {@link IllegalStateException}.
 *
 * <p><b>Native contract:</b> see {@code RenderGraph.h}. The constants below must
 * match {@code renderbackport::kVertexStrideBytes} and {@code kIndexStrideBytes}.
 */
public final class RenderGraphPass implements AutoCloseable {

    /** pos(12) + uv(8) + color(4) + normal(8) = 32 bytes. Must match {@code renderbackport::Vertex}. */
    public static final int VERTEX_STRIDE_BYTES = 32;

    /** 32-bit indices (VK_INDEX_TYPE_UINT32). */
    public static final int INDEX_STRIDE_BYTES = Integer.BYTES;

    static {
        System.loadLibrary("vulkan_render_bridge");
    }

    private final long nativeGraphPtr;
    private final ByteBuffer clientBuffer;
    private final long clientBufferAddress;
    private final int bufferCapacity;

    private final ReentrantLock submitLock = new ReentrantLock();
    private volatile boolean closed; // written only while holding submitLock

    /**
     * @param nativeGraphPtr address of a live {@code renderbackport::RenderGraph}
     * @param clientBuffer   direct buffer holding vertex and index data for submissions
     */
    public RenderGraphPass(long nativeGraphPtr, ByteBuffer clientBuffer) {
        if (nativeGraphPtr == 0L) {
            throw new IllegalArgumentException("nativeGraphPtr must not be null (0)");
        }
        Objects.requireNonNull(clientBuffer, "clientBuffer must not be null");
        if (!clientBuffer.isDirect()) {
            throw new IllegalArgumentException("clientBuffer must be a direct off-heap ByteBuffer");
        }
        if (clientBuffer.capacity() == 0) {
            throw new IllegalArgumentException("clientBuffer must have non-zero capacity");
        }

        long address = nGetDirectBufferAddress(clientBuffer);
        if (address == 0L) {
            throw new IllegalStateException("Failed to resolve direct buffer native address");
        }

        this.nativeGraphPtr = nativeGraphPtr;
        this.clientBuffer = clientBuffer;
        this.clientBufferAddress = address;
        this.bufferCapacity = clientBuffer.capacity();
    }

    /**
     * Stages and records one indexed draw.
     *
     * @param vertexOffsetBytes byte offset of the first vertex in the client buffer (no alignment required)
     * @param vertexCount       number of vertices ({@value #VERTEX_STRIDE_BYTES} bytes each)
     * @param indexOffsetBytes  byte offset of the first index in the client buffer (no alignment required)
     * @param indexCount        number of 32-bit indices
     * @throws IllegalArgumentException  for non-positive counts or negative offsets
     * @throws IndexOutOfBoundsException if either range extends past the buffer capacity
     * @throws IllegalStateException     if the pass is closed, the call is reentrant, or native staging fails
     */
    public void submitDrawBatch(
            int vertexOffsetBytes,
            int vertexCount,
            int indexOffsetBytes,
            int indexCount) {

        if (vertexCount <= 0 || indexCount <= 0) {
            throw new IllegalArgumentException(
                    "vertexCount and indexCount must be positive (got "
                            + vertexCount + ", " + indexCount + ")");
        }
        if (vertexOffsetBytes < 0 || indexOffsetBytes < 0) {
            throw new IllegalArgumentException(
                    "Buffer offsets must be non-negative (got "
                            + vertexOffsetBytes + ", " + indexOffsetBytes + ")");
        }

        // long arithmetic: cannot overflow for int inputs.
        long vertexEnd = (long) vertexOffsetBytes + (long) vertexCount * VERTEX_STRIDE_BYTES;
        long indexEnd = (long) indexOffsetBytes + (long) indexCount * INDEX_STRIDE_BYTES;
        if (vertexEnd > bufferCapacity || indexEnd > bufferCapacity) {
            throw new IndexOutOfBoundsException(
                    "Draw batch exceeds direct buffer capacity " + bufferCapacity
                            + " (vertex end " + vertexEnd + ", index end " + indexEnd + ")");
        }

        if (submitLock.isHeldByCurrentThread()) {
            throw new IllegalStateException("Reentrant submitDrawBatch from a native callback");
        }
        submitLock.lock();
        try {
            if (closed) {
                throw new IllegalStateException("RenderGraphPass is closed");
            }
            nSubmitDrawBatch(
                    nativeGraphPtr,
                    clientBufferAddress,
                    bufferCapacity,
                    vertexOffsetBytes,
                    vertexCount,
                    indexOffsetBytes,
                    indexCount);
        } finally {
            // Native code reads through a raw address, so keep the buffer object strongly
            // reachable until the call has returned (its memory may be Cleaner-managed).
            Reference.reachabilityFence(clientBuffer);
            submitLock.unlock();
        }
    }

    public boolean isClosed() {
        return closed;
    }

    /**
     * Marks this pass closed, waiting for an in-flight submission to return first.
     * Idempotent. Does not free the native graph or the buffer (see class docs).
     *
     * @throws IllegalStateException if called from within a submission on the same thread
     */
    @Override
    public void close() {
        if (submitLock.isHeldByCurrentThread()) {
            throw new IllegalStateException("close() called from within submitDrawBatch");
        }
        submitLock.lock();
        try {
            closed = true;
        } finally {
            submitLock.unlock();
        }
    }

    private static native long nGetDirectBufferAddress(ByteBuffer buffer);

    private static native void nSubmitDrawBatch(
            long graphPtr,
            long bufferAddr,
            long bufferCapacity,
            int vertexOffsetBytes,
            int vertexCount,
            int indexOffsetBytes,
            int indexCount);
}
