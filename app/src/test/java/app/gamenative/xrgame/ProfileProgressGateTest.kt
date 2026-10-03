package app.gamenative.xrgame

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicInteger

class ProfileProgressGateTest {
    @Test fun progressIsBoundedAndSilentPeriodsDoNotCreateCatchUpBursts() {
        val gate = ProfileProgressGate()
        assertTrue(gate.sample(10))
        repeat(10000) { assertFalse(gate.sample(999999999)) }
        assertTrue(gate.sample(1000000010))
        assertFalse(gate.sample(1000000010))
        assertTrue(gate.sample(9000000000))
        assertFalse(gate.sample(9000000000))
        assertTrue(ProfileProgressGate().sample(9000000000)) // independent operation
    }
    @Test fun concurrentWorkersShareOneBudget() {
        val gate = ProfileProgressGate()
        val ready = CountDownLatch(8)
        val go = CountDownLatch(1)
        val accepted = AtomicInteger()
        val workers = List(8) { Thread {
            ready.countDown(); go.await()
            repeat(1000) { if (gate.sample(123)) accepted.incrementAndGet() }
        }.apply { start() } }
        ready.await(); go.countDown(); workers.forEach { it.join() }
        assertEquals(1, accepted.get())
    }
    @Test fun uninitializedBackendReturnsSharedNoop() {
        assertSame(XrGameProfiler.noop(), XrGameProfiler.region("not_initialized"))
        XrGameProfiler.noop().close()
        XrGameProfiler.counter(0, 1)
    }
}
