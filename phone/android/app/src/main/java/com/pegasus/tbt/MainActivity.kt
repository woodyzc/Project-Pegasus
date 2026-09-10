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
 * A deliberately plain control panel: grant the two permissions, see whether
 * the link is up, and send a test frame without needing Maps at all.
 *
 * The test button matters more than it looks -- it separates "the BLE link is
 * broken" from "the notification parsing is broken", which are the two ways
 * this app fails and are otherwise hard to tell apart on a bike.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var status: TextView
    private lateinit var parseStatus: TextView
    private val handler = Handler(Looper.getMainLooper())
    private var ble: BleLink? = null

    private val refresh = object : Runnable {
        override fun run() {
            parseStatus.text = "Last notification: ${MapsNotificationListener.lastParse}"
            handler.postDelayed(this, 1000)
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
                val sent = ble?.send(
                    TbtFrame.encode(ManeuverParser.Icon.TURN_RIGHT, 250, "Test Street"),
                    force = true
                ) ?: false
                status.text = if (sent) "Test frame sent" else "Not connected"
            }
        }

        val clearButton = Button(this).apply {
            text = "Clear route"
            setOnClickListener { ble?.send(TbtFrame.clearFrame(), force = true) }
        }

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
            addView(status)
            addView(parseStatus)
            addView(notifButton)
            addView(testButton)
            addView(clearButton)
        })

        requestBluetoothPermissions()

        ble = BleLink(this).also {
            it.onStatus = { message -> status.text = message }
            MapsNotificationListener.ble = it
            it.start()
        }
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresh)
    }

    override fun onPause() {
        super.onPause()
        handler.removeCallbacks(refresh)
    }

    private fun requestBluetoothPermissions() {
        val needed = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            needed += Manifest.permission.BLUETOOTH_SCAN
            needed += Manifest.permission.BLUETOOTH_CONNECT
        } else {
            // Pre-12, BLE scanning is gated behind location permission.
            needed += Manifest.permission.ACCESS_FINE_LOCATION
        }
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1)
        }
    }
}
