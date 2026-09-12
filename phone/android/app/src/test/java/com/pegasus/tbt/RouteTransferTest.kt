package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The route upload state machine.
 *
 * These are the cases that cannot be reproduced on a bench: a link that drops
 * mid-transfer, a head unit that stops acknowledging, a chunk that silently
 * never lands. The transfer is a pure state machine precisely so they can be
 * driven here instead.
 */
class RouteTransferTest {

    private fun chunks(n: Int): List<ByteArray> =
        (0 until n).map { byteArrayOf(it.toByte()) }

    /** Writes everything the machine offers, returning how many it handed out. */
    private fun drain(t: RouteTransfer, nowMs: Long): Int {
        var count = 0
        while (t.nextChunk(nowMs) != null) count++
        return count
    }

    @Test
    fun cleanTransferCompletesInOnePass() {
        val t = RouteTransfer(chunks(10))
        assertEquals(RouteTransfer.State.SENDING, t.state)

        assertEquals(10, drain(t, 0))
        assertEquals(RouteTransfer.State.AWAITING_PROGRESS, t.state)

        t.onProgress(10, 100)
        assertEquals(RouteTransfer.State.COMPLETE, t.state)
        assertEquals(1, t.pass)
        assertEquals(100, t.percent)
    }

    @Test
    fun theWindowBoundsWhatIsInFlight() {
        val t = RouteTransfer(chunks(100), windowSize = 32)
        assertEquals(32, drain(t, 0))
        assertEquals(RouteTransfer.State.AWAITING_PROGRESS, t.state)

        // Progress short of the total resumes sending rather than restarting.
        t.onProgress(32, 10)
        assertEquals(RouteTransfer.State.SENDING, t.state)
        assertEquals(1, t.pass)
        assertEquals(32, drain(t, 20))
    }

    /** The case the design exists for: a chunk that never lands. The head unit
     * cannot say which, so the whole set goes again. */
    @Test
    fun aMissingChunkTriggersAnotherPass() {
        val t = RouteTransfer(chunks(10))
        drain(t, 0)
        t.onProgress(9, 100) // one short

        assertEquals(RouteTransfer.State.SENDING, t.state)
        assertEquals(2, t.pass)
        // And the second pass offers every chunk again, not just the tail.
        assertEquals(10, drain(t, 110))

        t.onProgress(10, 200)
        assertEquals(RouteTransfer.State.COMPLETE, t.state)
    }

    @Test
    fun givesUpAfterMaxPasses() {
        val t = RouteTransfer(chunks(4))
        var now = 0L
        // Each pass sends everything and is told the count never moved.
        repeat(RouteTransfer.MAX_PASSES) {
            drain(t, now)
            t.onProgress(3, now)
            now += 100
        }
        assertEquals(RouteTransfer.State.FAILED, t.state)
        assertFalse(t.onTick(now))
        assertNull("a failed transfer offers nothing", t.nextChunk(now))
    }

    /** A head unit that stops acknowledging entirely: no progress report ever
     * arrives, so only the clock can notice. */
    @Test
    fun aStallStartsAnotherPass() {
        val t = RouteTransfer(chunks(10))
        drain(t, 0) // now AWAITING_PROGRESS, stall timer started at 0

        assertTrue(t.onTick(RouteTransfer.STALL_TIMEOUT_MS - 1))
        assertEquals(RouteTransfer.State.AWAITING_PROGRESS, t.state)

        assertTrue(t.onTick(RouteTransfer.STALL_TIMEOUT_MS))
        assertEquals(RouteTransfer.State.SENDING, t.state)
        assertEquals(2, t.pass)
    }

    /** Progress that moves must reset the stall timer, or a slow but healthy
     * transfer gets restarted underneath itself. */
    @Test
    fun movingProgressResetsTheStallTimer() {
        val t = RouteTransfer(chunks(100), windowSize = 10)
        drain(t, 0)

        t.onProgress(5, 3_000)
        drain(t, 3_000)
        // 3,500 is more than STALL_TIMEOUT_MS after the start, but only 500ms
        // after the last real progress, so the transfer must be left alone.
        assertTrue(t.onTick(3_500))
        assertEquals("still the first pass", 1, t.pass)
    }

    /** A repeated identical count is not itself a stall: reports arrive after
     * every write, and a resend of chunks the head unit already holds produces
     * several in a row that do not move. */
    @Test
    fun repeatedIdenticalProgressIsNotAStall() {
        val t = RouteTransfer(chunks(100), windowSize = 10)
        drain(t, 0)
        t.onProgress(10, 1_000)

        drain(t, 1_000)
        t.onProgress(10, 1_100)
        t.onProgress(10, 1_200)
        assertEquals(1, t.pass)
        assertTrue(t.onTick(1_300))
        assertEquals(1, t.pass)
    }

