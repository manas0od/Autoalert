package com.example.data

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.media.RingtoneManager
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import com.example.AutoAlertApplication
import com.example.MainActivity
import com.example.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import java.io.BufferedReader
import java.util.Collections
import java.util.LinkedHashSet
import java.util.concurrent.TimeUnit

data class NtfyMessage(
    val id: String,
    val time: Long,
    val event: String,
    val topic: String,
    val title: String,
    val message: String,
    val tags: List<String> = emptyList()
)

fun formatBatteryAge(timestampMs: Long?): String {
    if (timestampMs == null || timestampMs <= 0) return "from last alert"
    val diffSec = (System.currentTimeMillis() - timestampMs) / 1000
    val ageText = when {
        diffSec < 60 -> "just now"
        diffSec < 120 -> "1 min ago"
        diffSec < 3600 -> "${diffSec / 60} min ago"
        diffSec < 7200 -> "1 hour ago"
        diffSec < 86400 -> "${diffSec / 3600} hours ago"
        diffSec < 172800 -> "1 day ago"
        else -> "${diffSec / 86400} days ago"
    }
    return "as of $ageText, from last alert"
}

class NtfyManager(
    private val context: Context,
    private val repository: NotificationRepository
) {

    private val preferences by lazy { (context.applicationContext as? AutoAlertApplication)?.preferences ?: AutoAlertPreferences(context) }

    companion object {
        fun extractBatteryPercentFromTags(tags: List<String>): Int? {
            val regex = Regex("""^batt(\d+)$""", RegexOption.IGNORE_CASE)
            for (tag in tags) {
                val match = regex.find(tag.trim())
                if (match != null) {
                    val percent = match.groupValues[1].toIntOrNull()
                    if (percent != null && percent in 0..100) {
                        return percent
                    }
                }
            }
            return null
        }
        // Exactly 15 seconds single source of truth for no-response reminder delay
        const val NO_RESPONSE_REMINDER_DELAY_MS = 15_000L

        // Thread-safe LRU set of processed ntfy message IDs to deduplicate across listeners and services
        private val processedMessageIds = Collections.synchronizedSet(object : LinkedHashSet<String>() {
            override fun add(element: String): Boolean {
                val added = super.add(element)
                while (size > 200) {
                    val it = iterator()
                    if (it.hasNext()) {
                        it.next()
                        it.remove()
                    }
                }
                return added
            }
        })

        // Track original request DB IDs that have already scheduled or spawned a reminder (at most ONE reminder per request)
        private val remindedOriginalIds = Collections.synchronizedSet(HashSet<Long>())

        // Active scheduled reminder coroutine jobs: originalNotificationId -> Job
        private val pendingReminderJobs = java.util.concurrent.ConcurrentHashMap<Long, Job>()

        // System notification IDs for active reminders: originalNotificationId -> reminderSystemNotificationId
        private val activeReminderNotifIds = java.util.concurrent.ConcurrentHashMap<Long, Int>()

        // System notification IDs for active original notifications: originalNotificationId -> originalSystemNotificationId
        private val activeOriginalNotifIds = java.util.concurrent.ConcurrentHashMap<Long, Int>()

        // System notification IDs for active backup escalation notifications: originalNotificationId -> escalationSystemNotificationId
        private val activeEscalationNotifIds = java.util.concurrent.ConcurrentHashMap<Long, Int>()

        // Bidirectional mapping between original DB ID and reminder DB ID
        private val originalToReminderDbId = java.util.concurrent.ConcurrentHashMap<Long, Long>()
        private val reminderToOriginalDbId = java.util.concurrent.ConcurrentHashMap<Long, Long>()

        // Bidirectional mapping between original DB ID and escalation DB ID
        private val originalToEscalationDbId = java.util.concurrent.ConcurrentHashMap<Long, Long>()
        private val escalationToOriginalDbId = java.util.concurrent.ConcurrentHashMap<Long, Long>()

        fun getOriginalNotificationId(id: Long): Long? =
            reminderToOriginalDbId[id] ?: escalationToOriginalDbId[id]
        fun getReminderDbId(originalId: Long): Long? = originalToReminderDbId[originalId]
        fun getEscalationDbId(originalId: Long): Long? = originalToEscalationDbId[originalId]

        fun markMessageProcessed(msgId: String): Boolean {
            if (msgId.isBlank()) return true
            return processedMessageIds.add(msgId)
        }

        fun isMessageAlreadyProcessed(msgId: String): Boolean {
            if (msgId.isBlank()) return false
            return processedMessageIds.contains(msgId)
        }

        fun resolveAndDismissReminder(originalOrReminderId: Long, context: Context?) {
            // Determine true original ID
            val originalId = reminderToOriginalDbId[originalOrReminderId]
                ?: escalationToOriginalDbId[originalOrReminderId]
                ?: originalOrReminderId
            val reminderDbId = originalToReminderDbId[originalId]
            val escalationDbId = originalToEscalationDbId[originalId]

            // 1. Cancel any pending reminder or escalation timer job for this request
            pendingReminderJobs.remove(originalId)?.cancel()

            // 2. Dismiss any active reminder notification from the system tray
            val reminderSystemId = activeReminderNotifIds.remove(originalId)
            if (reminderSystemId != null && context != null) {
                dismissNotification(context, reminderSystemId)
            }

            // 3. Dismiss any active escalation notification from the system tray
            val escalationSystemId = activeEscalationNotifIds.remove(originalId)
            if (escalationSystemId != null && context != null) {
                dismissNotification(context, escalationSystemId)
            }

            // 4. Dismiss original notification if needed
            val originalSystemId = activeOriginalNotifIds.remove(originalId)
            if (originalSystemId != null && context != null) {
                dismissNotification(context, originalSystemId)
            }

            // 5. Clean up ID mappings
            if (reminderDbId != null) {
                reminderToOriginalDbId.remove(reminderDbId)
            }
            if (escalationDbId != null) {
                escalationToOriginalDbId.remove(escalationDbId)
            }
            originalToReminderDbId.remove(originalId)
            originalToEscalationDbId.remove(originalId)
        }

        fun dismissAllNotifications(context: Context) {
            try {
                pendingReminderJobs.values.forEach { it.cancel() }
                pendingReminderJobs.clear()
                activeReminderNotifIds.clear()
                activeEscalationNotifIds.clear()
                activeOriginalNotifIds.clear()
                NotificationManagerCompat.from(context).cancelAll()
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }

        fun dismissNotification(context: Context, notificationId: Int) {
            try {
                NotificationManagerCompat.from(context).cancel(notificationId)
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }
    }

    private val client = OkHttpClient.Builder()
        .readTimeout(0, TimeUnit.SECONDS) // Infinite read timeout for long SSE stream
        .connectTimeout(12, TimeUnit.SECONDS)
        .retryOnConnectionFailure(true)
        .pingInterval(15, TimeUnit.SECONDS) // Ping every 15s to quickly detect dropped TCP connections / network handover
        .build()

    private val postClient = OkHttpClient.Builder()
        .connectTimeout(8, TimeUnit.SECONDS)
        .readTimeout(8, TimeUnit.SECONDS)
        .writeTimeout(8, TimeUnit.SECONDS)
        .retryOnConnectionFailure(true)
        .build()

    private val _messageFlow = MutableSharedFlow<NtfyMessage>()
    val messageFlow = _messageFlow.asSharedFlow()

    private var subscribeJob: Job? = null
    private val serviceScope = CoroutineScope(Dispatchers.IO)
    private var currentTopic: String = ""
    private var currentBackupTopic: String = ""
    private var currentTiltTopic: String = ""

    private var activeCall: okhttp3.Call? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    init {
        registerNetworkMonitoring()
    }

    private fun registerNetworkMonitoring() {
        try {
            val connectivityManager = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
            if (connectivityManager != null && networkCallback == null) {
                val callback = object : ConnectivityManager.NetworkCallback() {
                    override fun onAvailable(network: Network) {
                        Log.d("NtfyManager", "Network available. Releasing any local process network binding.")
                        DeviceDiscoveryManager.releaseNetworkBinding(context)
                        // Trigger immediate reconnect if listening
                        if ((currentTopic.isNotBlank() || currentTiltTopic.isNotBlank()) && subscribeJob?.isActive == true) {
                            reconnectStream()
                        }
                    }

                    override fun onLost(network: Network) {
                        Log.d("NtfyManager", "Network lost. Clearing socket pool & releasing binding.")
                        DeviceDiscoveryManager.releaseNetworkBinding(context)
                        try {
                            client.connectionPool.evictAll()
                        } catch (e: Exception) {
                            // Ignored
                        }
                        // Interrupt active call so it reconnects over the next available network interface (e.g. mobile data)
                        activeCall?.cancel()
                    }

                    override fun onCapabilitiesChanged(network: Network, networkCapabilities: NetworkCapabilities) {
                        val hasInternet = networkCapabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) &&
                                networkCapabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
                        if (hasInternet) {
                            DeviceDiscoveryManager.releaseNetworkBinding(context)
                        }
                    }
                }
                networkCallback = callback
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                    connectivityManager.registerDefaultNetworkCallback(callback)
                } else {
                    val request = NetworkRequest.Builder()
                        .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                        .build()
                    connectivityManager.registerNetworkCallback(request, callback)
                }
            }
        } catch (e: Exception) {
            Log.e("NtfyManager", "Error registering network callback: ${e.message}")
        }
    }

    private fun reconnectStream() {
        if (currentTopic.isBlank() && currentTiltTopic.isBlank()) return
        serviceScope.launch {
            try {
                activeCall?.cancel()
            } catch (e: Exception) {
                // Ignore
            }
        }
    }

    fun startListening(topic: String, backupTopic: String = "", tiltTopic: String = "") {
        if (topic.isBlank() && tiltTopic.isBlank()) return
        currentTopic = topic.trim()
        currentBackupTopic = if (backupTopic.isNotBlank()) backupTopic.trim() else if (currentTopic.isNotBlank()) "${currentTopic}-backup" else ""
        currentTiltTopic = tiltTopic.trim()
        stopListening(clearTopic = false)

        // Ensure process is unbound from any local Wi-Fi pairing network
        DeviceDiscoveryManager.releaseNetworkBinding(context)

        subscribeJob = serviceScope.launch {
            val distinctTopics = listOf(currentTopic, currentBackupTopic, currentTiltTopic)
                .filter { it.isNotBlank() }
                .distinct()
            val streamTopic = distinctTopics.joinToString(",")
            if (streamTopic.isNotBlank()) {
                listenToNtfyTopicStream(streamTopic)
            }
        }
    }

    fun stopListening(clearTopic: Boolean = true) {
        if (clearTopic) {
            currentTopic = ""
            currentBackupTopic = ""
            currentTiltTopic = ""
        }
        try {
            activeCall?.cancel()
        } catch (e: Exception) {
            // Ignore
        }
        subscribeJob?.cancel()
        subscribeJob = null
    }

    fun cleanup() {
        stopListening(clearTopic = true)
        try {
            val connectivityManager = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
            networkCallback?.let {
                connectivityManager?.unregisterNetworkCallback(it)
                networkCallback = null
            }
        } catch (e: Exception) {
            // Ignore
        }
    }

    private suspend fun listenToNtfyTopicStream(topic: String) {
        val url = "https://ntfy.sh/$topic/json"
        var backoffMs = 1500L

        while (true) {
            // Ensure no stale network binding is applied to the process
            DeviceDiscoveryManager.releaseNetworkBinding(context)

            try {
                val request = Request.Builder()
                    .url(url)
                    .build()

                val call = client.newCall(request)
                activeCall = call

                call.execute().use { response ->
                    val body = response.body

                    if (response.isSuccessful && body != null) {
                        backoffMs = 1500L // Reset backoff on successful stream connect
                        val reader = BufferedReader(body.charStream())
                        var line: String?

                        while (reader.readLine().also { line = it } != null) {
                            val trimmed = line?.trim() ?: continue
                            if (trimmed.isEmpty() || !trimmed.startsWith("{")) continue

                            try {
                                val json = JSONObject(trimmed)
                                val event = json.optString("event", "")
                                if (event == "message") {
                                    val id = json.optString("id", System.currentTimeMillis().toString())
                                    val time = json.optLong("time", System.currentTimeMillis() / 1000) * 1000
                                    val title = json.optString("title", "Auto-Rickshaw Call")
                                    val messageStr = json.optString("message", "Service requested!")
                                    val msgTopic = json.optString("topic").ifBlank { currentTopic.ifBlank { currentTiltTopic } }

                                    val tagsList = mutableListOf<String>()
                                    val tagsArray = json.optJSONArray("tags")
                                    if (tagsArray != null) {
                                        for (i in 0 until tagsArray.length()) {
                                            val t = tagsArray.optString(i, "")
                                            if (t.isNotBlank()) tagsList.add(t)
                                        }
                                    } else {
                                        val tagsStr = json.optString("tags", "")
                                        if (tagsStr.isNotBlank()) {
                                            tagsList.addAll(tagsStr.split(",").map { it.trim() })
                                        }
                                    }

                                    val msg = NtfyMessage(
                                        id = id,
                                        time = time,
                                        event = event,
                                        topic = msgTopic,
                                        title = title,
                                        message = messageStr,
                                        tags = tagsList
                                    )

                                    if (handleIncomingMessage(msg)) {
                                        _messageFlow.emit(msg)
                                    }
                                }
                            } catch (e: Exception) {
                                e.printStackTrace()
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                // If cancelled deliberately (e.g. on network switch), log info
                if (e is java.io.InterruptedIOException || e is java.net.SocketException) {
                    Log.d("NtfyManager", "Stream reconnecting/interrupted: ${e.message}")
                } else {
                    e.printStackTrace()
                }
            } finally {
                activeCall = null
            }

            // Retry stream connection after backoff
            delay(backoffMs)
            backoffMs = (backoffMs * 1.5).toLong().coerceAtMost(20000L)
        }
    }

    suspend fun handleIncomingMessage(msg: NtfyMessage): Boolean {
        // Extract battery tag (e.g. "batt64") and cache locally on every incoming message with the tag
        val parsedBattery = extractBatteryPercentFromTags(msg.tags)
        if (parsedBattery != null) {
            val now = if (msg.time > 0) msg.time else System.currentTimeMillis()
            try {
                preferences.updateLastKnownBattery(parsedBattery, now)
            } catch (e: Exception) {
                // Ignore
            }
        }

        if (msg.id.isNotBlank() && isMessageAlreadyProcessed(msg.id)) {
            return false
        }
        markMessageProcessed(msg.id)

        val isLowBattery = msg.title.lowercase().contains("battery") ||
                msg.message.lowercase().contains("battery") ||
                msg.message.lowercase().contains("voltage")

        val isReminder = msg.title.lowercase().contains("reminder") ||
                msg.message.lowercase().contains("reminder")

        val isTilt = msg.topic.endsWith("-tilt") ||
                msg.topic.contains("tilt") ||
                (currentTiltTopic.isNotBlank() && msg.topic.equals(currentTiltTopic, ignoreCase = true)) ||
                msg.title.contains("Tilt", ignoreCase = true) ||
                msg.title.contains("Rollover", ignoreCase = true) ||
                msg.title.contains("Safety", ignoreCase = true) ||
                msg.message.contains("Tilt", ignoreCase = true) ||
                msg.message.contains("Rollover", ignoreCase = true)

        val isEscalation = msg.topic.endsWith("-backup") ||
                msg.title.contains("Backup Alert", ignoreCase = true) ||
                msg.message.contains("Primary driver unavailable", ignoreCase = true)

        val status = if (isTilt) "Safety Alert" else if (isLowBattery) "Low Battery" else "Pending"
        val locationLabel = "Auto Stand Module"

        val entityTitle = when {
            isTilt -> if (msg.title.isNotBlank()) msg.title else "🚨 SAFETY ALERT: Vehicle Tilt / Rollover"
            isLowBattery -> "Low Battery Warning"
            isEscalation -> "🚨 Backup Alert: Primary driver unavailable"
            isReminder -> "🚨 Reminder: Call Unacknowledged"
            msg.title.isNotBlank() -> msg.title
            else -> "Service Requested"
        }

        val entity = NotificationEntity(
            title = entityTitle,
            message = msg.message,
            timestamp = if (msg.time > 0) msg.time else System.currentTimeMillis(),
            status = status,
            locationLabel = locationLabel,
            requestId = (if (isTilt) "TILT-" else if (isEscalation) "ESC-" else if (isReminder) "REM-" else "REQ-") + (1000..9999).random()
        )

        val insertedId = repository.insertNotification(entity)

        // Derive system notification ID directly from ntfy's unique message ID
        val uniqueSystemId = if (msg.id.isNotBlank()) {
            (msg.id.hashCode() and 0x7FFFFFFF).let { if (it == 0) 1001 else it }
        } else {
            insertedId.toInt()
        }

        if (isEscalation) {
            activeEscalationNotifIds[insertedId] = uniqueSystemId
        } else {
            activeOriginalNotifIds[insertedId] = uniqueSystemId
        }

        showSystemNotification(
            id = uniqueSystemId,
            title = entity.title,
            message = entity.message,
            isLowBattery = isLowBattery,
            dbNotificationId = insertedId,
            originalNotificationId = insertedId
        )

        // Schedule no-response reminder ONLY for genuine primary service requests (NEVER for reminders, low battery, tilt, or escalations)
        if (!isLowBattery && !isReminder && !isEscalation && !isTilt && status == "Pending") {
            scheduleNoResponseReminder(insertedId, msg.id, entity.message, locationLabel)
        }

        return true
    }

    private fun scheduleNoResponseReminder(
        notificationId: Long,
        originalMsgId: String,
        originalMsg: String,
        locationLabel: String
    ) {
        // Enforce: At most ONE reminder per original service request
        if (!remindedOriginalIds.add(notificationId)) {
            return
        }

        // Cancel any prior scheduled job for this request
        pendingReminderJobs.remove(notificationId)?.cancel()

        val job = serviceScope.launch {
            // First tier: Exactly 15 seconds delay for primary driver reminder
            delay(NO_RESPONSE_REMINDER_DELAY_MS)

            // Check original request's current status in the database before firing reminder
            val originalEntity = repository.getNotificationById(notificationId)
            if (originalEntity == null || !originalEntity.status.equals("Pending", ignoreCase = true) || originalEntity.replyStatus != null) {
                pendingReminderJobs.remove(notificationId)
                return@launch
            }

            // Create and persist the reminder record
            val reminderEntity = NotificationEntity(
                title = "🚨 Reminder: Call Unacknowledged",
                message = "Unacknowledged service call: $originalMsg. Passenger still awaiting ride!",
                timestamp = System.currentTimeMillis(),
                status = "Pending",
                locationLabel = locationLabel,
                requestId = "REM-" + (1000..9999).random()
            )

            val reminderId = repository.insertNotification(reminderEntity)
            originalToReminderDbId[notificationId] = reminderId
            reminderToOriginalDbId[reminderId] = notificationId

            val reminderSystemId = ((originalMsgId + "_rem").hashCode() and 0x7FFFFFFF).let {
                if (it == 0) ((reminderId.toInt() + 9999) and 0x7FFFFFFF) else it
            }
            activeReminderNotifIds[notificationId] = reminderSystemId

            // Show reminder system notification linking back to originalNotificationId
            showSystemNotification(
                id = reminderSystemId,
                title = "🚨 Reminder: Call Unacknowledged",
                message = "Service call at Auto Stand is waiting for driver response!",
                isLowBattery = false,
                dbNotificationId = reminderId,
                originalNotificationId = notificationId
            )

            // Second tier: Wait another reminder window (15 seconds) for escalation to backup driver
            delay(NO_RESPONSE_REMINDER_DELAY_MS)

            // Re-check database if primary driver answered or if request resolved
            val checkAfterReminder = repository.getNotificationById(notificationId)
            if (checkAfterReminder == null || !checkAfterReminder.status.equals("Pending", ignoreCase = true) || checkAfterReminder.replyStatus != null) {
                pendingReminderJobs.remove(notificationId)
                return@launch
            }

            // Unanswered a second time! Trigger backup driver escalation alert
            val targetBackupTopic = if (currentBackupTopic.isNotBlank()) currentBackupTopic else if (currentTopic.isNotBlank()) "${currentTopic}-backup" else ""
            if (targetBackupTopic.isNotBlank()) {
                publishEscalationAlert(
                    backupTopic = targetBackupTopic,
                    originalDbId = notificationId,
                    originalMsg = originalMsg,
                    locationLabel = locationLabel
                )
            }

            pendingReminderJobs.remove(notificationId)
        }

        pendingReminderJobs[notificationId] = job
    }

    suspend fun publishEscalationAlert(
        backupTopic: String,
        originalDbId: Long,
        originalMsg: String,
        locationLabel: String
    ) = withContext(Dispatchers.IO) {
        if (backupTopic.isBlank()) return@withContext

        DeviceDiscoveryManager.releaseNetworkBinding(context)

        val escalationTitle = "🚨 Backup Alert: Primary driver unavailable"
        val escalationMsg = "Escalated Request from $locationLabel: Passenger awaiting ride! Primary driver did not respond in time."

        // 1. Publish to ntfy backup topic
        val url = "https://ntfy.sh/$backupTopic"
        try {
            val request = Request.Builder()
                .url(url)
                .addHeader("Title", escalationTitle)
                .addHeader("Priority", "urgent")
                .addHeader("Tags", "warning,auto_rickshaw,escalation")
                .post(escalationMsg.toRequestBody("text/plain".toMediaType()))
                .build()

            postClient.newCall(request).execute().close()
        } catch (e: Exception) {
            e.printStackTrace()
        }

        // 2. Also record in local repository and display notification if not already shown
        try {
            val escalationEntity = NotificationEntity(
                title = escalationTitle,
                message = escalationMsg,
                timestamp = System.currentTimeMillis(),
                status = "Pending",
                locationLabel = locationLabel,
                requestId = "ESC-" + (1000..9999).random()
            )
            val escDbId = repository.insertNotification(escalationEntity)
            originalToEscalationDbId[originalDbId] = escDbId
            escalationToOriginalDbId[escDbId] = originalDbId

            val escSystemId = ((originalDbId.toString() + "_esc").hashCode() and 0x7FFFFFFF).let {
                if (it == 0) ((escDbId.toInt() + 19999) and 0x7FFFFFFF) else it
            }
            activeEscalationNotifIds[originalDbId] = escSystemId

            showSystemNotification(
                id = escSystemId,
                title = escalationTitle,
                message = escalationMsg,
                isLowBattery = false,
                dbNotificationId = escDbId,
                originalNotificationId = originalDbId
            )
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun showSystemNotification(
        id: Int,
        title: String,
        message: String,
        isLowBattery: Boolean,
        dbNotificationId: Long = id.toLong(),
        originalNotificationId: Long = dbNotificationId
    ) {
        val intent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra("NOTIFICATION_ID", dbNotificationId)
            putExtra("ORIGINAL_NOTIFICATION_ID", originalNotificationId)
        }

        val pendingIntent = PendingIntent.getActivity(
            context,
            id,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val channelId = if (isLowBattery) {
            AutoAlertApplication.CHANNEL_LOW_BATTERY
        } else {
            AutoAlertApplication.CHANNEL_SERVICE_CALLS
        }

        val defaultSound = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)

        val largeIconBitmap = try {
            BitmapFactory.decodeResource(context.resources, R.drawable.img_app_icon)
        } catch (e: Exception) {
            null
        }

        val builder = NotificationCompat.Builder(context, channelId)
            .setSmallIcon(R.drawable.ic_notification_auto)
            .setContentTitle(title)
            .setContentText(message)
            .setStyle(NotificationCompat.BigTextStyle().bigText(message))
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setCategory(if (isLowBattery) NotificationCompat.CATEGORY_ALARM else NotificationCompat.CATEGORY_CALL)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .setDefaults(NotificationCompat.DEFAULT_ALL)
            .setSound(defaultSound)
            .setVibrate(longArrayOf(0, 500, 250, 500))
            .setOnlyAlertOnce(false)
            .setColor(0xFFFFB300.toInt())
            .setAutoCancel(true)
            .setContentIntent(pendingIntent)

        if (largeIconBitmap != null) {
            builder.setLargeIcon(largeIconBitmap)
        }

        if (!isLowBattery) {
            // Add quick action buttons for Driver direct reply from notification shade / lockscreen
            val acceptIntent = Intent(context, NotificationActionReceiver::class.java).apply {
                action = NotificationActionReceiver.ACTION_REPLY
                putExtra(NotificationActionReceiver.EXTRA_NOTIFICATION_ID, dbNotificationId)
                putExtra(NotificationActionReceiver.EXTRA_ORIGINAL_NOTIFICATION_ID, originalNotificationId)
                putExtra(NotificationActionReceiver.EXTRA_SYSTEM_NOTIF_ID, id)
                putExtra(NotificationActionReceiver.EXTRA_REPLY_STATUS, "coming")
            }
            val acceptPendingIntent = PendingIntent.getBroadcast(
                context,
                id * 10 + 1,
                acceptIntent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )

            val busyIntent = Intent(context, NotificationActionReceiver::class.java).apply {
                action = NotificationActionReceiver.ACTION_REPLY
                putExtra(NotificationActionReceiver.EXTRA_NOTIFICATION_ID, dbNotificationId)
                putExtra(NotificationActionReceiver.EXTRA_ORIGINAL_NOTIFICATION_ID, originalNotificationId)
                putExtra(NotificationActionReceiver.EXTRA_SYSTEM_NOTIF_ID, id)
                putExtra(NotificationActionReceiver.EXTRA_REPLY_STATUS, "busy")
            }
            val busyPendingIntent = PendingIntent.getBroadcast(
                context,
                id * 10 + 2,
                busyIntent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )

            builder.addAction(0, "Coming (5 min)", acceptPendingIntent)
            builder.addAction(0, "Busy", busyPendingIntent)
        }

        try {
            val notificationManagerCompat = NotificationManagerCompat.from(context)
            if (notificationManagerCompat.areNotificationsEnabled()) {
                notificationManagerCompat.notify(id, builder.build())
            }
        } catch (e: SecurityException) {
            e.printStackTrace()
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    suspend fun publishTestNotification(topic: String, title: String, messageStr: String): Boolean = withContext(Dispatchers.IO) {
        if (topic.isBlank()) return@withContext false

        // Ensure process network binding is released so request routes through default active network (Cellular/Wi-Fi)
        DeviceDiscoveryManager.releaseNetworkBinding(context)

        val url = "https://ntfy.sh/$topic"
        try {
            val request = Request.Builder()
                .url(url)
                .addHeader("Title", title)
                .addHeader("Tags", "auto_rickshaw,call")
                .post(messageStr.toRequestBody("text/plain".toMediaType()))
                .build()

            postClient.newCall(request).execute().use { response ->
                response.isSuccessful
            }
        } catch (e: Exception) {
            e.printStackTrace()
            false
        }
    }

    suspend fun sendReply(topic: String, replyStatus: String): Boolean = withContext(Dispatchers.IO) {
        if (topic.isBlank()) return@withContext false

        // Ensure process network binding is released so request routes through default active network (Cellular/Wi-Fi)
        DeviceDiscoveryManager.releaseNetworkBinding(context)

        val replyTopic = "${topic}-reply"
        val url = "https://ntfy.sh/$replyTopic"
        try {
            val messageBody = "status:$replyStatus"
            val request = Request.Builder()
                .url(url)
                .post(messageBody.toRequestBody("text/plain".toMediaType()))
                .build()

            postClient.newCall(request).execute().use { response ->
                response.isSuccessful
            }
        } catch (e: Exception) {
            e.printStackTrace()
            false
        }
    }
}
