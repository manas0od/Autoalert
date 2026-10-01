package com.example.data

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import com.example.AutoAlertApplication
import com.example.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

class NotificationActionReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        if (action != ACTION_REPLY) return

        val notifId = intent.getLongExtra(EXTRA_NOTIFICATION_ID, -1L)
        val explicitOriginalId = intent.getLongExtra(EXTRA_ORIGINAL_NOTIFICATION_ID, -1L)
        val systemNotifId = intent.getIntExtra(EXTRA_SYSTEM_NOTIF_ID, if (notifId != -1L) notifId.toInt() else 1001)
        val rawReply = intent.getStringExtra(EXTRA_REPLY_STATUS) ?: "coming"
        val normalizedReply = if (rawReply.contains("busy", ignoreCase = true)) "busy" else "coming"

        // Determine true original request ID
        val targetOriginalId = when {
            explicitOriginalId != -1L -> explicitOriginalId
            notifId != -1L -> NtfyManager.getOriginalNotificationId(notifId) ?: notifId
            else -> -1L
        }
        val pairedReminderId = if (targetOriginalId != -1L) NtfyManager.getReminderDbId(targetOriginalId) else null

        val pendingResult = goAsync()
        val appContext = context.applicationContext as AutoAlertApplication
        val repository = NotificationRepository(appContext.database.notificationDao())
        val ntfyManager = NtfyManager(appContext, repository)

        CoroutineScope(Dispatchers.IO).launch {
            try {
                val dbStatus = if (normalizedReply == "coming") "Accepted" else "Cancelled"

                // 1. Update database record for clicked notification
                if (notifId != -1L) {
                    repository.updateStatus(notifId, dbStatus)
                    repository.updateReplyStatus(notifId, normalizedReply)
                }

                // 2. Update database record for paired original/reminder/escalation request
                val pairedEscalationId = if (targetOriginalId != -1L) NtfyManager.getEscalationDbId(targetOriginalId) else null
                if (targetOriginalId != -1L && targetOriginalId != notifId) {
                    repository.updateStatus(targetOriginalId, dbStatus)
                    repository.updateReplyStatus(targetOriginalId, normalizedReply)
                }
                if (pairedReminderId != null && pairedReminderId != notifId) {
                    repository.updateStatus(pairedReminderId, dbStatus)
                    repository.updateReplyStatus(pairedReminderId, normalizedReply)
                }
                if (pairedEscalationId != null && pairedEscalationId != notifId) {
                    repository.updateStatus(pairedEscalationId, dbStatus)
                    repository.updateReplyStatus(pairedEscalationId, normalizedReply)
                }

                // 3. Cancel any pending reminder/escalation job and dismiss any already-shown reminder/escalation notification
                if (targetOriginalId != -1L) {
                    NtfyManager.resolveAndDismissReminder(targetOriginalId, context)
                } else if (notifId != -1L) {
                    NtfyManager.resolveAndDismissReminder(notifId, context)
                }

                // 4. Send ntfy reply POST: 'status:coming' or 'status:busy' to both primary and backup topics
                val topic = appContext.preferences.ntfyTopic.first()
                val backupTopic = appContext.preferences.backupNtfyTopic.first()
                if (topic.isNotBlank()) {
                    ntfyManager.sendReply(topic, normalizedReply)
                }
                if (backupTopic.isNotBlank() && backupTopic != topic) {
                    ntfyManager.sendReply(backupTopic, normalizedReply)
                }

                // 5. Update the notification in the tray to show confirmation
                val replyText = if (normalizedReply == "coming") "Coming" else "Busy"
                val largeIconBitmap = try {
                    BitmapFactory.decodeResource(context.resources, R.drawable.img_app_icon)
                } catch (e: Exception) {
                    null
                }

                val confirmationBuilder = NotificationCompat.Builder(context, AutoAlertApplication.CHANNEL_SERVICE_CALLS)
                    .setSmallIcon(R.drawable.ic_notification_auto)
                    .setContentTitle("AutoAlert: Reply Sent")
                    .setContentText("You replied: $replyText")
                    .setStyle(
                        NotificationCompat.BigTextStyle()
                            .bigText("You replied: $replyText\nResponse sent to Auto Stand.")
                    )
                    .setPriority(NotificationCompat.PRIORITY_LOW)
                    .setColor(0xFFFFB300.toInt())
                    .setAutoCancel(true)
                    .setTimeoutAfter(7000L)

                if (largeIconBitmap != null) {
                    confirmationBuilder.setLargeIcon(largeIconBitmap)
                }

                val notificationManager = NotificationManagerCompat.from(context)
                if (notificationManager.areNotificationsEnabled()) {
                    notificationManager.notify(systemNotifId, confirmationBuilder.build())
                }
            } catch (e: Exception) {
                e.printStackTrace()
            } finally {
                pendingResult.finish()
            }
        }
    }

    companion object {
        const val ACTION_REPLY = "com.example.autoalert.ACTION_REPLY"
        const val EXTRA_NOTIFICATION_ID = "extra_notification_id"
        const val EXTRA_ORIGINAL_NOTIFICATION_ID = "extra_original_notification_id"
        const val EXTRA_SYSTEM_NOTIF_ID = "extra_system_notif_id"
        const val EXTRA_REPLY_STATUS = "extra_reply_status"
    }
}
