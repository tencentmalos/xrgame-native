// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Context
import android.system.Os
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.zip.GZIPInputStream
import java.util.concurrent.TimeUnit

/** A separate userspace for the official client. Games keep the existing Bionic Wine runtime. */
class LinuxSteamRuntime(private val context: Context) {
    val directory = File(context.filesDir, "linux-steam")
    val home = File(directory, "home")
    val client = File(home, "Steam")
    val root = File(directory, "root")
    val logs = File(directory, "logs")

    fun prepare(gpu: Boolean = false) {
        directory.mkdirs(); home.mkdirs(); logs.mkdirs()
        val identity = File(directory, "machine-id")
        if (!identity.isFile) {
            val existing = File(root, "etc/machine-id").takeIf { it.isFile }?.readText()?.trim()
            identity.writeText((existing?.takeIf { it.matches(Regex("[0-9a-f]{32}")) }
                ?: java.util.UUID.randomUUID().toString().replace("-", "")) + "\n")
        }
        val manifest = JSONObject(context.assets.open("linux-steam/manifest.json").bufferedReader().use { it.readText() })
        val expected = manifest.getJSONObject("files").getString("assets/linux-steam/rootfs.tgz")
        val marker = File(root, ".steampsp-rootfs")
        if (!marker.isFile || marker.readText().trim() != expected) {
            // Called by the service before it creates any child. Client/account
            // data lives outside this replaceable, hash-verified userspace.
            check(!LinuxSteamService.running) { "Stop Steam before updating its userspace" }
            val stage = File(directory, "root.installing")
            stage.deleteRecursively(); check(stage.mkdirs())
            try {
                val archive = File(directory, "rootfs.installing.tar.gz")
                check(copyAsset("linux-steam/rootfs.tgz", archive) == expected) { "Linux userspace checksum mismatch" }
                extractRoot(archive, stage)
                archive.delete()
                File(stage, ".steampsp-rootfs").writeText(expected)
                val previous = File(directory, "root.previous")
                check(!previous.exists() || previous.deleteRecursively())
                val replaced = root.exists()
                if (replaced) check(root.renameTo(previous))
                if (!stage.renameTo(root)) {
                    if (replaced) check(previous.renameTo(root))
                    error("Could not activate the Linux Steam userspace")
                }
                previous.deleteRecursively()
            } catch (e: Exception) { stage.deleteRecursively(); throw e }
        }
        if (gpu) installGpu(manifest)
        for (name in listOf("tmp/.X11-unix", "tmp/shm", "proc", "sys", "dev", "run/user/${android.os.Process.myUid()}")) File(root, name).mkdirs()
        // Real Android identity is shared with the Bionic client; no fake root/UID namespace.
        File(root, "etc/passwd").writeText("root:x:0:0:root:/root:/bin/bash\nsteam:x:${android.os.Process.myUid()}:${android.os.Process.myUid()}:Steam:${home.path}:/bin/bash\n")
        File(root, "etc/hosts").writeText("127.0.0.1 localhost steampsp\n::1 localhost\n")
        File(root, "etc/hostname").writeText("steampsp\n")
        val connectivity = context.getSystemService(android.net.ConnectivityManager::class.java)
        val dns = connectivity.getLinkProperties(connectivity.activeNetwork)?.dnsServers.orEmpty()
        check(dns.isNotEmpty()) { "Connect Android to a network before starting Steam" }
        File(root, "etc/resolv.conf").apply { if (isFile || java.nio.file.Files.isSymbolicLink(toPath())) delete() }
            .writeText(dns.joinToString("\n", postfix = "\n") { "nameserver ${it.hostAddress!!.substringBefore('%')}" })
        for (name in listOf("install-client.py", "launch-client.py", "ime-text.py")) {
            context.assets.open("linux-steam/$name").use { input ->
                File(root, "usr/local/lib/steampsp/$name").outputStream().use { input.copyTo(it) }
            }
        }
        val shim = File(root, "usr/local/lib/libsteampsp-client-compat.so")
        context.assets.open("linux-steam/libsteampsp-client-compat.so").use { input -> shim.outputStream().use { input.copyTo(it) } }
        File(root, "etc/ld.so.preload").writeText("/usr/local/lib/libsteampsp-client-compat.so\n")
        File(root, "etc/machine-id").writeText(identity.readText())
    }

    /** Copies an asset and returns its SHA-256. */
    private fun copyAsset(name: String, target: File): String {
        val sha = MessageDigest.getInstance("SHA-256")
        context.assets.open(name).use { input ->
            target.outputStream().use { output ->
                val bytes = ByteArray(262144)
                while (true) { val n = input.read(bytes); if (n < 0) break; sha.update(bytes, 0, n); output.write(bytes, 0, n) }
            }
        }
        return sha.digest().joinToString("") { "%02x".format(it) }
    }

