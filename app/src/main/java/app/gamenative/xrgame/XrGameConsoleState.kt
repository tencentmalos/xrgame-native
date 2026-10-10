package app.gamenative.xrgame

import java.io.File
import java.io.RandomAccessFile

/**
 * Scene readiness from a Source 2 `-condebug` console log (Half-Life: Alyx writes
 * `game/hlvr/console.log`): the host state the engine last reached, a request still loading, and
 * whether the game is paused. Only state names, map/save targets and timestamps are reported,
 * because log lines also carry the player's name.
 */
object XrGameConsoleState {
    data class State(
        val hostState: String? = null,
        val target: String? = null,
        val pendingState: String? = null,
        val pendingTarget: String? = null,
        val paused: Boolean = false,
        val launchedAt: String? = null,
        val lastEventAt: String? = null,
    ) {
        /**
         * A save or map other than the startup menu is running, nothing is loading and the game
         * is not paused. Alyx pauses itself a few seconds after every restore until the trigger
         * is pulled, so callers should see this hold for a while before measuring.
         */
        val inScene: Boolean
            get() = pendingState == null && !paused && target != null && target !in MENU_TARGETS &&
                (hostState == "Restoring Save" || hostState == "Loading")
    }

    private val MENU_TARGETS = setOf("startup", "mainmenu")
    private const val TIME = """(\d\d/\d\d \d\d:\d\d:\d\d)"""
    private val COMMAND_LINE = Regex("""^$TIME \[CommandLine\] """)
    private val HOST = Regex(
        """^$TIME \[HostStateManager\] (CHostStateMgr::QueueNewRequest\( )?([A-Za-z ]{1,32}) \(([A-Za-z0-9_/.\-]{0,64})\)""",
    )
    private val PAUSE = Regex("""^$TIME \[Client\] .* (un)?paused the game$""")

    // Bounded scan for the log below an install directory, and the tail that is parsed.
    private const val MAX_DEPTH = 3
    private const val MAX_ENTRIES = 20_000
    private const val TAIL_BYTES = 1L shl 20

    fun parse(lines: Sequence<String>): State {
        var state = State()
        for (raw in lines) {
            val line = raw.trimEnd()
            COMMAND_LINE.find(line)?.let { state = State(launchedAt = it.groupValues[1], lastEventAt = it.groupValues[1]) }
            HOST.find(line)?.let { match ->
                val (time, queued, name, target) = match.destructured
                state = if (queued.isNotEmpty()) {
                    state.copy(pendingState = name, pendingTarget = target, lastEventAt = time)
                } else {
                    val stillPending = state.pendingState != null &&
                        (state.pendingState != name || state.pendingTarget != target)
                    state.copy(
                        hostState = name, target = target, paused = false, lastEventAt = time,
                        pendingState = if (stillPending) state.pendingState else null,
                        pendingTarget = if (stillPending) state.pendingTarget else null,
                    )
                }
            }
            PAUSE.find(line)?.let { match ->
                state = state.copy(paused = match.groupValues[2].isEmpty(), lastEventAt = match.groupValues[1])
            }
        }
        return state
    }

    /** The most recently written `console.log` up to three levels below [installDir], or null. */
    fun findLog(installDir: File): File? {
        if (!installDir.isDirectory) return null
        return installDir.walkTopDown().maxDepth(MAX_DEPTH).take(MAX_ENTRIES)
            .filter { it.isFile && it.name == "console.log" }
            .maxByOrNull { it.lastModified() }
    }

    /** Parses the last [TAIL_BYTES] of [log]; a partial first line is dropped. */
    fun read(log: File): State {
        val text = RandomAccessFile(log, "r").use { file ->
            val start = (file.length() - TAIL_BYTES).coerceAtLeast(0L)
            file.seek(start)
            val bytes = ByteArray((file.length() - start).toInt())
            file.readFully(bytes)
            String(bytes, Charsets.UTF_8).let { if (start > 0) it.substringAfter('\n') else it }
        }
        return parse(text.lineSequence())
    }
}
