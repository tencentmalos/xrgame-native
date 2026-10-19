// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.system.Os
import android.system.OsConstants
import java.io.File

/**
 * The official client's processes. PRoot ignores SIGTERM, and a killed PRoot detaches its tracees
 * instead of ending them, so stopping the PRoot process alone leaves Steam, its web helper and
 * Xephyr running untraced. Stop the whole tree, then sweep any of this client's processes that a
 * killed PRoot (by us, by Android's phantom-process limit or by a crash) left behind.
 */
internal object LinuxSteamProcesses {
    fun pid(process: Process): Int? = runCatching {
        process.javaClass.getDeclaredField("pid").apply { isAccessible = true }.getInt(process)
    }.getOrNull()

    /** pid to parent pid for this app's visible, live (not zombie) processes. */
    private fun parents(): Map<Int, Int> = File("/proc").listFiles().orEmpty().mapNotNull { dir ->
        val pid = dir.name.toIntOrNull() ?: return@mapNotNull null
        val stat = runCatching { File(dir, "stat").readText() }.getOrNull() ?: return@mapNotNull null
        // comm can contain spaces and parentheses; the fields that follow start after the last ')'.
        val fields = stat.substringAfterLast(')').trim().split(' ')
        if (fields.getOrNull(0) == "Z") return@mapNotNull null
        pid to (fields.getOrNull(1)?.toIntOrNull() ?: return@mapNotNull null)
    }.toMap()

    fun descendants(root: Int, table: Map<Int, Int> = parents()): Set<Int> {
        val children = table.entries.groupBy({ it.value }, { it.key })
        val found = linkedSetOf<Int>()
        val queue = ArrayDeque(listOf(root))
        while (queue.isNotEmpty()) children[queue.removeFirst()].orEmpty().forEach { if (found.add(it)) queue.add(it) }
        return found
    }

    /** Processes started inside this client's userspace: they keep its HOME from exec on. */
    fun clientProcesses(home: File): Set<Int> {
        val marker = "HOME=${home.path}".toByteArray()
        val self = android.os.Process.myPid()
        return File("/proc").listFiles().orEmpty().mapNotNull { dir ->
            val pid = dir.name.toIntOrNull()?.takeIf { it != self } ?: return@mapNotNull null
            val environment = runCatching { File(dir, "environ").readBytes() }.getOrNull() ?: return@mapNotNull null
            pid.takeIf { environment.split(0).any { it.contentEquals(marker) } }
        }.toSet()
    }

    private fun ByteArray.split(separator: Byte): List<ByteArray> {
        val parts = mutableListOf<ByteArray>()
        var start = 0
        for (i in indices) if (this[i] == separator) { parts.add(copyOfRange(start, i)); start = i + 1 }
        if (start < size) parts.add(copyOfRange(start, size))
        return parts
    }

    /** SIGKILL a short-lived PRoot helper and everything it started. */
    fun stopTree(process: Process) {
        val root = pid(process)
        if (root == null) { process.destroyForcibly(); return }
        for (attempt in 0 until 3) {
            val victims = descendants(root)
            if (victims.isEmpty()) break
            for (pid in victims) runCatching { Os.kill(pid, OsConstants.SIGKILL) }
        }
        process.destroyForcibly()
    }

    /** SIGKILL [root]'s tree and every leftover client process; returns how many were signalled. */
    fun stop(root: Int?, home: File): Int {
        var signalled = 0
        repeat(5) {
            val table = parents()
            val victims = (root?.let { descendants(it, table) }.orEmpty() + clientProcesses(home))
                .filter { it in table }
            if (victims.isEmpty() && (root == null || root !in table)) return signalled
            // Children first, so a fork racing with this pass is caught on the next one.
            for (pid in victims) runCatching { Os.kill(pid, OsConstants.SIGKILL); signalled++ }
            root?.takeIf { it in table }?.let { runCatching { Os.kill(it, OsConstants.SIGKILL); signalled++ } }
            Thread.sleep(100)
        }
        return signalled
    }
}
