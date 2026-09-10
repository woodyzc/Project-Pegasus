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

        /** Sent by the notification's Stop action and by the in-app button. */
        const val ACTION_STOP = "com.pegasus.tbt.STOP"

        private const val PREFS = "pegasus_tbt"
        private const val KEY_ENABLED = "link_enabled"

        /**
         * Whether the user wants the link running, remembered across process
         * deaths.
         *
         * This exists because the service was genuinely unkillable, which was
         * not the intent. Three separate mechanisms brought it back:
         * START_STICKY, an ongoing notification that cannot be swiped, and --
         * the one that defeated even Force stop -- onListenerConnected() in
         * MapsNotificationListener. Notification access binds that listener
         * for as long as the permission is granted, so Android rebinds it
         * after a force stop and it started the service again.
         *
         * A flag the user sets is the only thing that can outlast all three,
         * because every one of those paths now reads it before starting.
         */
        fun isEnabled(context: Context): Boolean =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getBoolean(KEY_ENABLED, true)

        private fun setEnabled(context: Context, value: Boolean) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit()
                .putBoolean(KEY_ENABLED, value)
                .apply()
        }

        /** Starts the link and remembers that it should be running. */
        fun startByUser(context: Context) {
            setEnabled(context, true)
            start(context)
        }

        /**
         * Starts only if the user has not turned the link off. Every automatic
         * caller uses this; only an explicit user action uses startByUser.
         */
        fun startIfEnabled(context: Context) {
            if (isEnabled(context)) start(context)
        }

        /** Stops the link and remembers that it should stay stopped. */
        fun stopByUser(context: Context) {
            setEnabled(context, false)
            context.startService(Intent(context, TbtService::class.java).setAction(ACTION_STOP))
        }

        /** The one link in the process. Null until the service is created. */
        @Volatile
        var link: BleLink? = null
            private set

        /** Latest connection status, for MainActivity to display. */
        @Volatile
        var status: String = "Not started"
            private set

        // Private on purpose: an unguarded start is what made the service
        // unkillable. Callers go through startByUser or startIfEnabled so the
        // user's choice is always consulted.
        private fun start(context: Context) {
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
    }

    /**
     * Creates the BLE link on first use rather than in onCreate.
     *
     * onCreate runs even when this instance is about to shut down again -- a
     * STOP intent delivered to a dead service, or a START_STICKY restart the
     * user has since disabled -- and starting a BLE scan in those cases spun
     * the radio up for something that immediately stopped.
     */
    private fun ensureLink() {
        if (link != null) return
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

        // Foreground first, unconditionally. startForegroundService promises
        // Android a startForeground() within ~5 seconds, and it holds even for
        // the calls that are about to stop: skipping it to shut down faster
        // earns an ANR-style crash instead.
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

        // A user-requested stop, or a restart the user has since turned off.
        // Checking the flag here is what makes Force stop stick: Android will
        // rebind the notification listener regardless, but nothing starts the
        // link again while this is false.
        if (intent?.action == ACTION_STOP || !isEnabled(this)) {
            status = "Stopped by user"
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }

        ensureLink()

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

        // The notification is ongoing and so cannot be swiped away. Without an
        // explicit action there was no way to stop the link from the shade at
        // all -- the only apparent option was Force stop, which the listener
        // then undid. This is that missing way out.
        val stop = PendingIntent.getService(
            this,
            1,
            Intent(this, TbtService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Pegasus TBT")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setContentIntent(open)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop", stop)
            .build()
    }

    private fun notifyStatus(text: String) {
        // Keeps the shade in step with the link, so the state is visible
        // without unlocking and opening the app.
        getSystemService(NotificationManager::class.java)
            ?.notify(NOTIFICATION_ID, buildNotification(text))
    }
}
