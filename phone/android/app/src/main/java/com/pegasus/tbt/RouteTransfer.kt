package com.pegasus.tbt

/**
 * Drives a chunked route upload, and decides what to send next.
 *
 * Pure state machine: no Android types, no BLE, no clock of its own -- the
 * caller supplies the time and performs the writes. That is what makes the
 * awkward parts testable, and the awkward parts are the whole point. A route
 * is hundreds of writes over a link that drops, and the failure this is built
 * around is the quiet one: a chunk that never lands leaves a permanent hole in
 * a route that otherwise looks complete.
 *
 * The head unit reports progress as a count, not a bitmap, so this cannot know
 * WHICH chunk is missing. It therefore works in passes: send everything, ask
 * how many landed, and if the count is short send the whole set again. That
 * sounds wasteful and is not, because the common case is a clean first pass --
 * and unlike a per-chunk ack scheme it needs no reverse channel, no timers per
 * chunk, and no bookkeeping that can itself go wrong.
 *
 * A resend is safe because the head unit places each chunk by index rather
 * than appending, so receiving one twice is a no-op rather than a corruption.
 */
class RouteTransfer(
    private val chunks: List<ByteArray>,
    /** Chunks written before waiting for a progress report. */
    private val windowSize: Int = DEFAULT_WINDOW,
) {

    companion object {
        /**
         * How many chunks to write before pausing for a progress report.
         *
         * Android's GATT queue is one outstanding write deep, and the stack
         * drops a write issued while another is in flight rather than queuing
         * it. The BLE callback paces the sending, so this is not a throughput
         * knob -- it is how much can be in flight before the transfer stops to
         * check itself. Small enough that a stall is noticed quickly, large
         * enough that the progress round trip is not paid per chunk.
         */
        const val DEFAULT_WINDOW = 32

        /** How long to wait for progress to move before assuming a stall. */
        const val STALL_TIMEOUT_MS = 4_000L

        /** Give up after this many full passes. */
        const val MAX_PASSES = 5
    }

    enum class State {
        /** Chunks remain to be written in the current pass. */
        SENDING,

        /** Window full or pass complete; waiting for the head unit's count. */
        AWAITING_PROGRESS,

        /** Every chunk acknowledged. */
        COMPLETE,

        /** Gave up after MAX_PASSES. */
        FAILED,
    }

    var state: State = State.SENDING
        private set

    var pass: Int = 1
        private set

    /** Chunks the head unit says it has, from the last progress report. */
    var acknowledged: Int = 0
        private set

    private var nextIndex = 0
    private var inFlight = 0
    private var lastProgressAt = 0L
    private var lastAcknowledged = -1

    val totalChunks: Int get() = chunks.size

    /** 0..100, for a progress bar. */
    val percent: Int
        get() = if (chunks.isEmpty()) 100 else (acknowledged * 100) / chunks.size

    /**
     * The next chunk to write, or null when the caller should stop and wait.
     * Call repeatedly until it returns null, writing each chunk it hands back.
     */
    fun nextChunk(nowMs: Long): ByteArray? {
        if (state != State.SENDING) return null

        if (nextIndex >= chunks.size || inFlight >= windowSize) {
            state = State.AWAITING_PROGRESS
            lastProgressAt = nowMs
            return null
        }

        val chunk = chunks[nextIndex]
        nextIndex++
        inFlight++
        return chunk
    }

    /**
     * Feeds in the head unit's progress report: how many chunks it has.
     *
     * A report that moves the count resets the stall timer. One that does not
     * is not itself a failure -- reports arrive after every write, and several
     * can land while a resend of already-held chunks is in flight.
     */
    fun onProgress(received: Int, nowMs: Long) {
        acknowledged = received.coerceIn(0, chunks.size)

        if (acknowledged != lastAcknowledged) {
            lastAcknowledged = acknowledged
            lastProgressAt = nowMs
        }

        if (acknowledged >= chunks.size) {
            state = State.COMPLETE
            return
        }

        if (state == State.AWAITING_PROGRESS) {
            // Window drained and the route is still short: start another pass
            // from the beginning. We cannot know which chunk is missing, and
            // re-sending one the head unit already has costs nothing.
            if (nextIndex >= chunks.size) {
                beginNextPass()
            } else {
                inFlight = 0
                state = State.SENDING
            }
        }
    }

    /**
     * Call when nothing has arrived for a while. Returns true if the transfer
     * is still alive, false once it has given up.
     */
    fun onTick(nowMs: Long): Boolean {
        if (state == State.COMPLETE) return true
        if (state == State.FAILED) return false

        if (nowMs - lastProgressAt >= STALL_TIMEOUT_MS) {
            beginNextPass()
        }
        return state != State.FAILED
    }

    /** Call when the link drops, so the next connection restarts cleanly. */
    fun onDisconnected() {
        if (state == State.COMPLETE || state == State.FAILED) return
        // Deliberately not counted as a pass. A dropped link is not evidence
        // the route cannot be sent, and spending one of five attempts on it
        // would make a reconnecting phone give up on a route it never had a
        // clean chance to deliver.
        nextIndex = 0
        inFlight = 0
        state = State.SENDING
    }

    private fun beginNextPass() {
        if (pass >= MAX_PASSES) {
            state = State.FAILED
            return
        }
        pass++
        nextIndex = 0
        inFlight = 0
        state = State.SENDING
    }
}
