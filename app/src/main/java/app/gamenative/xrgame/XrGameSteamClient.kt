package app.gamenative.xrgame

import android.content.Context
import app.gamenative.service.SteamService
import app.gamenative.utils.SteamUtils
import java.io.File
import java.net.URI
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.zip.ZipInputStream
import org.json.JSONObject
import com.winlator.xenvironment.ImageFs

/** Catalog-pinned offline client, staged inside the prefix without replacing game files. */
object XrGameSteamClient {
    private val binaries = setOf("steamclient_loader_x64.exe", "steamclient64.dll", "extra/steamclient_extra_x64.dll")
    private val archiveFiles = binaries + setOf("LICENSE", "provenance.json")

    @Synchronized
    fun prepare(context: Context, prefix: File, game: File, drive: Char, appId: Int,
                target: XrGameSteamLaunch.Launch, injectExtra: Boolean = false,
                nestedGamePath: Boolean = false, followSelfRestart: Boolean = false,
                language: String? = null): XrGameSteamLaunch.Launch {
        val xrProfile = XrGameProfiler.region("steamclient.prepare")
        try {
        check(target.executable.isFile && target.workingDirectory.isDirectory) {
            "Steam launch files are missing. Verify the installed game in Steam."
        }
        // Use the same persisted identity as AutoCloud, including when launching offline.
        // A generated client identity cannot load account-bound saves.
        val userConfig = userConfiguration(SteamUtils.getSteamId64(), language)
        if (followSelfRestart) {
            val record = File(File(ImageFs.getSharedProtonDir(context), XrGameRuntimeVersions.WINE), "xrgame-build.json")
            check(JSONObject(record.readText()).optInt("xrgameBootstrapVersion") == 1) {
                "This game requires the bundled Proton runtime with self-restart support. Repair the runtime first."
            }
        }
        val entry = XrGameComponents.load(context).items["steamclient"]?.singleOrNull()
            ?: error("This XRGame runtime has no verified offline Steam client. Install a runtime bundle containing steamclient.")
        val archive = File(context.cacheDir, URI(entry.url).path.substringAfterLast('/'))
        XrGameComponents.download(context, entry, archive)
        val driveC = File(prefix, "drive_c").canonicalFile
        val relative = "xrgame/steamclient/${requireNotNull(entry.sha256)}"
        val directory = File(driveC, relative).canonicalFile
        check(directory.toPath().startsWith(driveC.toPath())) { "Client staging directory escapes prefix" }
        directory.mkdirs()
        extract(archive, directory)
        val windows = "C:\\${relative.replace('/', '\\')}"
        val alias = ensureGameAlias(driveC, game, appId)
        val gameBase = if (nestedGamePath) alias else "${drive.uppercaseChar()}:\\"
        val executable = gameBase + game.canonicalFile.toPath().relativize(target.executable.toPath()).toString().replace('/', '\\')
        val work = gameBase + game.canonicalFile.toPath().relativize(target.workingDirectory.toPath()).toString().replace('/', '\\')
        writeAtomic(File(directory, "ColdClientLoader.ini"), configuration(appId, executable, work, target.arguments, windows, injectExtra, followSelfRestart))
        val settings = File(directory, "steam_settings")
        settings.mkdirs()
        writeAtomic(File(settings, "configs.main.ini"), """
            [main::connectivity]
            offline=1
            disable_networking=1
            disable_sharing_stats_with_gameserver=1
        """.trimIndent() + "\n")
        writeAtomic(File(settings, "configs.app.ini"), appConfiguration(appId, SteamService.getXrGameDlcForLaunch(appId)))
        writeAtomic(File(settings, "configs.user.ini"), userConfig)
        return XrGameSteamLaunch.Launch("\"$windows\\steamclient_loader_x64.exe\"",
            File(directory, "steamclient_loader_x64.exe"), directory)
        } finally { xrProfile.close() }
    }

    internal fun appConfiguration(appId: Int, dlcs: Map<Int, String>): String {
        require(appId > 0 && dlcs.keys.all { it > 0 && it != appId })
        return buildString {
            appendLine("[app::dlcs]")
            appendLine("unlock_all=0")
            dlcs.toSortedMap().forEach { (id, name) ->
                appendLine("$id=" + name.map { if (it.isISOControl()) ' ' else it }.joinToString(""))
            }
            // GBE resolves these paths relative to steamclient64.dll, not the game EXE.
            // The alias also makes GetAppInstallDir independent of the catalog hash.
            appendLine("\n[app::paths]")
            appendLine("$appId=../../games/$appId")
        }
    }

