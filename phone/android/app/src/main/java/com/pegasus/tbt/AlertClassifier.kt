package com.pegasus.tbt

/**
 * Decides whether a notification is worth interrupting a ride for, and as
 * what.
 *
 * Pure on purpose -- no Android types, so the rules can be tested directly.
 * The listener's job is reduced to reading four fields off the notification
 * and passing them here, which is the same split ManeuverParser uses and for
 * the same reason: the part that can be got wrong should be the part that can
 * be run on a laptop.
 */
object AlertClassifier {

    /** What the head unit will be told. A null classification means silence. */
    data class Alert(val kind: Int, val name: String)

    // Android's Notification.CATEGORY_CALL, repeated as a plain string so
    // this file stays free of Android imports. It is a public API constant
    // and its value is fixed.
    //
    // CATEGORY_MESSAGE is deliberately absent: the allowlists below decide
    // what counts as a message, so the category is never consulted for one.
    const val CATEGORY_CALL = "call"

    const val FLAG_ONGOING_EVENT = 0x00000002
    const val FLAG_GROUP_SUMMARY = 0x00000200

    /**
     * Dialers, as a backstop for the category check. Most set CATEGORY_CALL;
     * these are named in case a vendor's does not.
     */
    val CALL_PACKAGES = setOf(
        "com.google.android.dialer",
        "com.android.dialer",
        "com.android.server.telecom",
        "com.samsung.android.dialer",
        "com.samsung.android.incallui",
    )

    /** Text messages. Deliberately a list, not "anything with CATEGORY_MESSAGE". */
    val SMS_PACKAGES = setOf(
        "com.google.android.apps.messaging",
        "com.samsung.android.messaging",
        "com.android.mms",
    )

    /**
     * Chat apps. WeChat is the one that was asked for; the set exists so
     * adding another is a one-line change rather than a new branch.
     *
     * An allowlist rather than "every app that posts CATEGORY_MESSAGE",
     * because the latter means email, Slack, and every shopping app that
     * calls its promotions messages -- a panel that lights up all ride.
     */
    val CHAT_PACKAGES = setOf(
        "com.tencent.mm", // WeChat
    )

    fun classify(
        packageName: String,
        category: String?,
        flags: Int,
        title: String?,
    ): Alert? {
        // The roll-up an app posts above a group of its own notifications.
        // It duplicates the individual ones that arrive with it, so honouring
        // both would banner every message twice.
        if (flags and FLAG_GROUP_SUMMARY != 0) return null

        val isCall = category == CATEGORY_CALL || packageName in CALL_PACKAGES

        // ---- The ongoing filter, and the one exception to it ----
        // An ongoing notification is a status, not an event: a music player, a
        // running timer, a VPN, and -- the one that made this necessary --
        // WeChat's permanent "running in background" notice. None is news.
        //
        // A ringing phone is the exception. Dialers mark an incoming call
        // ongoing too, because it is a live thing rather than a past event, so
        // applying this rule to calls would silence exactly the alert that
        // matters most.
        if (!isCall && flags and FLAG_ONGOING_EVENT != 0) return null

        val name = title?.trim().orEmpty()

        if (isCall) {
            // No name is normal here: a number that is not in the address book
            // has nobody to name, and the head unit still has to ring.
            return Alert(AlertFrame.KIND_CALL, name)
        }
        if (packageName in SMS_PACKAGES) {
            return Alert(AlertFrame.KIND_SMS, name)
        }
        if (packageName in CHAT_PACKAGES) {
            // For WeChat the title is the conversation: the sender in a
            // one-to-one chat, the group's name in a group. Either is the
            // right thing for the banner, which is why the text -- holding
            // "sender: message" in a group -- is not read at all.
            //
            // A locked phone set to hide content posts the app's own name here
            // instead. That still goes through, with whatever name it gave:
            // the app name and no sender is less than the rider wants but more
            // than silence, and special-casing it would only drop real
            // messages from anyone whose name matches.
            return Alert(AlertFrame.KIND_CHAT, name)
        }
        return null
    }
}
