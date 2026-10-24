package dev.renderbackport.vulkan;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

public final class BridgeTest {
    static { System.loadLibrary("vulkan_render_bridge"); } // same lib RenderGraphPass loads; second load is a no-op
    static native long nCreate(int ringBytes, int mode);
    static native void nDestroy(long p);
    static native long[] nLastDraw();
    static native byte[] nRing(long p, int off, int len);

    static RenderGraphPass callbackPass;
    static int passed;

    static void closeFromCallback() { callbackPass.close(); }

    static void check(boolean ok, String name) {
        if (!ok) throw new AssertionError("FAILED: " + name);
        passed++;
        System.out.println("ok  " + name);
    }

    static <T extends Throwable> void expect(Class<T> type, String msgPart, Runnable r, String name) {
        try {
            r.run();
        } catch (Throwable t) {
            check(type.isInstance(t) && (msgPart == null || String.valueOf(t.getMessage()).contains(msgPart)),
                  name + " -> " + t);
            return;
        }
        check(false, name + " (no exception)");
    }

    public static void main(String[] args) throws Exception {
        ByteBuffer buf = ByteBuffer.allocateDirect(4096);
        for (int i = 0; i < 4096; i++) buf.put(i, (byte) (i * 31 + 7));

        // 1. Unaligned source offsets are accepted; data lands aligned in the ring; draw recorded.
        long g = nCreate(1 << 16, 0);
        RenderGraphPass pass = new RenderGraphPass(g, buf);
        pass.submitDrawBatch(3, 4, 1001, 6);
        long[] d = nLastDraw();
        check(d[0] == 1 && d[3] == 6 && d[4] == 1 && d[5] == 1, "draw recorded: 6 UINT32 indices from ring buffer");
        check(d[1] % 32 == 0 && d[2] % 4 == 0, "ring offsets aligned (vb " + d[1] + ", ib " + d[2] + ")");
        byte[] v = nRing(g, (int) d[1], 128), ix = nRing(g, (int) d[2], 24);
        boolean same = true;
        for (int i = 0; i < 128; i++) same &= v[i] == buf.get(3 + i);
        for (int i = 0; i < 24; i++) same &= ix[i] == buf.get(1001 + i);
        check(same, "vertex and index bytes copied exactly");

        // 2. Second batch gets its own ring slot (no overwrite of batch 1).
        long firstVb = d[1];
        pass.submitDrawBatch(0, 2, 2000, 3);
        d = nLastDraw();
        check(d[0] == 2 && d[1] != firstVb, "second batch staged into a separate slot");

        // 3. Overlapping source ranges are allowed (pure copy source).
        pass.submitDrawBatch(0, 4, 0, 4);
        check(nLastDraw()[0] == 3, "overlapping source ranges accepted");

        // 4. Java-side validation.
        expect(IllegalArgumentException.class, "positive", () -> pass.submitDrawBatch(0, 0, 0, 3), "zero vertexCount rejected");
        expect(IllegalArgumentException.class, "non-negative", () -> pass.submitDrawBatch(-1, 1, 0, 3), "negative offset rejected");
        expect(IndexOutOfBoundsException.class, "capacity", () -> pass.submitDrawBatch(4000, 4, 0, 3), "vertex overrun rejected");
        expect(IndexOutOfBoundsException.class, "capacity", () -> pass.submitDrawBatch(0, 1, 0, Integer.MAX_VALUE), "int-overflow index count rejected");
        expect(IllegalArgumentException.class, "direct", () -> new RenderGraphPass(g, ByteBuffer.allocate(16)), "heap buffer rejected");
        expect(IllegalArgumentException.class, "null (0)", () -> new RenderGraphPass(0L, buf), "null graph rejected");

        // 5. Ring full -> VkResult surfaced as IllegalStateException.
        long small = nCreate(64, 0);
        RenderGraphPass sp = new RenderGraphPass(small, buf);
        expect(IllegalStateException.class, "VkResult -2)", () -> sp.submitDrawBatch(0, 4, 512, 4), "ring-full error reported");

        // 6. C++ exception converted, never unwinds into the JVM.
        long thr = nCreate(1 << 16, 1);
        RenderGraphPass tp = new RenderGraphPass(thr, buf);
        expect(RuntimeException.class, "mock staging exploded", () -> tp.submitDrawBatch(0, 1, 64, 3), "C++ exception -> RuntimeException");

        // 7. Native rejects misaligned slice from a buggy StageGeometry.
        long mis = nCreate(1 << 16, 2);
        RenderGraphPass mp = new RenderGraphPass(mis, buf);
        expect(IllegalStateException.class, "misaligned", () -> mp.submitDrawBatch(0, 1, 64, 3), "misaligned ring offset caught");

        // 8. Null command buffer caught.
        long nc = nCreate(1 << 16, 4);
        RenderGraphPass np = new RenderGraphPass(nc, buf);
        expect(IllegalStateException.class, "command buffer", () -> np.submitDrawBatch(0, 1, 64, 3), "null command buffer caught");

        // 9. close() from a native callback during submit is rejected (no deadlock, first exception kept).
        long cb = nCreate(1 << 16, 3);
        callbackPass = new RenderGraphPass(cb, buf);
        expect(IllegalStateException.class, "within submitDrawBatch", () -> callbackPass.submitDrawBatch(0, 1, 64, 3), "reentrant close rejected");
        check(!callbackPass.isClosed(), "pass still open after rejected reentrant close");

        // 10. close semantics.
        pass.close();
        pass.close();
        check(pass.isClosed(), "close idempotent");
        expect(IllegalStateException.class, "closed", () -> pass.submitDrawBatch(0, 1, 64, 3), "submit after close rejected");

        // 11. Concurrency: 8 threads x 2000 submits; native side must never be entered concurrently.
        long cg = nCreate(1 << 16, 0);
        RenderGraphPass cp = new RenderGraphPass(cg, buf);
        long before = nLastDraw()[0];
        AtomicReference<Throwable> err = new AtomicReference<>();
        List<Thread> threads = new ArrayList<>();
        for (int t = 0; t < 8; t++) {
            Thread th = new Thread(() -> {
                try { for (int i = 0; i < 2000; i++) cp.submitDrawBatch(i % 64, 3, 2048, 3); }
                catch (Throwable e) { err.set(e); }
            });
            threads.add(th);
            th.start();
        }
        for (Thread th : threads) th.join();
        d = nLastDraw();
        check(err.get() == null, "no errors under concurrency");
        check(d[0] - before == 16000, "all 16000 concurrent submits drawn");
        check(d[6] == 0, "native graph never entered concurrently");

        // 12. close() while another thread is submitting waits, then blocks further submits.
        Thread busy = new Thread(() -> {
            try { for (int i = 0; ; i++) cp.submitDrawBatch(0, 3, 2048, 3); }
            catch (IllegalStateException expected) { }
        });
        busy.start();
        Thread.sleep(50);
        cp.close();
        busy.join(5000);
        check(!busy.isAlive() && cp.isClosed(), "concurrent close stops submitter cleanly");

        for (long p : new long[] {g, small, thr, mis, nc, cb, cg}) nDestroy(p);
        System.out.println("ALL " + passed + " CHECKS PASSED");
    }
}
