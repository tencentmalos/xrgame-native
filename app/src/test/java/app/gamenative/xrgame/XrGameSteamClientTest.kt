package app.gamenative.xrgame

import java.io.File
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.*
import org.junit.Test

class XrGameSteamClientTest {
    private val names = listOf("steamclient_loader_x64.exe", "steamclient64.dll", "extra/steamclient_extra_x64.dll", "LICENSE", "provenance.json")

    @Test fun dlcConfigurationIsExplicitAndInstallPathResolvesOutsideTheClientCache() {
        val ini = XrGameSteamClient.appConfiguration(582010, mapOf(1118010 to "Iceborne\n[app::dlcs]\nunlock_all=1"))
        assertTrue(ini.contains("unlock_all=0\n"))
        assertFalse(ini.contains("\nunlock_all=1"))
        assertTrue(ini.contains("582010=../../games/582010\n"))
        val client = File("/prefix/drive_c/xrgame/steamclient/catalog-hash")
        assertEquals(File("/prefix/drive_c/xrgame/games/582010"), File(client, "../../games/582010").canonicalFile)
        assertEquals("[app::dlcs]\nunlock_all=0\n\n[app::paths]\n1=../../games/1\n",
            XrGameSteamClient.appConfiguration(1, emptyMap()))
    }

    @Test fun clientIdentityAndSaveRootMatchSteamCloudIncludingUnsignedAccountIds() {
        for (accountId in listOf(12345L, 0xf0000001L)) {
            val id = 0x0110000100000000L or accountId
            val config = XrGameSteamClient.userConfiguration(id)
            assertTrue(config.contains("account_steamid=$id\n"))
            assertTrue(config.contains("[user::saves]\nlocal_save_path=C:\\Program Files (x86)\\Steam\\userdata\\$accountId\n"))
            assertFalse(config.contains("saves_folder_name="))
        }
    }

    @Test fun absentOrInvalidIdentityCannotSilentlyCreateAnUnrelatedSaveProfile() {
        for (id in listOf(null, 0L, -1L, 12345L, 0x0110000100000000L)) {
            assertThrows(IllegalStateException::class.java) { XrGameSteamClient.userConfiguration(id) }
        }
    }

    @Test fun gameAliasPreservesDataAndFollowsAnInstallationMove() {
        val root = Files.createTempDirectory("xrgame-alias").toFile()
        val drive = File(root, "drive_c").apply { mkdirs() }
        val game = File(root, "Monster Hunter World").apply { mkdirs() }
        val moved = File(root, "moved").apply { mkdirs() }
        val alias = File(drive, "xrgame/games/582010")
        try {
            File(game, "save").writeText("preserve")
            assertEquals("C:\\xrgame\\games\\582010\\", XrGameSteamClient.ensureGameAlias(drive, game, 582010))
            assertEquals("preserve", File(alias, "save").readText())
            XrGameSteamClient.ensureGameAlias(drive, game, 582010)
            XrGameSteamClient.ensureGameAlias(drive, moved, 582010)
            assertEquals(moved.canonicalFile, alias.canonicalFile)
            assertEquals("preserve", File(game, "save").readText())
        } finally { Files.deleteIfExists(alias.toPath()); root.deleteRecursively() }
    }

    @Test fun gameAliasRejectsDirectoryConflictsAndEscapingParents() {
        val root = Files.createTempDirectory("xrgame-alias").toFile()
        val drive = File(root, "drive_c").apply { mkdirs() }
        val game = File(root, "game").apply { mkdirs() }
        val alias = File(drive, "xrgame/games/582010").apply { mkdirs() }
        try {
            File(alias, "save").writeText("keep")
            assertThrows(IllegalStateException::class.java) { XrGameSteamClient.ensureGameAlias(drive, game, 582010) }
            assertEquals("keep", File(alias, "save").readText())
            val otherDrive = File(root, "other_c").apply { mkdirs() }
            val escape = File(otherDrive, "xrgame")
            Files.createSymbolicLink(escape.toPath(), game.toPath())
            try {
                assertThrows(IllegalStateException::class.java) { XrGameSteamClient.ensureGameAlias(otherDrive, game, 582010) }
                assertFalse(File(game, "games").exists())
            } finally { Files.delete(escape.toPath()) }
        } finally { root.deleteRecursively() }
    }

    private fun archive(directory: File, members: List<String>): File = File(directory, "client.zip").also { file ->
        ZipOutputStream(file.outputStream()).use { zip ->
            members.forEach { name -> zip.putNextEntry(ZipEntry(name)); zip.write("verified $name".toByteArray()); zip.closeEntry() }
        }
    }

    @Test fun extractsOnlyPinnedMembersAndRepairsChangedCachedBinary() {
        val root = Files.createTempDirectory("xrgame-client").toFile()
        try {
            val zip = archive(root, names)
            val dest = File(root, "prefix").apply { mkdirs() }
            XrGameSteamClient.extract(zip, dest)
            File(dest, "steamclient64.dll").writeText("changed")
            XrGameSteamClient.extract(zip, dest)
            assertEquals("verified steamclient64.dll", File(dest, "steamclient64.dll").readText())
        } finally { root.deleteRecursively() }
    }

    @Test fun rejectsMissingAndUnexpectedMembersIncludingTraversal() {
        val root = Files.createTempDirectory("xrgame-client").toFile()
        try {
            val dest = File(root, "prefix").apply { mkdirs() }
            for (members in listOf(names.dropLast(1), listOf("../escape.dll"), listOf("extra/unexpected.dll"))) {
                assertThrows(IllegalArgumentException::class.java) { XrGameSteamClient.extract(archive(root, members), dest) }
            }
            assertFalse(File(root, "escape.dll").exists())
        } finally { root.deleteRecursively() }
    }

    @Test fun configurationPreservesPathsAndRejectsIniInjection() {
        val ini = XrGameSteamClient.configuration(1145350, "A:\\Ship\\Hades2.exe", "A:\\Ship", "/c ../", "C:\\xrgame\\client")
        assertTrue(ini.contains("Exe=A:\\Ship\\Hades2.exe\n"))
        assertTrue(ini.contains("ExeCommandLine=/c ../\n"))
        assertTrue(ini.contains("Mode=0\n"))
        assertTrue(ini.contains("DllsToInjectFolder=\n"))
        assertTrue(ini.contains("WineBootstrap=0\n"))
        assertTrue(XrGameSteamClient.configuration(1145350, "A:\\Ship\\Hades2.exe", "A:\\Ship", "/c ../", "C:\\xrgame\\client", true)
            .contains("DllsToInjectFolder=C:\\xrgame\\client\\extra\n"))
        assertThrows(IllegalArgumentException::class.java) {
            XrGameSteamClient.configuration(1, "bad\n[Injection]", "A:\\", "", "C:\\")
        }
    }

    @Test fun restartBootstrapRequiresTheReviewedDllAndDoesNotEnablePersistentRelaunch() {
        assertThrows(IllegalArgumentException::class.java) {
            XrGameSteamClient.configuration(582010, "C:\\game\\game.exe", "C:\\game", "", "C:\\client", false, true)
        }
        val ini = XrGameSteamClient.configuration(582010, "C:\\game\\game.exe", "C:\\game", "", "C:\\client", true, true)
        assertTrue(ini.contains("WineBootstrap=1\n"))
        assertTrue(ini.contains("Mode=0\n"))
    }
}
