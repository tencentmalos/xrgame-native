// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import org.junit.Assert.assertEquals
import org.junit.Test

class SteamLaunchStepTest {
    @Test fun `the transition page shows three steps in startup order`() {
        assertEquals(0, launchStep(LinuxSteamPhase.PREPARING))
        assertEquals(0, launchStep(LinuxSteamPhase.DOWNLOADING))
        assertEquals(1, launchStep(LinuxSteamPhase.OPENING))
        assertEquals(2, launchStep(LinuxSteamPhase.STARTING))
    }

    @Test fun `a stopped or failed client shows no progress`() {
        assertEquals(-1, launchStep(LinuxSteamPhase.STOPPED))
    }
}