    /** [language] is the container's Steam API language name (e.g. "schinese"); others fall back to English. */
    internal fun userConfiguration(steamId64: Long?, language: String? = null): String {
        check(steamId64 != null && steamId64 ushr 32 == 0x01100001L &&
            steamId64 and 0xffffffffL != 0L) {
            "Steam account identity is missing. Sign in to Steam before launching this game."
        }
        val accountId = steamId64 and 0xffffffffL
        val steamLanguage = language?.lowercase()?.takeIf { it.matches(Regex("[a-z]{2,32}")) } ?: "english"
        return """
            [user::general]
            account_name=XRGame
            account_steamid=$steamId64
            language=$steamLanguage

            [user::saves]
            local_save_path=C:\Program Files (x86)\Steam\userdata\$accountId
        """.trimIndent() + "\n"
    }

    /** Give root-sensitive games a directory path without copying or changing their installation. */
    internal fun ensureGameAlias(driveC: File, game: File, appId: Int): String {
        require(appId > 0 && game.isDirectory)
        val root = driveC.canonicalFile.toPath()
        val parent = File(driveC, "xrgame/games").canonicalFile.toPath()
        check(parent.startsWith(root)) { "Game alias directory escapes prefix" }
        Files.createDirectories(parent)
        val alias = parent.resolve(appId.toString())
        val destination = game.canonicalFile.toPath()
        if (Files.exists(alias, LinkOption.NOFOLLOW_LINKS)) {
            check(Files.isSymbolicLink(alias)) { "Game alias conflicts with an existing file or directory" }
        }
        if (!Files.isSymbolicLink(alias) || Files.readSymbolicLink(alias) != destination) {
            val pending = Files.createTempFile(parent, ".game-$appId-", ".link")
            try {
                Files.delete(pending)
                Files.createSymbolicLink(pending, destination)
                Files.move(pending, alias, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
            } finally {
                Files.deleteIfExists(pending)
            }
        }
        return "C:\\xrgame\\games\\$appId\\"
    }

    internal fun configuration(appId: Int, executable: String, workingDirectory: String,
                               arguments: String, directory: String, injectExtra: Boolean = false,
                               followSelfRestart: Boolean = false): String {
        require(appId > 0)
        require(!followSelfRestart || injectExtra) { "Self-restart requires an explicit bootstrap DLL" }
        require(listOf(executable, workingDirectory, arguments, directory).none { value -> value.any { it.isISOControl() } })
        return """
            [SteamClient]
            AppId=$appId
            Exe=$executable
            ExeRunDir=$workingDirectory
            ExeCommandLine=$arguments
            SteamClientDll=$directory\steamclient64.dll
            SteamClient64Dll=$directory\steamclient64.dll
            [Injection]
            DllsToInjectFolder=${if (injectExtra) "$directory\\extra" else ""}
            ForceInjectSteamClient=0
            ForceInjectGameOverlayRenderer=0
            IgnoreInjectionError=0
            WineBootstrap=${if (followSelfRestart) 1 else 0}
            [Persistence]
            Mode=0
        """.trimIndent() + "\n"
    }

    internal fun extract(archive: File, directory: File) {
        val seen = mutableSetOf<String>()
        var total = 0L
        ZipInputStream(archive.inputStream().buffered()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                require(!entry.isDirectory && entry.name in archiveFiles && seen.add(entry.name)) { "Unexpected Steam client archive member" }
                val target = File(directory, entry.name).canonicalFile
                require(target.toPath().startsWith(directory.canonicalFile.toPath())) { "Steam client member escapes staging directory" }
                target.parentFile!!.mkdirs()
                val partial = File.createTempFile(".client-", ".part", target.parentFile)
                try {
                    partial.outputStream().use { output ->
                        val buffer = ByteArray(65536)
                        while (true) {
                            val count = zip.read(buffer)
                            if (count < 0) break
                            total += count
                            require(total <= 64L * 1024 * 1024) { "Steam client archive exceeds extraction limit" }
                            output.write(buffer, 0, count)
                        }
                        output.fd.sync()
                    }
                    // Verify cached files against the authenticated archive on every launch.
                    if (!target.isFile || digest(target) != digest(partial)) {
                        Files.move(partial.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
                    }
                } finally { partial.delete() }
            }
        }
        require(seen == archiveFiles) { "Incomplete Steam client archive" }
    }

    private fun digest(file: File): List<Byte> {
        val sha = MessageDigest.getInstance("SHA-256")
        file.inputStream().buffered().use { input ->
            val bytes = ByteArray(65536)
            while (true) { val n = input.read(bytes); if (n < 0) break; sha.update(bytes, 0, n) }
        }
        return sha.digest().toList()
    }

    private fun writeAtomic(file: File, value: String) {
        val partial = File.createTempFile(".config-", ".part", file.parentFile)
        try {
            partial.writeText(value)
            Files.move(partial.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        } finally { partial.delete() }
    }
}
