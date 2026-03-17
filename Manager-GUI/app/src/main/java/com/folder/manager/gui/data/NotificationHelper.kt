package com.folder.manager.gui.data

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import com.folder.manager.gui.R

object NotificationHelper {

    private const val CHANNEL_ALERT = "pathguard_alert"
    private const val CHANNEL_ALERT_NAME = "敏感路径告警"
    private var notifId = 1000

    fun createChannels(context: Context) {
        val mgr = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        mgr.createNotificationChannel(
            NotificationChannel(CHANNEL_ALERT, CHANNEL_ALERT_NAME, NotificationManager.IMPORTANCE_HIGH).apply {
                description = "当受监控路径被访问时发出告警通知"
            }
        )
    }

    fun sendAlert(context: Context, pkg: String, path: String, action: String) {
        if (!NotificationManagerCompat.from(context).areNotificationsEnabled()) return
        val notif = NotificationCompat.Builder(context, CHANNEL_ALERT)
            .setSmallIcon(android.R.drawable.ic_dialog_alert)
            .setContentTitle("PathGuard 告警：$action")
            .setContentText("$pkg 访问了 $path")
            .setStyle(NotificationCompat.BigTextStyle().bigText("包名：$pkg\n路径：$path\n动作：$action"))
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setAutoCancel(true)
            .build()
        try {
            NotificationManagerCompat.from(context).notify(notifId++, notif)
        } catch (_: SecurityException) { }
    }
}
