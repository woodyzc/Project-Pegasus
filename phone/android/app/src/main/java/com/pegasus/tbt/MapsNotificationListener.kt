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
 * maneuver to the head unit -- and, from the same stream, forwards calls,
 * texts and chat messages as alerts.
 *
 * The two jobs share this class because they share the one scarce thing:
 * notification access is a single grant the user makes in system settings,
 * and a second listener service would need its own. Everything past the
 * package check is separate, and the alert half's decisions live in
 * AlertClassifier and AlertThrottle, which are pure and tested.
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

        /**
         * Alerts, counted the same way and for the same reason: "my phone
         * rang and nothing happened" has several causes that look identical
         * from the saddle.
         *   alertsSeen  - notifications from any app that reached us
         *   alertsSent  - those that were classified, admitted and written
         *   alertsFolded - those folded into a later banner by the throttle
         */
        @Volatile
        var alertsSeen: Int = 0
            private set

        @Volatile
        var alertsSent: Int = 0
            private set

        @Volatile
        var alertsFolded: Int = 0
            private set

        @Volatile
        var lastAlert: String = "none yet"
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

        // What was last put on the wire, so a distance-less repeat of the same
        // step can be recognised and dropped.
        @Volatile
        private var lastSentIcon: Int = -1

        @Volatile
        private var lastSentStreet: String = ""

        /** Status notifications carrying no maneuver ("Rerouting", ...). */
        @Volatile
        var transient: Int = 0
            private set

        /** Notifications Android blanked out before the listener saw them. */
        @Volatile
        var redacted: Int = 0
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

        private fun rememberUnparsed(title: String?, text: String?) {
            // ManeuverParser.unparsedKey collapses the counting-down distance
            // so one wording takes one slot; keeping that with the rest of the
            // text handling means a host test can cover it.
            val key = ManeuverParser.unparsedKey(title, text)
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

    /**
     * Per-service rather than per-notification, so a group chat is folded
     * across the whole ride. It is reset only when the system destroys this
     * listener, which is the right lifetime: a fold that survived a restart
     * would report a count for messages the rider already saw.
     */
    private val throttle = AlertThrottle()

    override fun onListenerConnected() {
        Log.i(TAG, "Notification access granted")
        // The listener can be bound by the system before the user ever opens
        // the app -- after a reboot, for instance -- so make sure the link is
        // up rather than assuming MainActivity started it.
        //
        // startIfEnabled, not start: Android rebinds this listener for as long
        // as notification access is granted, including straight after a Force
        // stop, and an unconditional start here made the service impossible to
        // kill from the phone.
        TbtService.startIfEnabled(applicationContext)
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        // Stopped means stopped: the system keeps this listener bound whatever
        // the user does, so it has to check for itself rather than assume no
        // notifications will arrive.
        //
        // Checked before the package split, so it governs alerts as well as
        // navigation. Stopping the service must silence the head unit
        // completely, not just stop the turns.
        if (!TbtService.isEnabled(applicationContext)) return

        if (sbn.packageName != MAPS_PACKAGE) {
            // Everything that is not Maps: a call, a text, a chat message, or
            // -- overwhelmingly -- nothing worth a rider's attention. This
            // line used to be a bare `return`.
            handleAlert(sbn)
            return
        }

        seen++
        lastAtMs = System.currentTimeMillis()

        val notification = sbn.notification ?: return
        // Navigation is an ongoing notification; ignore the rest (offers,
        // timeline prompts, "rate this place" and so on).
        if (notification.flags and Notification.FLAG_ONGOING_EVENT == 0) return
        used++

        val title = readTitle(notification)
        val text = readText(notification)

        // "Rerouting", "Starting navigation" and the redacted-content notice
        // are not maneuvers and never were. Counting them as parse failures
        // made the success rate unreadable, and they crowded the six remembered
        // sample slots with wordings there is nothing to fix about.
        val skip = ManeuverParser.skipReason(title, text)
        if (skip != null) {
            when (skip) {
                ManeuverParser.Skip.TRANSIENT -> transient++
                ManeuverParser.Skip.REDACTED -> redacted++
            }
            return
        }

        val maneuver = ManeuverParser.parse(title, text)
        if (maneuver == null) {
            rememberUnparsed(title, text)
            lastParse = "unparsed: title='$title' text='$text'"
            Log.d(TAG, lastParse)
            return
        }
        parsed++

        // The raw strings are kept even on success. A parse that succeeds but
        // is subtly wrong -- a road name that swallowed part of an aside, say
        // -- is invisible without seeing what Maps actually sent.
        val shownDistance = if (maneuver.hasDistance) "${maneuver.distanceMetres}m" else "unknown"
        lastParse = "icon=${maneuver.iconId} dist=$shownDistance " +
            "street='${maneuver.streetName}'\nraw: '$title' / '$text'"
        Log.d(TAG, lastParse)

        // A distance-less posting of the turn already on screen must not
        // replace a good number with a dash. Maps interleaves the two forms
        // for the same step, so without this the distance would flicker
        // between counting down and "--" every few seconds.
        val sameManeuver = maneuver.iconId == lastSentIcon && maneuver.streetName == lastSentStreet
        if (!maneuver.hasDistance && sameManeuver) {
            return
        }
        lastSentIcon = maneuver.iconId
        lastSentStreet = maneuver.streetName

        TbtService.link?.send(
            TbtFrame.encode(maneuver.iconId, maneuver.distanceMetres, maneuver.streetName)
        )
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PACKAGE) return
        // Navigation stopped. Clear immediately rather than waiting for the
        // firmware's 30s staleness timeout to notice.
        lastParse = "navigation ended (failures below are from the drive)"
        // Forget what was on screen: the next route must send its first step
        // even if it happens to open with the same turn onto the same road.
        lastSentIcon = -1
        lastSentStreet = ""
        TbtService.link?.send(TbtFrame.clearFrame(), force = true)
    }

    /**
     * A notification from some app other than Maps, on its way to the head
     * unit's banner -- or, far more often, to the floor.
     *
     * This method deliberately holds no rules. It reads four fields, asks
     * AlertClassifier what they mean and AlertThrottle whether to send, and
     * writes the frame. Everything that can be got wrong is in those two,
     * where it can be tested without a phone.
     */
    private fun handleAlert(sbn: StatusBarNotification) {
        // Our own foreground-service notification, which is posted and
        // updated on every status change. It could never classify as an alert
        // -- it is ongoing, and this package is in no allowlist -- but it is
        // the single most frequent thing arriving here, so it would dominate
        // both the seen counter and the log that counter exists to explain.
        if (sbn.packageName == applicationContext.packageName) return

        val notification = sbn.notification ?: return
        alertsSeen++

        val alert = AlertClassifier.classify(
            packageName = sbn.packageName,
            category = notification.category,
            flags = notification.flags,
            title = readTitle(notification),
        )

        if (alert == null) {
            // Logged, because silence is ambiguous in the one way that
            // matters. "My phone rang and the head unit did nothing" has two
            // very different causes -- the notification never reached this
            // service, or it reached it and a rule turned it down -- and
            // without this line they look identical from adb.
            //
            // The package only. A title is a person's name or a group's, and
            // this fires for every notification the phone receives; putting
            // that stream in the log would be a worse privacy leak than the
            // message bodies this feature already refuses to send.
            Log.d(TAG, "alert ignored from ${sbn.packageName}")
            return
        }

        val count = throttle.admit(alert.kind, alert.name, System.currentTimeMillis())
        if (count == null) {
            alertsFolded++
            return
        }

        val sent = TbtService.link?.sendAlert(
            AlertFrame.encode(alert.kind, count, alert.name)
        ) ?: false
        if (sent) alertsSent++

        // The name is kept for the diagnostics screen even when the write
        // failed. "Classified correctly but the head unit was not connected"
        // and "never recognised it at all" are different problems and look
        // the same from the saddle.
        lastAlert = "kind=${alert.kind} count=$count name='${alert.name}' " +
            "from=${sbn.packageName} ${if (sent) "sent" else "NOT sent"}"
        Log.d(TAG, lastAlert)
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
