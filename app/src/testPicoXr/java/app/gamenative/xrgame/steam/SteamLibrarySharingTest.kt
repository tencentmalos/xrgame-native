package app.gamenative.xrgame.steam

import java.io.File
import java.nio.file.Files
import org.junit.Test
import org.junit.Rule
import org.junit.rules.TemporaryFolder
import org.junit.Assert.*
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import android.app.Application
import app.gamenative.xrgame.XrGameStorage

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], manifest = Config.NONE, application = Application::class)
class SteamLibrarySharingTest {
    @get:Rule val temp = TemporaryFolder()
    private fun game() = File(temp.root, "library/steamapps/common/Game").apply { mkdirs() }
    private fun downloaded(dir: File, config: String = "{\"installedManifestIDs\":{\"11\":123,\"12\":18446744073709551614}}") {
        File(dir, ".download_complete").writeText("")
        File(dir, ".DepotDownloader").mkdirs()
        File(dir, ".DepotDownloader/depot.config").writeText(config)
    }
    private fun acf(dir: File, flags: Int = 4, install: String = "Game") = File(dir.parentFile!!.parentFile, "appmanifest_42.acf").apply {
        writeText(""""AppState" { "appid" "42" "installdir" "$install" "StateFlags" "$flags" "InstalledDepots" { "11" { "manifest" "123" } } }""")
    }
    @Test fun `new installs use SteamPSP and reuse an existing old app directory`() {
        assertEquals("SteamPSP", XrGameStorage.selectRoot(temp.root).name)
        File(temp.root, "XRGameNative/Steam/steamapps/common/Game").mkdirs()
        assertEquals("XRGameNative", XrGameStorage.selectRoot(temp.root).name)
        // Merely creating the branded root must not hide the old installed library.
        File(temp.root, "SteamPSP/Steam/steamapps/common").mkdirs()
        assertEquals("XRGameNative", XrGameStorage.selectRoot(temp.root).name)
    }
    @Test fun `selected library stays fixed after uninstalling its last game`() {
        File(temp.root, "XRGameNative/Steam/steamapps/common").mkdirs()
        val preferred = File(temp.root, "SteamPSP").apply { mkdirs() }
        File(preferred, ".steampsp-library").writeText("1")
        assertEquals(preferred, XrGameStorage.selectRoot(temp.root))
    }
    @Test fun `default official library writes into the same physical steamapps`() {
        val official = File(temp.root, "client/steamapps").apply { mkdirs() }
        File(official, "libraryfolders.vdf").writeText("old")
        val shared = File(temp.root, "external/steamapps").apply { mkdirs() }
        LinuxSteamLibrary.linkLibrary(official, shared)
        File(official, "appmanifest_42.acf").writeText("new")
        assertEquals(shared.canonicalFile, official.canonicalFile)
        assertEquals("new", File(shared, "appmanifest_42.acf").readText())
        assertEquals("old", File(official.parentFile, "steamapps.before-shared-directory/libraryfolders.vdf").readText())
        LinuxSteamLibrary.linkLibrary(official, shared)
    }
    @Test fun `private game data is preserved instead of creating two installations`() {
        val official = File(temp.root, "client/steamapps").apply { mkdirs() }
        File(official, "appmanifest_42.acf").writeText("existing")
        assertThrows(IllegalStateException::class.java) {
            LinuxSteamLibrary.linkLibrary(official, File(temp.root, "external/steamapps"))
        }
        assertEquals("existing", File(official, "appmanifest_42.acf").readText())
    }

