package app.gamenative.xrgame

import com.winlator.core.KeyValueSet

/**
 * Video memory a D3D game is told it has: DXVK's `dxgi.maxDeviceMemory` (also used by D3D12 through
 * DXVK's dxgi), kept in the container's dxwrapperConfig as `maxDeviceMemory` in MiB. Adreno's single
 * unified heap would otherwise report most of the system memory, and games size their texture
 * budgets from it. XRGame keeps it between 2 and 8 GiB; an unset or zero value (no limit upstream)
 * means the 4 GiB default.
 */
object XrGameMemoryBudget {
    const val KEY = "maxDeviceMemory"
    const val MIN_MB = 2048
    const val MAX_MB = 8192
    const val DEFAULT_MB = 4096
    val STEPS_MB = listOf(2048, 3072, 4096, 6144, 8192)

    /** The budget in MiB a stored `maxDeviceMemory` value stands for. */
    fun effectiveMb(stored: String?): Int {
        val value = stored?.trim()?.toIntOrNull() ?: 0
        return if (value <= 0) DEFAULT_MB else value.coerceIn(MIN_MB, MAX_MB)
    }

    fun fromConfig(dxwrapperConfig: String): Int = effectiveMb(KeyValueSet(dxwrapperConfig).get(KEY))

    fun withBudget(dxwrapperConfig: String, mb: Int): String =
        KeyValueSet(dxwrapperConfig).put(KEY, mb.coerceIn(MIN_MB, MAX_MB)).toString()
}
