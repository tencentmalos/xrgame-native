package app.gamenative.xrgame

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Test

class XrGameSteamLaunchTest {
    private val root = File(System.getProperty("java.io.tmpdir"), "Hades II").canonicalFile

    @Test fun hadesUsesVerifiedClientProfileWithoutOverridingUserTools() {
        val profile = XrGameSteamLaunch.clientProfile(1145350, "Release\\Hades2.exe")!!
        assertEquals("Ship/Hades2.exe", profile.executable)
        assertEquals("/c ../", profile.arguments)
        assertFalse(profile.nestedGamePath)
        assertFalse(profile.followSelfRestart)
        assertNull(XrGameSteamLaunch.clientProfile(1145350, "tools/config.exe"))
        assertNull(XrGameSteamLaunch.clientProfile(1, "Release/Hades2.exe"))
    }

    @Test fun launchesInstalledPeWithItsWorkingDirectoryAndArguments() {
        val launch = XrGameSteamLaunch.resolve(root, 'a', "Release/Hades2.exe", "Release\\Hades2.exe", "--test \"two words\"", "Release")
        assertEquals("\"A:\\Release\\Hades2.exe\" --test \"two words\"", launch.command)
        assertEquals(File(root, "Release"), launch.workingDirectory)
        assertEquals(File(root, "Release/Hades2.exe"), launch.executable)
        assertFalse(launch.command.contains("steamclient_loader"))
    }

    @Test fun monsterHunterUsesClientAtGameRootWithoutOverridingUserTools() {
        for (selected in listOf("", "MonsterHunterWorld.exe", "MONSTERHUNTERWORLD.EXE")) {
            val profile = XrGameSteamLaunch.clientProfile(582010, selected)!!
            assertTrue(profile.nestedGamePath)
            assertTrue(profile.followSelfRestart)
            val launch = XrGameSteamLaunch.resolve(root, 'A', profile.executable,
                profile.executable, profile.arguments, profile.workingDirectory)
            assertEquals("\"A:\\MonsterHunterWorld.exe\"", launch.command)
            assertEquals(root, launch.workingDirectory)
        }
        assertNull(XrGameSteamLaunch.clientProfile(582010, "tools/config.exe"))
        assertNull(XrGameSteamLaunch.clientProfile(1, "MonsterHunterWorld.exe"))
    }

    @Test fun userSelectedExecutableDoesNotInheritAnotherLaunchConfiguration() {
        val launch = XrGameSteamLaunch.resolve(root, 'A', "Ship/Hades2.exe", "Release/Hades2.exe", "--other", "Release")
        assertEquals("\"A:\\Ship\\Hades2.exe\"", launch.command)
        assertEquals(File(root, "Ship"), launch.workingDirectory)
    }

    @Test fun missingSelectionUsesManifestAndExecutableDirectory() {
        val launch = XrGameSteamLaunch.resolve(root, 'D', "", "Game Folder/game.exe", "", "")
        assertEquals("\"D:\\Game Folder\\game.exe\"", launch.command)
        assertEquals(File(root, "Game Folder"), launch.workingDirectory)
    }

    @Test fun invalidPathsFailBeforeStartingWine() {
        for (exe in listOf("", "../outside.exe", "/outside.exe", "C:\\outside.exe", "bad\"name.exe")) {
            assertThrows(IllegalArgumentException::class.java) { XrGameSteamLaunch.resolve(root, 'A', exe, "", "", "") }
        }
        assertThrows(IllegalArgumentException::class.java) { XrGameSteamLaunch.resolve(root, 'A', "game.exe", "game.exe", "", "../outside") }
    }

    @Test fun bundledClientIsSelectedByPeArchitectureForUnlistedGames() {
        val file = Files.createTempFile("xrgame-pe", ".exe").toFile()
        try {
            val bytes = ByteBuffer.allocate(256).order(ByteOrder.LITTLE_ENDIAN)
            bytes.putShort(0, 0x5a4d.toShort()).putInt(0x3c, 128).putInt(128, 0x4550).putShort(132, 0x8664.toShort())
            file.writeBytes(bytes.array())
            assertTrue(XrGameSteamLaunch.usesBundledClient(file))
            bytes.putShort(132, 0x14c.toShort()); file.writeBytes(bytes.array())
            assertFalse(XrGameSteamLaunch.usesBundledClient(file))
            bytes.putInt(0x3c, -1); file.writeBytes(bytes.array())
            assertFalse(XrGameSteamLaunch.usesBundledClient(file))
            file.writeText("@echo off")
            assertFalse(XrGameSteamLaunch.usesBundledClient(file))
        } finally { file.delete() }
    }
}
