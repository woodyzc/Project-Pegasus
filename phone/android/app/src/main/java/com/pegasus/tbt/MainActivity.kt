package com.pegasus.tbt

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.widget.Button
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
            parseStatus.text =
                "$cadence\n${MapsNotificationListener.lastParse}$redactedNote$failureBlock"
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
                        status.text = if (done) "Route sent" else "Sending route… \$percent%"
                    }
                }
                link.sendRoute(TestRoute.square().encode())
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
            addView(powerButton)
        })

        requestRuntimePermissions()
        // Respect a previous stop rather than overriding it just because the
        // user opened the screen to look at the counters.
        TbtService.startIfEnabled(this)
        syncPowerButton()
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
        } else {
            // Pre-12, BLE scanning is gated behind location permission.
            needed += Manifest.permission.ACCESS_FINE_LOCATION
        }
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
