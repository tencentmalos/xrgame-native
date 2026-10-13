package app.gamenative.xrgame

import com.winlator.container.ContainerData

/**
 * The launch panel's display modes and how each maps onto the container fields that pick the
 * runtime path, differently for ordinary and VR games:
 *  - 2D: a window (on a headset, Pico's 2D app panel). VR games cannot run in it.
 *  - SBS: ordinary games in the SBS cinema; VR games side by side with simulated tracking.
 *  - XR: headsets only. Ordinary games on the 2D big screen; VR games in an OpenXR session.
 * The choice is kept in [ContainerData.xrDisplayMode]. Containers from before the panel derive it
 * from their fields, and a headset starts a game that was never set to anything in XR. XR caps
 * the game resolution at 1920x1080 ([XrGameResolution]).
 */
object XrGameLaunchMode {
    enum class Mode(val key: String) { FLAT("2d"), SBS("sbs"), XR("xr") }

    fun available(headset: Boolean): List<Mode> = if (headset) Mode.entries else listOf(Mode.FLAT, Mode.SBS)

    /** Whether [mode] exists on this device and can run this kind of game. */
    fun allowed(mode: Mode, vrGame: Boolean, headset: Boolean): Boolean =
        mode in available(headset) && !(vrGame && mode == Mode.FLAT)

    /** The nearest mode this device and game allow. */
    fun fallback(mode: Mode, vrGame: Boolean, headset: Boolean): Mode = when {
        allowed(mode, vrGame, headset) -> mode
        vrGame -> if (headset) Mode.XR else Mode.SBS
        else -> Mode.FLAT
    }

    fun current(data: ContainerData, headset: Boolean): Mode {
        val derived = when {
            data.windowsVrEnabled -> if (data.xrPresentationMode == "openxr") Mode.XR else Mode.SBS
            data.sbsTheaterEnabled -> Mode.SBS
            data.launchImmersiveMode -> Mode.XR
            else -> Mode.FLAT
        }
        val chosen = Mode.entries.firstOrNull { it.key == data.xrDisplayMode }
            ?: if (headset && derived == Mode.FLAT) Mode.XR else derived
        return fallback(chosen, data.windowsVrEnabled, headset)
    }

    /** [data] with the fields of [mode] for an ordinary or a VR game. */
    fun apply(data: ContainerData, mode: Mode, vrGame: Boolean): ContainerData = applyFields(
        if (mode == Mode.XR) data.copy(screenSize = XrGameResolution.capped(data.screenSize)) else data,
        mode,
        vrGame,
    )

    private fun applyFields(data: ContainerData, mode: Mode, vrGame: Boolean): ContainerData = if (vrGame) {
        data.copy(
            windowsVrEnabled = true,
            sbsTheaterEnabled = false,
            launchImmersiveMode = false,
            xrPresentationMode = if (mode == Mode.XR) "openxr" else "sbs",
            xrDisplayMode = mode.key,
        )
    } else {
        data.copy(
            windowsVrEnabled = false,
            sbsTheaterEnabled = mode == Mode.SBS,
            launchImmersiveMode = mode == Mode.XR,
            xrPresentationMode = "sbs",
            xrDisplayMode = mode.key,
        )
    }
}
