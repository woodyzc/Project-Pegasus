package com.pegasus.tbt

/**
 * Keeps a chatty group chat from becoming a hazard.
 *
 * A group can post every few seconds. One banner per message would cover the
 * metric cells for the whole ride and train the rider to ignore the thing
 * entirely -- which then also costs them the call that mattered.
 *
 * So messages from one conversation are folded: the first is shown at once,
 * the rest are counted, and the next one to arrive after the window carries
 * the total. The head unit draws that as "6 messages" under the name.
 *
 * No timers anywhere. A suppressed message is not scheduled for later, only
 * remembered -- so nothing fires on its own while the rider is reading a turn,
 * and there is nothing to cancel when the service stops.
 */
class AlertThrottle(
    private val windowMs: Long = 30_000,
    private val callWindowMs: Long = 60_000,
) {
    private val lastSentAtMs = HashMap<String, Long>()
    private val suppressedSince = HashMap<String, Int>()

    /**
     * The count to send for this alert, or null to send nothing at all.
     *
     * Calls never accumulate a count. A dialer re-posts its notification when
     * the call is answered or updated, and "2 messages" under a caller's name
     * would be nonsense -- so a repeat inside the window is dropped outright
     * rather than folded. The first banner already said who is calling.
     */
    fun admit(kind: Int, name: String, nowMs: Long): Int? {
        prune(nowMs)

        // A space cannot appear in the kind, so this cannot collide the way
        // concatenating two free strings would.
        val key = "$kind $name"
        val window = if (kind == AlertFrame.KIND_CALL) callWindowMs else windowMs
        val last = lastSentAtMs[key]

        // `nowMs > last` guards against a clock that steps backwards. The
        // caller passes a monotonic one, so this should be unreachable -- but
        // the failure if it ever is not would be a conversation silenced
        // indefinitely rather than anything noisy, and that is the kind of
        // bug nobody reports because it looks like nobody messaged.
        if (last != null && nowMs >= last && nowMs - last < window) {
            if (kind != AlertFrame.KIND_CALL) {
                suppressedSince[key] = (suppressedSince[key] ?: 0) + 1
            }
            return null
        }

        lastSentAtMs[key] = nowMs
        val folded = suppressedSince.remove(key) ?: 0
        return if (kind == AlertFrame.KIND_CALL) 1 else folded + 1
    }

    /**
     * Forgets conversations nothing has come from in a long time, so a day of
     * riding through a busy chat does not grow these maps without bound. Ten
     * windows is far longer than a fold could still be relevant for.
     */
    private fun prune(nowMs: Long) {
        val cutoff = nowMs - (maxOf(callWindowMs, windowMs) * 10)
        val stale = lastSentAtMs.filterValues { it < cutoff }.keys.toList()
        for (key in stale) {
            lastSentAtMs.remove(key)
            suppressedSince.remove(key)
        }
    }
}
