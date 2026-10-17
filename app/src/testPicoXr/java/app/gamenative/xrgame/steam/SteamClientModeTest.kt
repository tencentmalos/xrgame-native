package app.gamenative.xrgame.steam

import android.app.Application
import androidx.test.core.app.ApplicationProvider
import app.gamenative.xrgame.XrGameOfficialSteam
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.Assert.*
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], manifest = Config.NONE, application = Application::class)
class SteamClientModeTest {
    private val context get() = ApplicationProvider.getApplicationContext<Application>()
    @Before fun before() { XrGameOfficialSteam.initialize(context); XrGameOfficialSteam.releaseLibrary(); XrGameOfficialSteam.selectCustom() }
    @After fun after() { XrGameOfficialSteam.releaseLibrary(); XrGameOfficialSteam.selectCustom() }
    @Test fun `official ownership survives process initialization and blocks a premature custom switch`() {
        XrGameOfficialSteam.selectOfficial()
        XrGameOfficialSteam.claimLibrary()
        XrGameOfficialSteam.initialize(context)
        assertTrue(XrGameOfficialSteam.active)
        assertThrows(IllegalStateException::class.java) { XrGameOfficialSteam.selectCustom() }
        XrGameOfficialSteam.releaseLibrary()
        XrGameOfficialSteam.selectCustom()
        assertFalse(XrGameOfficialSteam.active)
    }
    @Test fun `only the official handoff authorizes an official game`() {
        XrGameOfficialSteam.selectOfficial()
        XrGameOfficialSteam.claimLibrary()
        assertThrows(IllegalStateException::class.java) { XrGameOfficialSteam.requireLaunchOwner(42) }
        XrGameOfficialSteam.authorizeGame(42)
        XrGameOfficialSteam.requireLaunchOwner(42)
        assertThrows(IllegalStateException::class.java) { XrGameOfficialSteam.requireLaunchOwner(43) }
        XrGameOfficialSteam.finishGame(42)
        assertThrows(IllegalStateException::class.java) { XrGameOfficialSteam.requireLaunchOwner(42) }
        XrGameOfficialSteam.releaseLibrary()
        XrGameOfficialSteam.selectCustom()
        XrGameOfficialSteam.requireLaunchOwner(42)
        assertFalse(XrGameOfficialSteam.isOfficialGame(42))
    }
    @Test fun `official account comes from Valve most recent identity without Java login`() {
        assertEquals(76561198000000002L, SteamOfficialAccount.select(""""users" {
          "76561198000000001" { "MostRecent" "0" }
          "76561198000000002" { "MostRecent" "1" }
        }"""))
        assertNull(SteamOfficialAccount.select(""""users" {
          "76561198000000001" { "MostRecent" "0" }
          "76561198000000002" { "MostRecent" "0" }
        }"""))
    }
}
