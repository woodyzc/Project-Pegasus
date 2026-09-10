package com.pegasus.tbt

import android.app.Notification
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView

/**
 * Watches Google Maps' ongoing navigation notification and forwards each
 * maneuver to the head unit.
 *
 * This exists because Maps exposes no turn-by-turn API. It is therefore built
 * on a UI surface Google can change at any release, and it is the least
 * durable part of this project. Two mitigations:
 *
 *  - Text that cannot be understood produces no frame at all (see
 *    ManeuverParser), so a Maps update degrades to "no turn shown" rather than
 *    to a wrong turn.
 *  - Navigation ending clears the head unit explicitly instead of leaving the
 *    last maneuver on screen.
 *
 * Requires the user to grant notification access in
 * Settings > Apps > Special app access > Notification access. There is no
 * permission dialog for this; it can only be enabled from system settings.
 */
class MapsNotificationListener : NotificationListenerService() {

    companion object {
        private const val TAG = "PegasusNotif"
        private const val MAPS_PACKAGE = "com.google.android.apps.maps"

        @Volatile
        var lastParse: String = "nothing seen yet"
            private set

        // Instrumentation, because "the update rate feels low" has three very
        // different causes and they need telling apart:
        //   seen   - Maps notifications that arrived at all
        //   used   - those that passed the ongoing-event filter
        //   parsed - those that yielded a maneuver
        // seen >> used means the filter is discarding real updates; used >>
        // parsed means the wording is not matching; seen barely climbing means
        // Maps itself is posting slowly and no amount of tuning here helps.
        @Volatile
        var seen: Int = 0
            private set

        @Volatile
        var used: Int = 0
            private set

        @Volatile
        var parsed: Int = 0
            private set

        @Volatile
        var lastAtMs: Long = 0L
            private set

        // Distinct phrasings the parser could not read, oldest first.
        //
        // Keeping only the most recent one was not enough: a drive ends with
        // "navigation ended", which overwrote the single sample and destroyed
        // the evidence for every failure that happened along the way. A drive
        // that reports 101 failures should hand back the wordings that caused
        // them, not a blank.
        private const val MAX_SAMPLES = 6
        private val samples = LinkedHashSet<String>()

        fun unparsedSamples(): List<String> = synchronized(samples) { samples.toList() }

        private fun rememberUnparsed(title: String?) {
            // ManeuverParser.unparsedKey collapses the counting-down distance
            // so one wording takes one slot; keeping that with the rest of the
            // text handling means a host test can cover it.
            val key = ManeuverParser.unparsedKey(title)
            synchronized(samples) {
                if (samples.contains(key)) return
                if (samples.size >= MAX_SAMPLES) {
                    samples.remove(samples.first())
                }
                samples.add(key)
            }
        }

        /** Seconds since the last Maps notification, or -1 if none yet. */
        fun secondsSinceLast(): Float =
            if (lastAtMs == 0L) -1f else (System.currentTimeMillis() - lastAtMs) / 1000f
    }

    override fun onListenerConnected() {
        Log.i(TAG, "Notification access granted")
        // The listener can be bound by the system before the user ever opens
        // the app -- after a reboot, for instance -- so make sure the link is
        // up rather than assuming MainActivity started it.
        TbtService.start(applicationContext)
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PACKAGE) return

        seen++
        lastAtMs = System.currentTimeMillis()

        val notification = sbn.notification ?: return
        // Navigation is an ongoing notification; ignore the rest (offers,
        // timeline prompts, "rate this place" and so on).
        if (notification.flags and Notification.FLAG_ONGOING_EVENT == 0) return
        used++

        val title = readTitle(notification)
        val text = readText(notification)

        val maneuver = ManeuverParser.parse(title, text)
        if (maneuver == null) {
            rememberUnparsed(title)
            lastParse = "unparsed: title='$title' text='$text'"
            Log.d(TAG, lastParse)
            return
        }
        parsed++

        // The raw strings are kept even on success. A parse that succeeds but
        // is subtly wrong -- a road name that swallowed part of an aside, say
        // -- is invisible without seeing what Maps actually sent.
        lastParse = "icon=${maneuver.iconId} dist=${maneuver.distanceMetres}m " +
            "street='${maneuver.streetName}'\nraw: '$title' / '$text'"
        Log.d(TAG, lastParse)

        TbtService.link?.send(
            TbtFrame.encode(maneuver.iconId, maneuver.distanceMetres, maneuver.streetName)
        )
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PACKAGE) return
        // Navigation stopped. Clear immediately rather than waiting for the
        // firmware's 30s staleness timeout to notice.
        lastParse = "navigation ended (failures below are from the drive)"
        TbtService.link?.send(TbtFrame.clearFrame(), force = true)
    }

    private fun readTitle(n: Notification): String? =
        n.extras?.getCharSequence(Notification.EXTRA_TITLE)?.toString()
            ?: remoteViewsText(n).getOrNull(0)

    private fun readText(n: Notification): String? =
        n.extras?.getCharSequence(Notification.EXTRA_TEXT)?.toString()
            ?: remoteViewsText(n).getOrNull(1)

    /**
     * Fallback for Maps versions that ship a custom RemoteViews layout instead
     * of populating EXTRA_TITLE / EXTRA_TEXT -- in that case the extras come
     * back null and the only way to the strings is to inflate the view and
     * walk it.
     *
     * Best effort by nature: it depends on Maps' private view hierarchy and
     * the order text appears in it. Returns an empty list when anything goes
     * wrong, which the parser treats as "no maneuver".
     */
    private fun remoteViewsText(n: Notification): List<String> {
        val views = n.bigContentView ?: n.contentView ?: return emptyList()
        return runCatching {
            val parent = FrameLayout(applicationContext)
            val inflated = views.apply(applicationContext, parent)
            val out = mutableListOf<String>()
            collectText(inflated, out)
            out
        }.getOrElse {
            Log.w(TAG, "RemoteViews inflation failed: ${it.message}")
            emptyList()
        }
    }

    private fun collectText(view: View, into: MutableList<String>) {
        if (view is TextView) {
            val value = view.text?.toString()?.trim()
            if (!value.isNullOrEmpty()) into.add(value)
        }
        if (view is ViewGroup) {
            for (i in 0 until view.childCount) {
                collectText(view.getChildAt(i), into)
            }
        }
    }
}
