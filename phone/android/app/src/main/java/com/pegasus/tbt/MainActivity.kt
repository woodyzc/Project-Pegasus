package com.pegasus.tbt

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.text.InputType
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

/**
 * A deliberately plain control panel: grant permissions, see whether the link
 * is up, and send a test frame without needing Maps at all.
 *
 * It deliberately owns nothing. The BLE link belongs to TbtService so it
 * survives this screen being destroyed -- which is the normal case, since the
 * phone spends the ride in a pocket. This class only starts that service and
 * reads its published state.
 *
 * The test button matters more than it looks -- it separates "the BLE link is
 * broken" from "the notification parsing is broken", which are the two ways
 * this app fails and are otherwise hard to tell apart on a bike.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var status: TextView
    private lateinit var parseStatus: TextView
    private lateinit var tokenStatus: TextView
    private lateinit var navStatus: TextView
    private lateinit var powerButton: Button
    private val handler = Handler(Looper.getMainLooper())

    private val refresh = object : Runnable {
        override fun run() {
            // Polled rather than pushed: the service is the source of truth and
            // this screen is often not alive to receive a callback.
            status.text = TbtService.status
            syncPowerButton()
            val age = MapsNotificationListener.secondsSinceLast()
            val cadence = if (age < 0) {
                "no Maps notification yet"
            } else {
                // Maneuvers only: status notifications and redacted ones are
                // excluded from the denominator and shown on their own line,
                // so this figure means "of the notifications that should have
                // held a turn, how many were read".
                val maneuvers = MapsNotificationListener.used -
                    MapsNotificationListener.transient - MapsNotificationListener.redacted
                "seen ${MapsNotificationListener.seen}" +
                    " / used ${MapsNotificationListener.used}" +
                    " / maneuvers $maneuvers" +
                    " / parsed ${MapsNotificationListener.parsed}" +
                    String.format("  (%.1fs ago)", age) +
                    "\nskipped: ${MapsNotificationListener.transient} status" +
                    ", ${MapsNotificationListener.redacted} hidden by Android"
            }

            // A redacted notification is the one failure mode no parser change
            // can reach -- the text is blanked before this app ever sees it --
            // so it gets an instruction rather than a number.
            val redactedNote = if (MapsNotificationListener.redacted > 0) {
                "\n\n⚠ Android is hiding Maps' notification text." +
                    "\nSettings > Notifications > Sensitive notifications: turn OFF." +
                    "\nTurn-by-turn cannot work until you do."
            } else {
                ""
            }

            val failures = MapsNotificationListener.unparsedSamples()
            val failureBlock = if (failures.isEmpty()) {
                ""
            } else {
                "\n\nUNPARSED (${failures.size} distinct):\n" + failures.joinToString("\n") { "· $it" }
            }
            // Alerts get their own line. They come from a different set of
            // apps and fail for different reasons, so folding them into the
            // Maps figures above would make both unreadable -- and "nothing
            // happened when my phone rang" needs an answer this screen can
            // give: seen but not classified, classified but folded, or sent
            // and the head unit was not listening.
            val alertLine = "\n\nalerts: seen ${MapsNotificationListener.alertsSeen}" +
                " / sent ${MapsNotificationListener.alertsSent}" +
                " / folded ${MapsNotificationListener.alertsFolded}" +
                "\n${MapsNotificationListener.lastAlert}"

            parseStatus.text =
                "$cadence\n${MapsNotificationListener.lastParse}$redactedNote$failureBlock$alertLine"
            // 200ms rather than a second: this line is the only window onto
            // what the parser is doing, and a second of lag makes a working
            // parser look broken while you watch it.
            handler.postDelayed(this, 200)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        status = TextView(this).apply { textSize = 16f; text = "Starting…" }
        parseStatus = TextView(this).apply { textSize = 13f; text = "" }

        val notifButton = Button(this).apply {
            text = "Grant notification access"
            setOnClickListener {
                // No runtime dialog exists for this one; system settings only.
                startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
            }
        }

        val testButton = Button(this).apply {
            text = "Send test turn (250 m, right)"
            setOnClickListener {
                val sent = TbtService.link?.send(
                    TbtFrame.encode(ManeuverParser.Icon.TURN_RIGHT, 250, "Test Street"),
                    force = true
                ) ?: false
                status.text = if (sent) "Test frame sent" else "Not connected"
            }
        }

        val clearButton = Button(this).apply {
            text = "Clear route"
            setOnClickListener { TbtService.link?.send(TbtFrame.clearFrame(), force = true) }
        }

        // Exercises the whole route upload without Mapbox and without a GPS
        // module, neither of which exists here yet. Everything except turning
        // a position into a turn is proved by this button: chunking,
        // reassembly, the head unit's PSRAM store and its progress reporting.
        val routeButton = Button(this).apply {
            text = "Send test route (400 m loop)"
            setOnClickListener {
                val link = TbtService.link
                if (link == null || !link.isConnected) {
                    status.text = "Not connected"
                    return@setOnClickListener
                }
                link.onRouteProgress = { percent, done ->
                    runOnUiThread {
                        status.text = if (done) "Route sent" else "Sending route… $percent%"
                    }
                }
                link.sendRoute(TestRoute.square().encode())
            }
        }

        // Destination entry and route planning. Present in every build; a
        // build without the Mapbox SDK has no route source and says so rather
        // than hiding the controls, because "the button is missing" is a worse
        // thing to debug than "the button explains itself".
        navStatus = TextView(this).apply { textSize = 13f }

        val destinationInput = EditText(this).apply {
            hint = "Destination: Maps link, or \"lat, lon\""
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }

        val planButton = Button(this).apply {
            text = "Plan cycling route"
            setOnClickListener {
                val source = TbtService.routeSource
                if (source == null) {
                    navStatus.text = "Routing: ${RouteSources.unavailable}"
                    return@setOnClickListener
                }
                when (val parsed = DestinationParser.parse(destinationInput.text.toString())) {
                    is DestinationParser.Result.Err ->
                        navStatus.text = "Routing: ${parsed.reason}"

                    is DestinationParser.Result.Ok -> {
                        val where = DestinationParser.describe(parsed.destination)
                        val refusal = source.requestRouteTo(parsed.destination)
                        navStatus.text = if (refusal == null) {
                            "Routing: planning to $where…"
                        } else {
                            "Routing: $refusal"
                        }
                    }
                }
            }
        }

        // Mapbox's public token, entered here rather than compiled in. A token
        // in BuildConfig ends up in the dex as a plain string and travels with
        // every APK; kept in the app's private preferences it needs the device
        // to be compromised instead. See MapboxToken.
        tokenStatus = TextView(this).apply { textSize = 13f }

        val tokenInput = EditText(this).apply {
            hint = "Paste Mapbox public token (pk.…)"
            // No suggestions and no autofill: a keyboard that learns the token
            // or an autofill service that stores it is another copy of a
            // credential, in a place neither of us controls.
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
            // Keep it out of saved instance state, so rotating the screen does
            // not write the token into a system-managed bundle.
            isSaveEnabled = false
        }

        val saveTokenButton = Button(this).apply {
            text = "Save token"
            setOnClickListener {
                val rejection = MapboxToken.save(this@MainActivity, tokenInput.text.toString())
                if (rejection == null) {
                    // Clear the field on success: leaving the token on screen
                    // is how it ends up in a screenshot.
                    tokenInput.setText("")
                }
                syncTokenStatus(rejection)
            }
        }

        val clearTokenButton = Button(this).apply {
            text = "Clear token"
            setOnClickListener {
                MapboxToken.clear(this@MainActivity)
                tokenInput.setText("")
                syncTokenStatus(null)
            }
        }

        // The way to turn the thing off. Without it the only apparent option
        // was Force stop, which the rebound notification listener promptly
        // undid -- so the app looked unkillable.
        powerButton = Button(this).apply {
            setOnClickListener {
                if (TbtService.isEnabled(this@MainActivity)) {
                    TbtService.stopByUser(this@MainActivity)
                } else {
                    TbtService.startByUser(this@MainActivity)
                }
                syncPowerButton()
            }
        }

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
            addView(status)
            addView(parseStatus)
            addView(notifButton)
            addView(testButton)
            addView(clearButton)
            addView(routeButton)
            addView(navStatus)
            addView(destinationInput)
            addView(planButton)
            addView(tokenStatus)
            addView(tokenInput)
            addView(saveTokenButton)
            addView(clearTokenButton)
            addView(powerButton)
        })

        requestRuntimePermissions()
        // Respect a previous stop rather than overriding it just because the
        // user opened the screen to look at the counters.
        TbtService.startIfEnabled(this)
        syncPowerButton()
        syncTokenStatus(null)
        syncNavStatus()
    }

    /**
     * Whether routing could plan a route right now, and why not if it could
     * not.
     *
     * Re-read rather than remembered: the service starts before the user has
     * answered the location dialog, so this line is wrong the moment it is
     * first drawn and right a few seconds later.
     */
    private fun syncNavStatus() {
        val source = TbtService.routeSource
        navStatus.text = "Routing: " + when {
            source == null -> RouteSources.unavailable
            else -> source.unavailableReason() ?: "ready"
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        // Location arrives here, after the service has already started without
        // it. Nothing else would ever start the trip session, so a granted
        // permission would otherwise leave routing permanently dead until the
        // next reboot. start() is idempotent for exactly this call.
        TbtService.routeSource?.start()
        syncNavStatus()
    }

    /**
     * Shows which token is stored, redacted, or why the last paste was refused.
     *
     * Redacted rather than printed: screenshots of this screen get shared while
     * debugging, and a token shown in full leaks with the first one.
     */
    private fun syncTokenStatus(rejection: String?) {
        val stored = MapboxToken.Rules.redact(MapboxToken.load(this))
        tokenStatus.text = if (rejection != null) {
            "Mapbox token: $stored\nNot saved: $rejection"
        } else {
            "Mapbox token: $stored"
        }
    }

    /**
     * Keeps the button's label matching the real state, which can change
     * without this screen doing anything -- the notification's Stop action
     * works while the Activity is in the foreground. Only assigns on a change
     * so it can be called from the 200ms refresh without churning layout.
     */
    private fun syncPowerButton() {
        val wanted = if (TbtService.isEnabled(this)) "Stop link" else "Start link"
        if (powerButton.text != wanted) powerButton.text = wanted
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresh)
    }

    override fun onPause() {
        super.onPause()
        handler.removeCallbacks(refresh)
    }

    private fun requestRuntimePermissions() {
        val needed = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            needed += Manifest.permission.BLUETOOTH_SCAN
            needed += Manifest.permission.BLUETOOTH_CONNECT
        }

        // Location on every release, not just pre-12.
        //
        // It used to be asked for only below API 31, where BLE scanning
        // required it. Mapbox navigation needs it on all of them: without a
        // fix the trip session produces no route progress, so a route plans
        // and then never yields a single turn. Both precisions are named
        // because Android 12+ offers the user "approximate" as a choice, and a
        // request for FINE alone leaves that choice granting nothing.
        needed += Manifest.permission.ACCESS_FINE_LOCATION
        needed += Manifest.permission.ACCESS_COARSE_LOCATION
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // Without this the foreground service still runs, but its
            // notification is suppressed -- so the one visible sign that the
            // link is alive would silently disappear.
            needed += Manifest.permission.POST_NOTIFICATIONS
        }
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1)
        }
    }
}
