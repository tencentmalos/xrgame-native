package app.gamenative.xrgame

import app.gamenative.ui.screen.xr.windows.WindowsVrUpscale
import com.winlator.core.envvars.EnvVars

/**
 * A cap on a game's MSAA, like shadPS4's MSAA override: the bundled Turnip then creates
 * multisampled images, render passes and rasterization with at most that many samples
 * (`TU_MAX_SAMPLES`, patches/turnip-max-samples.patch). Turnip renders a pass with a fragment
 * density map only when none of its multisampled attachments is loaded or stored, so a VR game
 * that foveates itself (Half-Life: Alyx on Vulkan) gets MSAA off while eye-tracked foveation is on.
 */
object XrGameMsaa {
    const val ENV = "XRGAME_MSAA"
    const val TURNIP_ENV = "TU_MAX_SAMPLES"

    enum class Mode(val key: String, val samples: Int) { GAME("game", 0), OFF("off", 1), X2("2x", 2) }

    /** Turnip builds carrying patches/turnip-max-samples.patch. */
    private val MAX_SAMPLES_TURNIP = setOf("turnip-adb7e30-xrg15", "turnip-adb7e30-xrg16", "turnip-55186f1-xrg17")

    fun stored(envVars: String): Mode = Mode.entries.firstOrNull { it.key == EnvVars(envVars).get(ENV) } ?: Mode.GAME

    fun withMode(envVars: String, mode: Mode): String = EnvVars(envVars).apply {
        if (mode == Mode.GAME) remove(ENV) else put(ENV, mode.key)
    }.toString()

    /** A VR game gets the eye gaze while eye-tracked foveation is on, and needs single-sampled passes. */
    fun forcedOff(envVars: String, vrGame: Boolean): Boolean =
        vrGame && WindowsVrUpscale.fromEnvVars(envVars).foveation == WindowsVrUpscale.Foveation.EYE

    fun effective(envVars: String, vrGame: Boolean): Mode = if (forcedOff(envVars, vrGame)) Mode.OFF else stored(envVars)

    fun configure(env: EnvVars, vrGame: Boolean, turnipVersion: String = XrGameRuntimeVersions.TURNIP) {
        env.remove(TURNIP_ENV)
        if (turnipVersion !in MAX_SAMPLES_TURNIP) return
        val mode = effective(env.toString(), vrGame)
        if (mode.samples > 0) env.put(TURNIP_ENV, mode.samples)
    }
}
