package app.gamenative.xrgame

/**
 * One-shot launch arguments for headless device tests. The debug DebugBus `launch <appId>
 * load=<save>` sets them, and the next launch of that game's container appends them once.
 * Nothing is persisted, so the container's own launch arguments stay as the user set them.
 * Such a launch also answers the game's "press Trigger" prompt by itself ([autoTrigger]) unless
 * the request turned that off; any later launch that takes no pending arguments clears it.
 */
object XrGameDebugLaunch {
    private class Pending(val containerId: String, val arguments: String, val autoTrigger: Boolean)

    // Source 2 save names below the game's save directory, e.g. `s0/autosave`.
    private val SAVE = Regex("[A-Za-z0-9_]{1,32}(/[A-Za-z0-9_]{1,32})?")

    @Volatile private var pending: Pending? = null

    /** Whether the running launch came from [loadSave] with the automatic trigger on. */
    @Volatile var autoTrigger: Boolean = false
        private set

    fun isSaveName(name: String): Boolean = SAVE.matches(name)

    /**
     * The next launch of Steam app [appId] loads [save] (`+load <save>`) instead of stopping at the
     * menu, and with [autoTrigger] presses the trigger when the game asks for it after loading.
     */
    @Synchronized
    fun loadSave(appId: Int, save: String, autoTrigger: Boolean = true) {
        require(isSaveName(save)) { "invalid_save" }
        pending = Pending("STEAM_$appId", "+load $save", autoTrigger)
    }

    fun pendingArguments(): String? = pending?.arguments

    /** The pending arguments with a leading space if they belong to [containerId], consumed; else "". */
    @Synchronized
    fun take(containerId: String): String {
        val current = pending?.takeIf { it.containerId == containerId }
        autoTrigger = current?.autoTrigger ?: false
        if (current == null) return ""
        pending = null
        return " " + current.arguments
    }
}
