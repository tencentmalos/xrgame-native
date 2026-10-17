package app.gamenative.utils

import com.winlator.core.WineUtils
import com.winlator.winhandler.ProcessInfo
import java.io.File
import java.util.Locale

object WineProcessSnapshotHelper {
    private val coreWineProcesses = setOf(
        "wineserver",
        "services",
        "start",
        "winhandler",
        "tabtip",
        "explorer",
        "winedevice",
        "svchost",
    )

    private val systemWindowProcesses = setOf(
        "conhost",
        "wineconsole",
        "winedbg",
        "taskmgr",
        "rundll32",
        "msiexec",
        "winebrowser",
        "steam",
        "steamwebhelper",
    )

    fun isSystemProcessName(name: String): Boolean {
        val normalized = normalizeProcessName(name)
        if (normalized.isEmpty()) return true
        return normalized in buildEssentialProcessAllowlist() || normalized in systemWindowProcesses
    }

    data class Snapshot(val processes: List<ProcessInfo>, val complete: Boolean)

    fun readFromProc(): List<ProcessInfo> = readSnapshot().processes

    /** Unknown/read failures must not look like an empty session to the exit watcher. */
    fun readSnapshot(): Snapshot {
        val procDir = File("/proc")
        val pidDirs = try {
            procDir.listFiles { file -> file.isDirectory && file.name.all { it.isDigit() } } ?: return Snapshot(emptyList(), false)
        } catch (_: Exception) {
            return Snapshot(emptyList(), false)
        }

        val myUid = android.os.Process.myUid()
        val processes = mutableListOf<ProcessInfo>()
        var complete = true

        for (pidDir in pidDirs) {
            try {
                val pid = pidDir.name.toIntOrNull() ?: continue
                if (pid == android.os.Process.myPid()) continue
                // stat needs no access to another app's cmdline. A disappearing PID
                // is normal; an unidentifiable live PID is not proof of game exit.
                val owner = try { android.system.Os.stat(pidDir.path).st_uid } catch (_: Exception) {
                    if (pidDir.exists()) complete = false
                    continue
                }
                if (owner != myUid) continue

                val statusLines = try {
                    File(pidDir, "status").readLines()
                } catch (_: Exception) {
                    if (pidDir.exists()) complete = false
                    emptyList()
                }
                val uid = statusLines
                    .firstOrNull { it.startsWith("Uid:") }
                    ?.trim()
                    ?.split(Regex("\\s+"))
                    ?.getOrNull(1)
                    ?.toIntOrNull()
                if (uid == null) {
                    if (pidDir.exists()) complete = false
                    continue
                }
                if (uid != myUid) continue
                if (statusLines.any { it.startsWith("State:") && it.substringAfter(':').trim().startsWith("Z") }) continue

                val comm = try { File(pidDir, "comm").readText().trim() } catch (_: Exception) { "" }
                val cmdlineBytes = try { File(pidDir, "cmdline").readBytes() } catch (_: Exception) {
                    if (pidDir.exists()) complete = false
                    ByteArray(0)
                }
                val cmdlineArgs = cmdlineBytes.toNullSeparatedStrings()
                if (cmdlineArgs.isEmpty()) {
                    if (pidDir.exists()) complete = false
                    continue
                }

                if (!com.winlator.core.ProcessHelper.isWineCommandLine(String(cmdlineBytes, Charsets.UTF_8))) {
                    continue
                }

                val name = cmdlineArgs
                    .firstOrNull { it.endsWith(".exe", ignoreCase = true) }
                    ?: cmdlineArgs.firstOrNull { it.contains(".exe", ignoreCase = true) }
                    ?: cmdlineArgs.firstOrNull { it.contains("wine", ignoreCase = true) }
                    ?: comm.ifBlank { null }
                    ?: continue

                val rssBytes = statusLines
                    .firstOrNull { it.startsWith("VmRSS:") }
                    ?.trim()
                    ?.split(Regex("\\s+"))
                    ?.getOrNull(1)
                    ?.toLongOrNull()
                    ?.times(1024L)
                    ?: 0L

                processes.add(
                    ProcessInfo(
                        pid,
                        name.substringAfterLast('/').substringAfterLast('\\'),
                        rssBytes,
                        0,
                        false,
                    ),
                )
            } catch (_: Exception) {
                // Process exited or became unreadable while enumerating /proc.
                if (pidDir.exists()) complete = false
            }
        }

        val allowlist = buildEssentialProcessAllowlist()
        return Snapshot(processes.sortedWith(
            compareByDescending<ProcessInfo> { normalizeProcessName(it.name) !in allowlist }
                .thenByDescending { it.memoryUsage },
        ), complete)
    }

    private fun buildEssentialProcessAllowlist(): Set<String> {
        val essentialServices = WineUtils.getEssentialServiceNames()
            .map { normalizeProcessName(it) }
        return (essentialServices + coreWineProcesses).toSet()
    }

    private fun normalizeProcessName(name: String): String {
        val trimmed = name.trim().trim('"')
        val base = trimmed.substringAfterLast('/').substringAfterLast('\\')
        val lower = base.lowercase(Locale.ROOT)
        return if (lower.endsWith(".exe")) lower.removeSuffix(".exe") else lower
    }

    private fun ByteArray.toNullSeparatedStrings(): List<String> {
        if (isEmpty()) return emptyList()

        val result = mutableListOf<String>()
        var start = 0
        for (i in indices) {
            if (this[i] == 0.toByte()) {
                if (i > start) {
                    val value = String(this, start, i - start).trim()
                    if (value.isNotBlank()) result.add(value)
                }
                start = i + 1
            }
        }
        if (start < size) {
            val value = String(this, start, size - start).trim()
            if (value.isNotBlank()) result.add(value)
        }
        return result
    }
}