    @Test fun `export keeps installed unsigned manifest ids and does not invent a build`() {
        val dir = game(); downloaded(dir)
        val kv = SteamLibraryManifest.export(42, "Game", dir, "public", "english")!!
        assertEquals("0", kv["buildid"].value)
        assertEquals("18446744073709551614", kv["InstalledDepots"]["12"]["manifest"].value)
        val file = File(dir.parentFile!!.parentFile, "appmanifest_42.acf")
        SteamLibraryManifest.write(file, kv)
        assertTrue(SteamLibraryManifest.read(file)!!.complete)
    }
    @Test fun `in progress depot never becomes installed`() {
        val dir = game(); downloaded(dir, "{\"installedManifestIDs\":{\"11\":9223372036854775807}}")
        assertNull(SteamLibraryManifest.export(42, "Game", dir, "public", "english"))
    }
    @Test fun `cached manifest without completion does not prove installation`() {
        val dir = game(); downloaded(dir); File(dir, ".download_complete").delete()
        assertNull(SteamLibraryManifest.installedDepots(dir))
    }
    @Test fun `official update cannot use a stale custom completion marker`() {
        val dir = game(); downloaded(dir)
        assertFalse(SteamLibraryManifest.read(acf(dir, 6))!!.complete)
        val file = acf(dir)
        File(file.parentFile, "downloading/42").mkdirs()
        assertFalse(SteamLibraryManifest.read(file)!!.complete)
    }
    @Test fun `custom partial download cannot use an old official installed manifest`() {
        val dir = game(); downloaded(dir)
        File(dir, ".download_in_progress").writeText("")
        assertFalse(SteamLibraryManifest.read(acf(dir))!!.complete)
    }
    @Test fun `removing official manifest does not forget managed installation`() {
        val context = androidx.test.core.app.ApplicationProvider.getApplicationContext<Application>()
        val file = acf(game())
        LinuxSteamLibrary.remember(context, 42, file)
        file.delete()
        assertTrue(LinuxSteamLibrary.managed(context, 42))
    }
    @Test fun `manifest path escapes and wrong app ids are rejected`() {
        val dir = game()
        assertNull(SteamLibraryManifest.read(acf(dir, install = "../../outside")))
        val file = acf(dir); file.writeText(file.readText().replace("\"appid\" \"42\"", "\"appid\" \"43\""))
        assertNull(SteamLibraryManifest.read(file))
    }
    @Test fun `official remote shares the existing save bytes without moving the prefix`() {
        val canonical = File(temp.root, "prefix/remote").apply { mkdirs() }
        File(canonical, "save.dat").writeText("original")
        val official = File(temp.root, "official/remote")
        SteamSaveSharing.linkRemote(official, canonical)
        assertTrue(Files.isSymbolicLink(official.toPath()))
        assertEquals("original", File(official, "save.dat").readText())
        File(official, "save.dat").writeText("changed")
        assertEquals("changed", File(canonical, "save.dat").readText())
        SteamSaveSharing.linkRemote(official, canonical)
    }
    @Test fun `divergent save trees are preserved and never merged`() {
        val canonical = File(temp.root, "prefix/remote").apply { mkdirs() }
        val official = File(temp.root, "official/remote").apply { mkdirs() }
        File(canonical, "save.dat").writeText("custom")
        File(official, "save.dat").writeText("official")
        assertThrows(IllegalStateException::class.java) { SteamSaveSharing.linkRemote(official, canonical) }
        assertEquals("custom", File(canonical, "save.dat").readText())
        assertEquals("official", File(official, "save.dat").readText())
    }
    @Test fun `official only saves are verified and original copy retained`() {
        val official = File(temp.root, "official/remote").apply { mkdirs() }
        val canonical = File(temp.root, "prefix/remote")
        File(official, "save.dat").writeText("official")
        SteamSaveSharing.linkRemote(official, canonical)
        assertEquals("official", File(canonical, "save.dat").readText())
        assertEquals("official", File(official.parentFile, "remote.steampsp-before-sharing/save.dat").readText())
    }
    @Test fun `old forced cloud disable is restored once without changing later user choices`() {
        val file = File(temp.root, "sharedconfig.vdf")
        val old = "\"UserRoamingConfigStore\" { \"Software\" { \"Valve\" { \"Steam\" { \"CloudEnabled\" \"1\" \"Unrelated\" \"keep\" } } } }"
        File(temp.root, "sharedconfig.vdf.steampsp-before-sharing").writeText(old)
        file.writeText(old.replace("\"1\"", "\"0\""))
        SteamSaveSharing.restoreOfficialCloudPreference(file, File(temp.root, "private-migration"))
        val kv = `in`.dragonbra.javasteam.types.KeyValue.loadFromString(file.readText())!!
        assertEquals("1", kv["Software"]["Valve"]["Steam"]["CloudEnabled"].value)
        assertEquals("keep", kv["Software"]["Valve"]["Steam"]["Unrelated"].value)
        assertFalse(File(file.parentFile, "sharedconfig.vdf.steampsp-before-sharing").exists())
        assertFalse(File(file.parentFile, "steampsp-cloud-owner-v2").exists())
        assertTrue(File(temp.root, "private-migration/steampsp-cloud-owner-v2").isFile)
        file.writeText(old.replace("\"1\"", "\"0\""))
        SteamSaveSharing.restoreOfficialCloudPreference(file, File(temp.root, "private-migration"))
        assertEquals("0", `in`.dragonbra.javasteam.types.KeyValue.loadFromString(file.readText())!!["Software"]["Valve"]["Steam"]["CloudEnabled"].value)
    }
    @Test fun `preexisting cloud disabled preference is preserved`() {
        val file = File(temp.root, "sharedconfig.vdf")
        val text = "\"UserRoamingConfigStore\" { \"Software\" { \"Valve\" { \"Steam\" { \"CloudEnabled\" \"0\" } } } }"
        File(temp.root, "sharedconfig.vdf.steampsp-before-sharing").writeText(text)
        file.writeText(text)
        SteamSaveSharing.restoreOfficialCloudPreference(file, File(temp.root, "private-migration"))
        assertEquals(text, file.readText())
    }
    @Test fun `Proton AutoCloud alias reaches existing xuser saves without a second prefix`() {
        val prefix = File(temp.root, "container/.wine").apply { mkdirs() }
        val user = File(prefix, "drive_c/users/xuser").apply { mkdirs() }
        File(user, "Saved Games/Hades II").mkdirs()
        File(user, "Saved Games/Hades II/Profile1.sjson").writeText("original")
        val alias = File(temp.root, "compatdata/1145350/pfx")
        SteamSaveSharing.linkDirectory(alias, prefix)
        SteamSaveSharing.linkDirectory(File(prefix, "drive_c/users/steamuser"), user)
        assertEquals(prefix.canonicalFile, alias.canonicalFile)
        assertEquals("original", File(alias, "drive_c/users/steamuser/Saved Games/Hades II/Profile1.sjson").readText())
        SteamSaveSharing.linkDirectory(alias, prefix)
    }
    @Test fun `existing independent prefix cannot be hidden or overwritten`() {
        val alias = File(temp.root, "compatdata/42/pfx").apply { mkdirs() }
        File(alias, "save").writeText("keep")
        assertThrows(IllegalStateException::class.java) {
            SteamSaveSharing.linkDirectory(alias, File(temp.root, "original").apply { mkdirs() })
        }
        assertEquals("keep", File(alias, "save").readText())
    }
}
