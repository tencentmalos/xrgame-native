package app.gamenative.xrgame

/**
 * One-shot launch arguments for headless device tests. The debug DebugBus `launch <appId>
 * load=<save>` sets them, and the next launch of that game's container appends them once.
 * Nothing is persisted, so the container's own launch arguments stay as the user set them.
 */
object XrGameDebugLaunch {
    private class Pending(val containerId: String, val arguments: String)

    // Source 2 save names below the game's save directory, e.g. `s0/autosave`.
    private val SAVE = Regex("[A-Za-z0-9_]{1,32}(/[A-Za-z0-9_]{1,32})?")

    @Volatile private var pending: Pending? = null

    fun isSaveName(name: String): Boolean = SAVE.matches(name)

    /** The next launch of Steam app [appId] loads [save] (`+load <save>`) instead of stopping at the menu. */
    @Synchronized
    fun loadSave(appId: Int, save: String) {
        require(isSaveName(save)) { "invalid_save" }
        pending = Pending("STEAM_$appId", "+load $save")
    }

    fun pendingArguments(): String? = pending?.arguments

    /** The pending arguments with a leading space if they belong to [containerId], consumed; else "". */
    @Synchronized
    fun take(containerId: String): String {
        val current = pending ?: return ""
        if (current.containerId != containerId) return ""
        pending = null
        return " " + current.arguments
    }
}
