package com.pegasus.tbt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The folding that keeps a busy group chat from owning the panel.
 *
 * Time is passed in rather than read, so these run in microseconds and the
 * boundary cases are exact -- a throttle tested against a real clock is a
 * throttle tested at whatever moment the suite happened to run.
 */
class AlertThrottleTest {

    @Test
    fun `the first message of a conversation goes straight through`() {
        val t = AlertThrottle()
        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "张三", 0))
    }

    @Test
    fun `a burst produces one banner, then a count`() {
        val t = AlertThrottle(windowMs = 30_000)

        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "group", 0))
        // Five more in the next twenty seconds: all folded, none shown.
        for (i in 1..5) {
            assertNull(t.admit(AlertFrame.KIND_CHAT, "group", i * 4_000L))
        }
        // The next one past the window carries the five it stood in for, plus
        // itself.
        assertEquals(6, t.admit(AlertFrame.KIND_CHAT, "group", 31_000))
    }

    @Test
    fun `the fold resets once it has been sent`() {
        val t = AlertThrottle(windowMs = 30_000)
        t.admit(AlertFrame.KIND_CHAT, "group", 0)
        t.admit(AlertFrame.KIND_CHAT, "group", 1_000)
        assertEquals(2, t.admit(AlertFrame.KIND_CHAT, "group", 31_000))
        // A quiet spell, then one message: it stands for itself alone.
        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "group", 200_000))
    }

    @Test
    fun `conversations are folded separately`() {
        // Two busy chats must not hide each other -- the rider needs to know
        // which one is asking.
        val t = AlertThrottle(windowMs = 30_000)
        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "A", 0))
        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "B", 100))
        assertNull(t.admit(AlertFrame.KIND_CHAT, "A", 200))
        assertNull(t.admit(AlertFrame.KIND_CHAT, "B", 300))
        assertEquals(2, t.admit(AlertFrame.KIND_CHAT, "A", 31_000))
        assertEquals(2, t.admit(AlertFrame.KIND_CHAT, "B", 31_100))
    }

    @Test
    fun `the same name in two apps is two conversations`() {
        // A text and a WeChat message from the same person are different
        // events and the banner says which.
        val t = AlertThrottle(windowMs = 30_000)
        assertEquals(1, t.admit(AlertFrame.KIND_SMS, "Alex", 0))
        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "Alex", 100))
    }

    @Test
    fun `the window boundary admits rather than folds`() {
        val t = AlertThrottle(windowMs = 30_000)
        t.admit(AlertFrame.KIND_CHAT, "group", 1_000)
        assertNull(t.admit(AlertFrame.KIND_CHAT, "group", 30_999))
        assertEquals(2, t.admit(AlertFrame.KIND_CHAT, "group", 31_000))
    }

    @Test
    fun `a repeated call is dropped, not counted`() {
        // A dialer re-posts its notification when the call is answered or
        // updated. Folding those would put "3 messages" under a caller's name,
        // which is nonsense -- the first banner already said who it was.
        val t = AlertThrottle(callWindowMs = 60_000)
        assertEquals(1, t.admit(AlertFrame.KIND_CALL, "Mum", 0))
        assertNull(t.admit(AlertFrame.KIND_CALL, "Mum", 500))
        assertNull(t.admit(AlertFrame.KIND_CALL, "Mum", 30_000))
        // And when she calls again later, it is still one call, not three.
        assertEquals(1, t.admit(AlertFrame.KIND_CALL, "Mum", 61_000))
    }

    @Test
    fun `a different caller is never suppressed by the first`() {
        val t = AlertThrottle(callWindowMs = 60_000)
        assertEquals(1, t.admit(AlertFrame.KIND_CALL, "Mum", 0))
        assertEquals(1, t.admit(AlertFrame.KIND_CALL, "Alex", 1_000))
    }

    @Test
    fun `unnamed callers all share one slot`() {
        // Every withheld number arrives with the same empty name, so they
        // cannot be told apart. Treating them as one conversation is the
        // honest reading, and it keeps a spam dialler from banner-flooding.
        val t = AlertThrottle(callWindowMs = 60_000)
        assertEquals(1, t.admit(AlertFrame.KIND_CALL, "", 0))
        assertNull(t.admit(AlertFrame.KIND_CALL, "", 1_000))
    }

    @Test
    fun `a clock that steps backwards does not silence a conversation`() {
        // The caller passes SystemClock.elapsedRealtime(), which cannot do
        // this. The test exists because the consequence if anything ever
        // changed that is invisible: not a crash, not a wrong banner, just a
        // conversation that stops arriving and looks exactly like nobody
        // having messaged.
        val t = AlertThrottle(windowMs = 30_000)
        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "group", 1_000_000))
        // An NTP correction drags the clock back an hour.
        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "group", 1_000_000 - 3_600_000))
    }

    @Test
    fun `a long ride through many conversations does not grow without bound`() {
        // Each name is seen once and never again. Nothing asserts on internal
        // state -- the point is that this completes and that the throttle
        // still behaves afterwards.
        val t = AlertThrottle(windowMs = 30_000, callWindowMs = 60_000)
        for (i in 0 until 5_000) {
            t.admit(AlertFrame.KIND_CHAT, "chat-$i", i * 1_000L)
        }
        assertEquals(1, t.admit(AlertFrame.KIND_CHAT, "after", 5_000_000))
    }
}