    /** The GPU component sits in the replaceable userspace, so a userspace update reinstalls it. */
    private fun installGpu(manifest: JSONObject) {
        val expected = manifest.getJSONObject("files").getString("assets/${LinuxSteamGpu.ASSET}")
        val target = File(root, LinuxSteamGpu.GUEST_ROOT.removePrefix("/"))
        val marker = File(target, ".steampsp-gpu")
        if (marker.isFile && marker.readText().trim() == expected) return
        val stage = File(target.parentFile, "${target.name}.installing")
        stage.deleteRecursively(); check(stage.mkdirs())
        try {
            val archive = File(directory, "gpu.installing.tar.gz")
            check(copyAsset(LinuxSteamGpu.ASSET, archive) == expected) { "Steam GPU component checksum mismatch" }
            extractRoot(archive, stage)
            archive.delete()
            File(stage, ".steampsp-gpu").writeText(expected)
            check(!target.exists() || target.deleteRecursively())
            check(stage.renameTo(target)) { "Could not activate the Steam GPU component" }
        } catch (e: Exception) { stage.deleteRecursively(); throw e }
    }

    private fun extractRoot(archive: File, stage: File) {
        val base = stage.canonicalFile.toPath()
        val hardlinks = mutableListOf<Pair<File, File>>()
        var total = 0L
        fun safe(name: String): File {
            require(!name.startsWith('/') && name.split('/').none { it == ".." })
            return File(stage, name).canonicalFile.also { require(it.toPath().startsWith(base)) }
        }
        TarArchiveInputStream(GZIPInputStream(archive.inputStream().buffered())).use { tar ->
            while (true) {
                val entry = tar.nextTarEntry ?: break
                val target = safe(entry.name)
                target.parentFile?.mkdirs()
                when {
                    entry.isDirectory -> check(target.isDirectory || target.mkdirs())
                    entry.isSymbolicLink -> {
                        require(!entry.linkName.startsWith('/'))
                        require(File(target.parentFile, entry.linkName).canonicalFile.toPath().startsWith(base))
                        Os.symlink(entry.linkName, target.path)
                    }
                    entry.isLink -> hardlinks.add(target to safe(entry.linkName))
                    entry.isFile -> {
                        total += entry.size
                        check(total <= 2L * 1024 * 1024 * 1024) { "Linux userspace exceeds size limit" }
                        target.outputStream().use { tar.copyTo(it, 262144) }
                        Os.chmod(target.path, (entry.mode and 0x1ff) or 0x180)
                    }
                    else -> error("Unsupported Linux userspace entry")
                }
            }
        }
        for ((target, source) in hardlinks) { check(source.isFile); source.copyTo(target); target.setExecutable(source.canExecute(), true) }
    }

