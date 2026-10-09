package app.gamenative.ui.screen.xr.windows

import app.gamenative.ui.screen.xr.XrNative

/**
 * Controller pose correction for Windows VR games.
 *
 * The bridge reports every controller to Windows as an Oculus Touch, and OpenComposite turns the
 * OpenXR grip pose into the OpenVR pose with the Quest 2 controller's model offsets; games then
 * add their own Touch pointing offset. A headset whose controller relates grip and pointing
 * differently (Swan) gets a ray that does not follow the physical controller. This rotates and
 * shifts the grip pose sent to Windows (right-hand frame, left mirrored) so the game's ray lines
 * up again. Only Windows games see it; the immersive quad pointer keeps the runtime's aim pose.
 * DebugBus `vr_grip` tunes it live.
 */
object WindowsVrGripCorrection {
    data class Settings(
        val pitch: Float = 0f,
        val yaw: Float = 0f,
        val roll: Float = 0f,
        val xMm: Float = 0f,
        val yMm: Float = 0f,
        val zMm: Float = 0f,
    )

    /**
     * Per-device defaults by android.os.Build.DEVICE, calibrated on the device.
     *
     * swan (2026-10-09, Half-Life: Alyx through OpenComposite): the Pico runtime places the aim
     * pose 34.0° below the grip pose, while the Quest 2 offsets OpenComposite and the game apply
     * put the ray about 58° below it, so the game's ray pointed roughly 24-30° under the
     * controller. Raising the grip by 24° matches the analytic difference.
     */
    private val DEVICE_DEFAULTS = mapOf(
        "swan" to Settings(pitch = 24f),
    )

    fun defaultFor(device: String): Settings = DEVICE_DEFAULTS[device] ?: Settings()

    @Volatile var settings: Settings = Settings()
        private set

    @Volatile var device: String = ""
        private set

    /** Native switch; replaced in unit tests. */
    internal var sink: (Settings) -> Unit = {
        XrNative.nativeSetGripCorrection(it.pitch, it.yaw, it.roll, it.xMm / 1000f, it.yMm / 1000f, it.zMm / 1000f)
    }

    @Synchronized
    fun set(newSettings: Settings) {
        sink(newSettings)
        settings = newSettings
    }

    /** Applies the device default at session start. */
    @Synchronized
    fun reset(deviceName: String) {
        device = deviceName
        set(defaultFor(deviceName))
    }

    /**
     * Applies `pitch=`, `yaw=`, `roll=` (degrees, -90..90), `x=`, `y=`, `z=` (millimetres,
     * -100..100) and `reset=1` (the device default first). Returns an error token, or null when
     * every argument was valid (then all of them are applied).
     */
    @Synchronized
    fun apply(arguments: List<String>): String? {
        var next = settings
        for (argument in arguments) {
            val separator = argument.indexOf('=')
            if (separator <= 0) return "malformed_argument"
            val value = argument.substring(separator + 1)
            val number = value.toFloatOrNull()
            next = when (argument.substring(0, separator)) {
                "pitch" -> next.copy(pitch = number?.takeIf { it in -90f..90f } ?: return "invalid_pitch")
                "yaw" -> next.copy(yaw = number?.takeIf { it in -90f..90f } ?: return "invalid_yaw")
                "roll" -> next.copy(roll = number?.takeIf { it in -90f..90f } ?: return "invalid_roll")
                "x" -> next.copy(xMm = number?.takeIf { it in -100f..100f } ?: return "invalid_x")
                "y" -> next.copy(yMm = number?.takeIf { it in -100f..100f } ?: return "invalid_y")
                "z" -> next.copy(zMm = number?.takeIf { it in -100f..100f } ?: return "invalid_z")
                "reset" -> if (value == "1") defaultFor(device) else return "invalid_reset"
                else -> return "unknown_argument"
            }
        }
        if (next != settings) set(next)
        return null
    }
}
