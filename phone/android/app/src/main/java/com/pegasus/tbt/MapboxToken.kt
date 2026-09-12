package com.pegasus.tbt

import android.content.Context

/**
 * The Mapbox public token, entered on the phone instead of compiled in.
 *
 * It used to be a BuildConfig field read from local.properties, and that was
 * measurably worse. A token in BuildConfig lands in the dex as a plain string:
 * unzip the APK, run `strings` on classes*.dex, and there it is. That was
 * verified with a canary token, not assumed. Two consequences followed --
 * every APK built here carried a live billing credential, and rotating the
 * token meant a rebuild and a reinstall.
 *
 * Kept here instead, in the app's private preferences, it is reachable only by
 * compromising the device: the manifest sets allowBackup="false", so there is
 * no backup path off the phone either. That is not secrecy -- the Navigation
 * SDK needs the token client-side and no client-side token can be kept from
 * the device's owner -- but it is the difference between one shell command and
 * a rooted phone.
 *
 * What actually caps the bill lives on Mapbox's side: give this app its own
 * token with minimum scopes so it can be revoked alone, and watch the usage
 * graph. Rotation is the reason this screen exists; it costs a paste.
 */
object MapboxToken {

    // The same preferences file TbtService uses for the link-enabled flag.
    // MODE_PRIVATE, so no other app can read it.
    private const val PREFS = "pegasus_tbt"
    private const val KEY = "mapbox_access_token"

    /** The stored token, or an empty string if none has been entered. */
    fun load(context: Context): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(KEY, "")
            .orEmpty()

    /**
     * Stores a pasted token, or returns why it was refused and stores nothing.
     *
     * Refusing at the moment of entry is the whole point. The two tokens are
     * easy to mix up and neither Mapbox nor the SDK says "wrong token" -- the
     * secret one produces routing requests that are simply rejected, hours
     * later, on a road.
     */
    fun save(context: Context, raw: String): String? {
        val token = raw.trim()
        Rules.rejectionReason(token)?.let { return it }
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY, token)
            .apply()
        return null
    }

    /** Forgets the token. Used when rotating, and after a suspected leak. */
    fun clear(context: Context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .remove(KEY)
            .apply()
    }

    fun isPresent(context: Context): Boolean = load(context).isNotEmpty()

    /**
     * Validation and display, free of Android types so they unit test on the
     * JVM. Everything worth getting right here is in this object.
     */
    object Rules {

        /** The shortest thing after "pk." that could plausibly be a token. */
        private const val MIN_BODY = 10

        /**
         * Why this string cannot be used as a public token, or null if it can.
         *
         * Deliberately lenient about length. A real token is far longer than
         * the floor below, but guessing Mapbox's exact format would mean
         * refusing a valid token some day, which is a worse failure than
         * accepting a mistyped one -- the mistyped one is reported by
         * unavailableReason() as soon as routing is attempted.
         */
        fun rejectionReason(raw: String): String? {
            val token = raw.trim()
            return when {
                token.isEmpty() ->
                    "Nothing entered."

                // Named first and explicitly: this is the mistake, and the
                // generic message below would not explain it.
                token.startsWith("sk.") ->
                    "That is the secret token. It belongs in Gradle, for " +
                        "downloading the SDK. The app needs the public one, " +
                        "which starts with pk."

                !token.startsWith("pk.") ->
                    "A Mapbox public token starts with pk."

                // A paste that picked up a line break or a stray word would
                // otherwise be stored and fail much later.
                token.any { it.isWhitespace() } ->
                    "That has a space or line break in it. Paste the token alone."

                token.length - 3 < MIN_BODY ->
                    "That looks truncated. Copy the whole token."

                else -> null
            }
        }

        /**
         * The token as it is safe to put on screen: enough to tell which token
         * it is, not enough to use.
         *
         * This matters more than it looks. Screenshots of this screen get sent
         * around while debugging -- that is how the head unit's panel gets
         * reported -- and a token printed in full leaks the moment one is
         * shared.
         */
        fun redact(raw: String): String {
            val token = raw.trim()
            if (token.isEmpty()) return "none"
            if (token.length <= 12) return "pk.…"
            return token.take(7) + "…" + token.takeLast(4)
        }
    }
}
