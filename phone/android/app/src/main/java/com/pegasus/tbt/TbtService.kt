package com.pegasus.tbt

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat

/**
 * Owns the BLE link for the lifetime of the process.
 *
 * It lives in a service rather than in MainActivity because of how this app is
 * actually used: the phone goes in a pocket, the Activity is destroyed, and
 * turn prompts have to keep flowing. The first version held the link in the
 * Activity and published it through a static field, which both leaked the
 * Activity and left the notification listener writing into an object whose
 * owner had been torn down -- prompts would stop partway through a ride with
 * nothing on screen to explain why.
 *
 * Foreground, with a persistent notification, because that is the only way to
 * ask Android not to reclaim it. The notification is the honest price of the
 * feature: a background app has no right to hold a BLE connection for an hour.
 */
class TbtService : Service() {

    companion object {
        private const val CHANNEL_ID = "pegasus_tbt_link"
        private const val NOTIFICATION_ID = 1

        /** The one link in the process. Null until the service is created. */
        @Volatile
        var link: BleLink? = null
            private set

        /** Latest connection status, for MainActivity to display. */
        @Volatile
        var status: String = "Not started"
            private set

        fun start(context: Context) {
            val intent = Intent(context, TbtService::class.java)
            // startForegroundService requires startForeground() within ~5s;
            // onStartCommand does it immediately below.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        createChannel()

        // applicationContext, never an Activity: this object outlives every
        // screen in the app.
        link = BleLink(applicationContext).also { ble ->
            ble.onStatus = { message ->
                status = message
                notifyStatus(message)
            }
            ble.start()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = buildNotification(status)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            // API 34 requires declaring why a foreground service exists.
            // connectedDevice is the honest description: it holds a GATT link.
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        // Restart if Android reclaims us under memory pressure: a dropped link
        // mid-ride is the failure this service exists to prevent.
        return START_STICKY
    }

    override fun onDestroy() {
        link?.stop()
        link = null
        status = "Stopped"
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Head unit link",
            // LOW: it must be visible, but it should never make a sound or
            // interrupt while riding.
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Keeps the connection to the Pegasus head unit alive"
            setShowBadge(false)
        }
        getSystemService(NotificationManager::class.java)?.createNotificationChannel(channel)
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Pegasus TBT")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setContentIntent(open)
            .build()
    }

    private fun notifyStatus(text: String) {
        // Keeps the shade in step with the link, so the state is visible
        // without unlocking and opening the app.
        getSystemService(NotificationManager::class.java)
            ?.notify(NOTIFICATION_ID, buildNotification(text))
    }
}
