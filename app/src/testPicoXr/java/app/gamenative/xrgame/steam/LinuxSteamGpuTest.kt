// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import org.junit.Assert.assertEquals
import org.junit.Test

class LinuxSteamGpuTest {
    @Test fun `software drawing keeps the existing CEF flag`() {
        assertEquals(listOf("-cef-disable-gpu"), LinuxSteamGpu.cefArguments(false))
    }

    @Test fun `GPU drawing forces CEF's GPU process and never disables it`() {
        assertEquals(listOf("-cef-force-gpu"), LinuxSteamGpu.cefArguments(true))
    }

    @Test fun `GPU drawing is on unless switched off`() {
        assertEquals(true, LinuxSteamGpu.requestedFrom(null))
        assertEquals(true, LinuxSteamGpu.requestedFrom("on\n"))
        assertEquals(false, LinuxSteamGpu.requestedFrom("off\n"))
    }

    @Test fun `the component path matches the ICD written by build-gpu`() {
        assertEquals("/opt/steampsp-gpu", LinuxSteamGpu.GUEST_ROOT)
        assertEquals("linux-steam/gpu.tgz", LinuxSteamGpu.ASSET)
    }
}
