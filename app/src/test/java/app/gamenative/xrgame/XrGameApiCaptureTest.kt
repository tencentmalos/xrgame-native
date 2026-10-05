package app.gamenative.xrgame

import com.winlator.core.envvars.EnvVars
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class XrGameApiCaptureTest {
    @Test fun modeRoundTripsThroughContainerEnvironmentAndOffRemovesTheSetting() {
        val enabled = XrGameApiCapture.withMode("WINEESYNC=1", "d3d12")
        assertEquals("d3d12", XrGameApiCapture.mode(enabled))
        assertEquals("1", EnvVars(enabled).get("WINEESYNC"))
        val disabled = XrGameApiCapture.withMode(enabled, "off")
        assertEquals("off", XrGameApiCapture.mode(disabled))
        assertFalse(EnvVars(disabled).has(XrGameApiCapture.MODE_ENV))
    }

    @Test fun unknownModeIsTreatedAsOff() {
        assertEquals("off", XrGameApiCapture.mode("XRGAME_API_CAPTURE=d3d11"))
    }

    @Test fun d3d12UsesWindowsPathsAndTheRuntimeDirectory() {
        val env = XrGameApiCapture.environment("d3d12", 3, "page_guard", "/data/x/xrgame-captures/STEAM_1", "STEAM_1")
        assertEquals("Z:\\xrgame-captures\\STEAM_1\\capture.gfxr", env["GFXRECON_CAPTURE_FILE"])
        assertEquals("Z:\\xrgame-captures\\STEAM_1\\trigger", env["GFXRECON_CAPTURE_TRIGGER_FILE"])
        assertEquals("C:\\xrgame\\gfxr\\runtime", env["GFXRECON_DX_RUNTIME_DIR"])
        assertEquals("3", env["GFXRECON_CAPTURE_TRIGGER_FRAMES"])
        assertTrue(env.values.none { it.contains(' ') })
    }

    @Test fun vulkanUsesUnixPathsWithoutTheDirectXRuntimeDirectory() {
        val env = XrGameApiCapture.environment("vulkan", 1, "unassisted", "/data/x/xrgame-captures/STEAM_1", "STEAM_1")
        assertEquals("/data/x/xrgame-captures/STEAM_1/capture.gfxr", env["GFXRECON_CAPTURE_FILE"])
        assertEquals("unassisted", env["GFXRECON_MEMORY_TRACKING_MODE"])
        assertNull(env["GFXRECON_DX_RUNTIME_DIR"])
    }

    @Test fun d3d12DefaultsToUnassistedTrackingBecausePageGuardBlackedOutMhr() {
        assertEquals("unassisted", XrGameApiCapture.defaultMemoryTracking("d3d12"))
        assertEquals("page_guard", XrGameApiCapture.defaultMemoryTracking("vulkan"))
    }

    @Test fun invalidSettingsAreRejected() {
        val dir = "/data/x"
        assertThrows(IllegalArgumentException::class.java) { XrGameApiCapture.environment("off", 3, "page_guard", dir, "STEAM_1") }
        assertThrows(IllegalArgumentException::class.java) { XrGameApiCapture.environment("d3d12", 0, "page_guard", dir, "STEAM_1") }
        assertThrows(IllegalArgumentException::class.java) { XrGameApiCapture.environment("d3d12", 3, "fast", dir, "STEAM_1") }
        assertThrows(IllegalArgumentException::class.java) { XrGameApiCapture.environment("d3d12", 3, "page_guard", dir, "../x") }
    }
}
