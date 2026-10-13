package app.gamenative.xrgame

import android.view.InputDevice
import com.winlator.inputcontrols.ExternalController

/**
 * Whether a real mouse is connected. XRGame shows the game's pointer only then; touch, gamepads
 * and the XR controllers have no use for it. Pico's virtual inputs announce a mouse source for the
 * XR controllers, and controllers bring touchpad and motion sub-devices, so neither counts.
 */
object XrGamePhysicalMouse {
    fun connected(): Boolean = InputDevice.getDeviceIds().any { id -> InputDevice.getDevice(id)?.let(::isMouse) == true }

    fun isMouse(device: InputDevice): Boolean {
        val name = device.name.orEmpty()
        return !device.isVirtual &&
            device.supportsSource(InputDevice.SOURCE_MOUSE) &&
            !name.startsWith("pico-virtual-input") &&
            !name.startsWith("pvr-virtual-input") &&
            !name.contains("Controller", ignoreCase = true) &&
            !ExternalController.isGameController(device)
    }
}
