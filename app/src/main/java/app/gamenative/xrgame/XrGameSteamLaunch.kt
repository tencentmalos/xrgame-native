package app.gamenative.xrgame

import java.io.File
import java.io.RandomAccessFile

/** Resolve Steam metadata first, with narrowly scoped compatibility overrides. */
object XrGameSteamLaunch {
    data class Launch(val command: String, val executable: File, val workingDirectory: File, val arguments: String = "")
    data class ClientProfile(val executable: String, val arguments: String, val workingDirectory: String,
                             val injectExtra: Boolean = false, val nestedGamePath: Boolean = false,
                             val followSelfRestart: Boolean = false)

    fun clientProfile(appId: Int, selectedExecutable: String): ClientProfile? {
        val selected = selectedExecutable.replace('\\', '/').lowercase()
        return when {
            appId == 1145350 && selected in setOf("", "release/hades2.exe", "ship/hades2.exe") ->
                ClientProfile("Ship/Hades2.exe", "/c ../", "Ship", injectExtra = true)
            appId == 582010 && selected in setOf("", "monsterhunterworld.exe") ->
                // MHW builds an empty graphics configuration path when launched at a drive root.
                ClientProfile("MonsterHunterWorld.exe", "", "", injectExtra = true, nestedGamePath = true,
                    followSelfRestart = true)
            else -> null
        }
    }

    /** The bundled client is x64. Never inject it into a 32-bit process. */
    fun usesBundledClient(executable: File): Boolean = RandomAccessFile(executable, "r").use { file ->
        if (file.length() < 64 || file.readUnsignedShort() != 0x4d5a) return false
        file.seek(0x3c)
        val offset = Integer.reverseBytes(file.readInt()).toLong() and 0xffffffffL
        if (offset < 64 || offset > file.length() - 6) return false
        file.seek(offset)
        file.readInt() == 0x50450000 && (java.lang.Short.reverseBytes(file.readShort()).toInt() and 0xffff) == 0x8664
    }

    fun resolve(
        directory: File,
        drive: Char,
        selectedExecutable: String,
        manifestExecutable: String,
        manifestArguments: String,
        manifestWorkingDirectory: String,
    ): Launch {
        require(drive.uppercaseChar() in 'A'..'Z') { "Game drive is missing" }
        fun relative(value: String): String = value.replace('\\', '/').trim().removePrefix("./")
        val exe = relative(selectedExecutable.ifBlank { manifestExecutable })
        require(exe.isNotBlank() && !exe.startsWith('/') && ':' !in exe && '"' !in exe && exe.none { it.isISOControl() }) {
            "Steam executable must be relative to the installed game"
        }
        val root = directory.canonicalFile
        val executable = File(root, exe).canonicalFile
        require(executable.toPath().startsWith(root.toPath()) && executable != root) {
            "Steam executable escapes the installed game"
        }
        val matchesManifest = exe.equals(relative(manifestExecutable), ignoreCase = true)
        val work = if (matchesManifest && manifestWorkingDirectory.isNotBlank()) {
            relative(manifestWorkingDirectory)
        } else exe.substringBeforeLast('/', "")
        require(!work.startsWith('/') && ':' !in work && work.none { it.isISOControl() }) { "Steam working directory must be relative" }
        val workingDirectory = File(root, work).canonicalFile
        require(workingDirectory.toPath().startsWith(root.toPath())) { "Steam working directory escapes the installed game" }
        val path = root.toPath().relativize(executable.toPath()).toString().replace('/', '\\')
        val arguments = if (matchesManifest) manifestArguments.trim() else ""
        return Launch("\"${drive.uppercaseChar()}:\\$path\"" + if (arguments.isEmpty()) "" else " $arguments",
            executable, workingDirectory, arguments)
    }
}
