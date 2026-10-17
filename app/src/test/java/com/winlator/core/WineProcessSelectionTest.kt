package com.winlator.core

import org.junit.Assert.*
import org.junit.Test

class WineProcessSelectionTest {
    @Test fun `official client handoff and shell arguments are never wine processes`() {
        for (argv in listOf(
            listOf("/usr/bin/python3", "launch.py", "waitforexitandrun", "/games/Hades II/Ship/Hades2.exe"),
            listOf("/bin/sh", "-c", "proton run game.exe"),
            listOf("/usr/bin/reaper", "SteamLaunch", "--", "wine", "game.exe"),
            listOf("/usr/bin/steam", "steam://run/42"))) {
            assertFalse(argv.toString(), ProcessHelper.isWineCommandLine(argv.joinToString("\u0000")))
        }
    }
    @Test fun `wine launchers and renamed Windows processes remain controllable`() {
        for (argv in listOf(
            listOf("/path/bin/wine", "explorer", "game.exe"),
            listOf("/system/bin/linker64", "/path/bin/wine", "game.exe"),
            listOf("/path/bin/wineserver"),
            listOf("C:\\windows\\system32\\winedevice.exe"),
            listOf("A:\\Ship\\Hades2.exe", "/c", "../"))) {
            assertTrue(argv.toString(), ProcessHelper.isWineCommandLine(argv.joinToString("\u0000")))
        }
    }
}
