// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Context
import androidx.annotation.Keep
import app.gamenative.utils.SteamUtils
import app.gamenative.xrgame.*
import com.winlator.container.Container
import com.winlator.core.WineRegistryEditor
import com.winlator.core.envvars.EnvVars
import com.winlator.xenvironment.ImageFs
import org.json.JSONObject
import java.io.File

/** Official client authentication with the existing Bionic game runtime, outside PRoot. */
@Keep
class LinuxSteamLauncher : XrGameOfficialSteam.Launcher {
    override fun prepare(context: Context, container: Container, game: File, drive: Char, appId: Int,
                         target: XrGameSteamLaunch.Launch, profile: XrGameSteamLaunch.ClientProfile?, env: EnvVars): XrGameSteamLaunch.Launch {
        val runtime = LinuxSteamRuntime(context)
        check(LinuxSteamService.running) { "Open Official Linux Steam in Settings → Accounts and sign in before launching a Steam game." }
        // Keep the same account as our existing library/cloud paths. Never put its ID in diagnostics.
        val expectedAccount = SteamUtils.getSteamId64()
        check(expectedAccount != null) { "Sign in to Official Steam before launching a game." }
        when (val result = runtime.checkSession(expectedAccount)) {
            0 -> Unit
            8 -> error("Official Steam and the library use different accounts. Sign in with the same test account to preserve saves.")
            else -> error("Official Steam session is not ready ($result). Open Official Linux Steam and finish signing in.")
        }
        SteamSaveSharing.prepareContainer(context, container, appId)
        val wine = File(ImageFs.getSharedProtonDir(context), XrGameRuntimeVersions.WINE)
        val inventory = JSONObject(File(wine, "xrgame-build.json").readText())
        check(inventory.optJSONObject("derivedFrom")?.optString("patch") == "tools/xrgame/patches/wine-linux-steam-client.patch") {
            "Install the official Steam runtime bundle before launching games."
        }
        val prefix = File(container.rootDir, ".wine")
        val driveC = File(prefix, "drive_c").canonicalFile
        // A Custom prefix does not contain lsteamclient, and its cleanup removes
        // the old Bionic Steam copies. Wine's Android DLL search path does not
        // automatically expose this optional Proton module. Deploy the matching
        // bundled PE bridges before steam.exe initializes its registry/IPC.
        for ((architecture, system) in listOf("aarch64-windows" to "system32", "i386-windows" to "syswow64")) {
            val member = "lib/wine/$architecture/lsteamclient.dll"
            XrGameRuntimeFiles.copyVerified(File(wine, member), File(driveC, "windows/$system/lsteamclient.dll"),
                inventory.getJSONObject("files").getJSONObject(member).getString("sha256"))
        }
        val steam = File(driveC, "Program Files (x86)/Steam").canonicalFile
        check(steam.toPath().startsWith(driveC.toPath())) { "Steam prefix path escapes drive C" }
        val compat = File(runtime.directory, "windows-abi")
        val files = JSONObject(File(compat, "steampsp-compat.json").readText()).getJSONObject("files")
        for (name in listOf("steamclient.dll", "steamclient64.dll", "Steam.dll", "tier0_s.dll", "tier0_s64.dll", "vstdlib_s.dll", "vstdlib_s64.dll")) {
            XrGameRuntimeFiles.copyVerified(File(compat, name), File(steam, name), files.getString(name))
        }
        // Proton's source-built helper maintains Windows Steam IPC windows and process lifetime.
        val helper = "lib/wine/aarch64-windows/steam.exe"
        XrGameRuntimeFiles.copyVerified(File(wine, helper), File(steam, "steam.exe"),
            inventory.getJSONObject("files").getJSONObject(helper).getString("sha256"))
        val windowsSteam = "C:\\Program Files (x86)\\Steam"
        WineRegistryEditor(File(prefix, "user.reg")).use { registry ->
            registry.setStringValue("Software\\Valve\\Steam", "SteamPath", windowsSteam)
            registry.setStringValue("Software\\Valve\\Steam", "SteamExe", "$windowsSteam\\steam.exe")
            registry.setStringValue("Software\\Valve\\Steam\\ActiveProcess", "SteamClientDll", "$windowsSteam\\steamclient.dll")
            registry.setStringValue("Software\\Valve\\Steam\\ActiveProcess", "SteamClientDll64", "$windowsSteam\\steamclient64.dll")
        }
        val base = if (profile?.nestedGamePath == true) XrGameSteamClient.ensureGameAlias(driveC, game, appId) else "${drive.uppercaseChar()}:\\"
        val executable = base + game.canonicalFile.toPath().relativize(target.executable.toPath()).toString().replace('/', '\\')
        // Selected guest instrumentation retains the existing per-image loader hook, without GBE.
        val patches = XrGameGuestPatches.prepare(context, container, appId, profile?.followSelfRestart == true)
        patches?.let {
            it.environment.forEach { (key, value) -> env.put(key, value) }
            val relative = driveC.toPath().relativize(it.installer.canonicalFile.toPath())
            check(!relative.startsWith(".."))
            env.put("XRGAME_BOOTSTRAP_IMAGE", executable)
            env.put("XRGAME_BOOTSTRAP_DLL", "C:\\" + relative.toString().replace('/', '\\'))
        }
        env.put("HOME", runtime.home.path)
        env.put("XRGAME_STEAMCLIENT_LIBRARY", File(runtime.client, "androidarm64/libsteamclient.so").path)
        env.put("STEAM_COMPAT_CLIENT_INSTALL_PATH", runtime.client.path)
        env.put("SteamAppId", appId.toString())
        env.put("SteamGameId", appId.toString())
        env.put("PROTON_DISABLE_LSTEAMCLIENT", "0")
        // Android kills an app's child processes beyond a device-wide limit (32 by default), and the
        // official client already holds about a dozen. Wine's touch keyboard helper is one we can
        // spare: Android's IME handles text input.
        val overrides = env.get("WINEDLLOVERRIDES").split(';').map(String::trim).filter(String::isNotEmpty)
            .filterNot { it.substringBefore('=').equals("tabtip.exe", ignoreCase = true) }
        env.put("WINEDLLOVERRIDES", (overrides + "tabtip.exe=d").joinToString(";"))
        val work = base + game.canonicalFile.toPath().relativize(target.workingDirectory.toPath()).toString().replace('/', '\\')
        val args = if (target.arguments.isBlank()) "" else " ${target.arguments}"
        // Wine's /exec accepts an executable directly and waits for it. An empty
        // cmd.exe-style title is parsed as the executable by this start.exe build.
        val command = "start.exe /d ${XrGameCommandLine.quote(work)} /exec " +
            "${XrGameCommandLine.quote("$windowsSteam\\steam.exe")} ${XrGameCommandLine.quote(executable)}$args"
        return XrGameSteamLaunch.Launch(command, File(steam, "steam.exe"), target.workingDirectory)
    }
}
