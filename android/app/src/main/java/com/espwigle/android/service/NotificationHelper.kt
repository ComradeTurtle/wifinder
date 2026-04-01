package com.espwigle.android.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.app.NotificationCompat
import com.espwigle.android.MainActivity

object NotificationHelper {
  const val CHANNEL_ID = "espwigle_scanner"
  const val NOTIFICATION_ID = 1

  fun createChannel(context: Context) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      val channel = NotificationChannel(
        CHANNEL_ID,
        "Scanner Service",
        NotificationManager.IMPORTANCE_LOW,
      ).apply {
        description = "Keeps BLE scanning and GPS active in the background"
        setShowBadge(false)
      }
      val manager = context.getSystemService(NotificationManager::class.java)
      manager.createNotificationChannel(channel)
    }
  }

  fun build(
    context: Context,
    connected: Boolean,
    scanning: Boolean,
    apCount: Int,
    csvActive: Boolean,
  ): Notification {
    val openIntent = Intent(context, MainActivity::class.java).apply {
      flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
    }
    val openPending = PendingIntent.getActivity(
      context, 0, openIntent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    val stopIntent = Intent(context, ScannerService::class.java).apply {
      action = ScannerService.ACTION_STOP
    }
    val stopPending = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      PendingIntent.getForegroundService(
        context, 1, stopIntent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
      )
    } else {
      PendingIntent.getService(
        context, 1, stopIntent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
      )
    }

    val status = buildString {
      append(if (connected) "BLE ✓" else "BLE ✗")
      if (scanning) append(" · Scanning")
      append(" · $apCount APs")
      if (csvActive) append(" · CSV ●")
    }

    return NotificationCompat.Builder(context, CHANNEL_ID)
      .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
      .setContentTitle("ESPWIGLE")
      .setContentText(status)
      .setOngoing(true)
      .setSilent(true)
      .setContentIntent(openPending)
      .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop", stopPending)
      .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
      .build()
  }
}
