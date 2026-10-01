package com.example

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.example.data.AutoAlertDatabase
import com.example.data.NotificationEntity
import com.example.data.NotificationRepository
import com.example.data.NtfyManager
import com.example.data.NtfyMessage
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class NtfyTiltSubscriptionTest {

    private lateinit var context: Context
    private lateinit var database: AutoAlertDatabase
    private lateinit var repository: NotificationRepository
    private lateinit var ntfyManager: NtfyManager

    @Before
    fun setup() {
        context = ApplicationProvider.getApplicationContext()
        database = Room.inMemoryDatabaseBuilder(context, AutoAlertDatabase::class.java)
            .allowMainThreadQueries()
            .build()
        repository = NotificationRepository(database.notificationDao())
        ntfyManager = NtfyManager(context, repository)
    }

    @After
    fun teardown() {
        ntfyManager.cleanup()
        database.close()
    }

    @Test
    fun testSafetyMessageOnConfiguredTiltTopic_classifiedAsSafetyAlert() = runBlocking {
        val primaryTopic = "autoalert-driver-101"
        val backupTopic = "autoalert-driver-101-backup"
        val tiltTopic = "autoalert-tilt-xyz99"

        ntfyManager.startListening(
            topic = primaryTopic,
            backupTopic = backupTopic,
            tiltTopic = tiltTopic
        )

        val safetyMessage = NtfyMessage(
            id = "msg_safety_001",
            time = System.currentTimeMillis(),
            event = "message",
            topic = tiltTopic,
            title = "MPU SAFETY: Vehicle Tilt Detected",
            message = "Tilt 54.2 deg >= 50.0 degrees! Rollover danger!",
            tags = listOf("warning", "safety")
        )

        val handled = ntfyManager.handleIncomingMessage(safetyMessage)
        assertTrue(handled)

        val notifications = repository.allNotifications.first()
        assertEquals(1, notifications.size)

        val saved = notifications.first()
        assertEquals("Safety Alert", saved.status)
        assertTrue(saved.title.contains("Tilt") || saved.title.contains("SAFETY"))
        assertTrue(saved.requestId.startsWith("TILT-"))
    }

    @Test
    fun testSafetyMessageWithoutTiltInTopicName_classifiedByTiltTopicMatch() = runBlocking {
        // Even if custom tilt topic name has no 'tilt' in it, it must match configured tiltTopic
        val primaryTopic = "driver-main"
        val backupTopic = "driver-backup"
        val customTiltTopic = "safety-stand-module-42"

        ntfyManager.startListening(
            topic = primaryTopic,
            backupTopic = backupTopic,
            tiltTopic = customTiltTopic
        )

        val safetyMsg = NtfyMessage(
            id = "msg_custom_002",
            time = System.currentTimeMillis(),
            event = "message",
            topic = customTiltTopic,
            title = "Hazard Alert",
            message = "Extreme angle warning",
            tags = listOf("hazard")
        )

        val handled = ntfyManager.handleIncomingMessage(safetyMsg)
        assertTrue(handled)

        val notifications = repository.allNotifications.first()
        val saved = notifications.find { it.message.contains("Extreme angle") }
        assertNotNull(saved)
        assertEquals("Safety Alert", saved?.status)
    }

    @Test
    fun testNormalServiceMessageStillClassifiedAsPending() = runBlocking {
        val primaryTopic = "autoalert-driver-202"
        val tiltTopic = "autoalert-driver-202-tilt"

        ntfyManager.startListening(
            topic = primaryTopic,
            backupTopic = "$primaryTopic-backup",
            tiltTopic = tiltTopic
        )

        val regularMsg = NtfyMessage(
            id = "msg_reg_003",
            time = System.currentTimeMillis(),
            event = "message",
            topic = primaryTopic,
            title = "Passenger Ride Call",
            message = "Auto needed at Main Stand",
            tags = listOf("auto_rickshaw", "call", "batt75")
        )

        val handled = ntfyManager.handleIncomingMessage(regularMsg)
        assertTrue(handled)

        val notifications = repository.allNotifications.first()
        val saved = notifications.find { it.message.contains("Auto needed") }
        assertNotNull(saved)
        assertEquals("Pending", saved?.status)
        assertTrue(saved?.requestId?.startsWith("REQ-") == true)
    }
}
