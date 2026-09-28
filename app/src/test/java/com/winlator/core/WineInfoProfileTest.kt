package com.winlator.core

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import com.winlator.contents.ContentProfile
import com.winlator.contents.ContentsManager
import io.mockk.every
import io.mockk.mockk
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class WineInfoProfileTest {
    private val context = ApplicationProvider.getApplicationContext<Context>()

    @Test fun profileNamesAndEntryNamesResolveTheSameArm64ecRuntime() {
        val profile = ContentProfile().apply {
            type = ContentProfile.ContentType.CONTENT_TYPE_PROTON
            verName = "proton-11.0-2-arm64ec"
            verCode = 12
        }
        val manager = mockk<ContentsManager>()
        every { manager.getProfileByEntryName(any()) } returns profile
        for (name in listOf(profile.verName, "Proton-${profile.verName}", ContentsManager.getEntryName(profile))) {
            val info = WineInfo.fromIdentifier(context, manager, name)
            assertTrue(info.isArm64EC)
            assertEquals("11.0-2", info.fullVersion())
            assertEquals(profile.verName, info.identifier())
            assertEquals(ContentsManager.getInstallDir(context, profile).path, info.path)
        }
    }
}