    fun process(arguments: List<String>, logName: String, gpu: Boolean = false): ProcessBuilder {
        val native = context.applicationInfo.nativeLibraryDir
        val locales = android.content.res.Resources.getSystem().configuration.locales
        // A same-path bind lets the glibc and Bionic clients agree on HOME and Steam IPC paths.
        val args = mutableListOf("$native/libproot.so", "--kill-on-exit", "-r", root.path,
            "-b", "/dev", "-b", "/proc", "-b", "/sys", "-b", context.filesDir.path,
            "-b", "${File(root, "tmp/shm").path}:/dev/shm", "-w", home.path)
        // Android exposes filesDir as /data/user/0, while save symlinks use its
        // canonical /data/data path. PRoot needs both names for the same files;
        // otherwise Steam sees a dangling prefix and treats existing saves as missing.
        val canonicalFiles = context.filesDir.canonicalPath
        if (canonicalFiles != context.filesDir.path) args.addAll(listOf("-b", canonicalFiles))
        // External games remain at their original Android paths; no duplicate install or prefix.
        for (library in LinuxSteamLibrary.roots(context)) {
            if (library.isDirectory && !library.toPath().startsWith(context.filesDir.canonicalFile.toPath())) {
                args.addAll(listOf("-b", library.path))
                val compat = File(library, "steamapps/compatdata")
                // Never hide an independently populated prefix. Empty probe directories are safe.
                check(!compat.walkTopDown().any { it.isFile }) { "A separate Steam prefix exists; shared saves were not replaced." }
                compat.mkdirs()
                args.addAll(listOf("-b", "${SteamSaveSharing.compatData(context).path}:${compat.path}"))
            }
        }
        // Chromium's GPU process exits when libpci can list /proc/bus/pci but not read its
        // devices file, and CEF then draws on SwiftShader. Nothing the app sees is on PCI.
        val pci = File("/proc/bus/pci/devices")
        if (gpu && File("/proc/bus/pci").isDirectory && !pci.canRead()) {
            val empty = File(root, "etc/steampsp/pci-devices").apply { parentFile?.mkdirs(); writeText("") }
            args.addAll(listOf("-b", "${empty.path}:${pci.path}"))
        }
        args.addAll(arguments)
        return ProcessBuilder(args).apply {
            environment().clear()
            environment().putAll(mapOf("PROOT_LOADER" to "$native/libproot-loader.so", "PROOT_TMP_DIR" to context.cacheDir.path,
                "HOME" to home.path, "USER" to "steam", "LOGNAME" to "steam", "PATH" to "/usr/local/bin:/usr/bin:/bin",
                "DISPLAY" to ":0", "LANG" to "C.UTF-8", "TMPDIR" to "/tmp",
                // Keep a valid glibc UTF-8 locale; Steam's -language selects its UI.
                // Fontconfig uses Android's language to choose SC/TC/JP/KR CJK glyphs.
                "FC_LANG" to SteamClientLanguage.fontconfig((0 until locales.size()).map { locales[it] }),
                "XDG_RUNTIME_DIR" to "/run/user/${android.os.Process.myUid()}",
                "STEAM_RUNTIME" to "0", "STEAM_RUNTIME_PREFER_HOST_LIBRARIES" to "1",
                "SDL_VIDEO_X11_XSHM" to "0", "QT_X11_NO_MITSHM" to "1"))
            redirectErrorStream(true); redirectOutput(File(logs, logName))
        }
    }

    fun prepareClientLinks() {
        val links = File(home, ".steam").apply { mkdirs() }
        for ((name, path) in mapOf("root" to "", "steam" to "", "bin64" to "steamrtarm64", "binarm64" to "steamrtarm64",
            "sdk64" to "linuxarm64", "sdkarm64" to "linuxarm64", "sdk32" to "linux32")) {
            val link = File(links, name)
            if (!java.nio.file.Files.exists(link.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) Os.symlink(File(client, path).path, link.path)
        }
        val alias = File(client, "steamrtarm32")
        if (!java.nio.file.Files.exists(alias.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) Os.symlink("steamrtarm64", alias.path)
    }

    /** Valve's client libraries, for its own binaries in PRoot. */
    fun clientLibraryPath() = "${client.path}/steamrtarm64:${client.path}/steamrtarm64/steam-runtime/usr/lib/aarch64-linux-gnu"

    /**
     * Ask the running client to start a game, as a steam:// link does. Steam's own binary
     * forwards its command line through ~/.steam/steam.pipe and exits; -ifrunning never starts
     * a second client. Returns the forwarder's exit code, or -1 if it did not finish.
     */
    fun forwardLaunch(appId: Int): Int {
        require(appId > 0) { "invalid app ID" }
        if (!LinuxSteamService.running) return 6
        val builder = process(listOf("${client.path}/steamrtarm64/steam", "-ifrunning", "steam://rungameid/$appId"),
            "forward-launch.log")
        builder.environment().putAll(mapOf("LD_LIBRARY_PATH" to clientLibraryPath(), "DISPLAY" to ":1"))
        val forwarder = builder.start()
        if (!forwarder.waitFor(30, TimeUnit.SECONDS)) { forwarder.destroyForcibly(); return -1 }
        return forwarder.exitValue()
    }

    /** Return only a bounded result code; never expose account data or Valve's own diagnostics. */
    fun checkSession(expectedSteamId: Long? = null): Int {
        if (!LinuxSteamService.running) return 6
        val args = mutableListOf("${context.applicationInfo.nativeLibraryDir}/libxrgame_steam_session.so",
            File(client, "androidarm64/libsteamclient.so").path)
        val builder = ProcessBuilder(args)
        builder.environment().remove("LD_PRELOAD")
        builder.environment().putAll(mapOf("HOME" to home.path, "TMPDIR" to File(root, "tmp").path,
            "LD_LIBRARY_PATH" to File(client, "androidarm64").path))
        builder.redirectErrorStream(true).redirectOutput(File(logs, "session-check.log"))
        val probe = builder.start()
        probe.outputStream.bufferedWriter().use { it.write(expectedSteamId?.toString().orEmpty() + "\n") }
        if (!probe.waitFor(15, TimeUnit.SECONDS)) { probe.destroyForcibly(); return 9 }
        return probe.exitValue()
    }
}