    /** A dropped link restarts the transfer but must not spend one of the five
     * attempts: reconnecting is not evidence the route cannot be delivered. */
    @Test
    fun aDroppedLinkRestartsWithoutCostingAPass() {
        val t = RouteTransfer(chunks(50))
        drain(t, 0)
        assertEquals(1, t.pass)

        t.onDisconnected()
        assertEquals(RouteTransfer.State.SENDING, t.state)
        assertEquals("a reconnect is not a failed pass", 1, t.pass)
        // From the top, and the window still applies -- 50 chunks, 32 offered.
        assertEquals(RouteTransfer.DEFAULT_WINDOW, drain(t, 100))
    }

    @Test
    fun disconnectAfterCompletionChangesNothing() {
        val t = RouteTransfer(chunks(4))
        drain(t, 0)
        t.onProgress(4, 10)
        assertEquals(RouteTransfer.State.COMPLETE, t.state)

        t.onDisconnected()
        assertEquals(RouteTransfer.State.COMPLETE, t.state)
        assertNull(t.nextChunk(20))
    }

    @Test
    fun progressIsClampedToWhatWasSent() {
        val t = RouteTransfer(chunks(10))
        drain(t, 0)
        // A head unit reporting nonsense must not produce a percentage over
        // 100 or a negative one.
        t.onProgress(999, 10)
        assertEquals(100, t.percent)
        assertEquals(10, t.acknowledged)

        val u = RouteTransfer(chunks(10))
        drain(u, 0)
        u.onProgress(-5, 10)
        assertEquals(0, u.percent)
        assertEquals(0, u.acknowledged)
    }

    @Test
    fun percentTracksAcknowledgedChunks() {
        val t = RouteTransfer(chunks(200), windowSize = 200)
        drain(t, 0)
        t.onProgress(50, 10)
        assertEquals(25, t.percent)
        t.onProgress(100, 20)
        assertEquals(50, t.percent)
    }

    @Test
    fun aRealRouteProducesAPlausibleChunkCount() {
        val points = (0 until 1200).map {
            RouteFrame.Point(39.0 + it * 0.0001, -77.0 + it * 0.0001)
        }
        val maneuvers = (0 until 30).map {
            RouteFrame.Maneuver(ManeuverParser.Icon.TURN_LEFT, 0, it * 300, "Street $it")
        }
        val encoded = RouteFrame.encode(7, points, maneuvers, 36_000)
        val t = RouteTransfer(encoded.chunks)

        // 1200 points is 9,600 bytes and 30 maneuvers another 1,200: about
        // 60 chunks of 180 bytes plus the manifest.
        assertTrue("chunk count ${t.totalChunks}", t.totalChunks in 55..65)
        assertNotNull(t.nextChunk(0))
    }

    /**
     * Reproduces the "11 of 13 chunks, pass 5" failure seen on real hardware.
     *
     * Android accepts one outstanding GATT write at a time and refuses the
     * rest, so a caller draining the window gets a refusal on most of them.
     * Before onWriteRefused existed, each refusal consumed a chunk that was
     * then never written, and a 13-chunk route converged a couple short no
     * matter how many passes it spent.
     */
    @Test
    fun `a refused write does not lose its chunk`() {
        val chunks = (0 until 13).map { byteArrayOf(it.toByte()) }
        val transfer = RouteTransfer(chunks)

        val delivered = mutableSetOf<Int>()
        var now = 0L
        // The real stack: the first write of a burst lands, the next is
        // refused because one is already in flight.
        var acceptNext = true

        repeat(200) {
            val chunk = transfer.nextChunk(now) ?: run {
                transfer.onProgress(delivered.size, now)
                acceptNext = true
                return@repeat
            }
            if (acceptNext) {
                delivered += chunk[0].toInt()
                acceptNext = false
            } else {
                transfer.onWriteRefused()
                // The write callback that frees the queue again.
                acceptNext = true
            }
            now += 1
        }

        assertEquals(RouteTransfer.State.COMPLETE, transfer.state)
        assertEquals(13, delivered.size)
    }

    @Test
    fun `handing a chunk back re-offers the same one`() {
        val chunks = (0 until 4).map { byteArrayOf(it.toByte()) }
        val transfer = RouteTransfer(chunks)

        assertEquals(0, transfer.nextChunk(0L)!![0].toInt())
        val second = transfer.nextChunk(0L)!![0].toInt()
        assertEquals(1, second)
        transfer.onWriteRefused()
        // The same chunk, not the one after it.
        assertEquals(1, transfer.nextChunk(0L)!![0].toInt())
    }
}
