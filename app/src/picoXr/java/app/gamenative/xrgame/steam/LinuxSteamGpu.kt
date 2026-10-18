// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Context
import java.io.File

/**
 * Experimental GPU drawing for the official client, off by default. Steam's web helper (CEF's
 * GPU process and Steam's compositor) draws with a glibc Zink on Turnip; each of its windows
 * presents to the client's Android X server with DRI3/Present and host-allocated hardware
 * buffers, as games do, stacked as in Xephyr, which still owns windows, input and fonts.
 * Without the bundled component the client keeps CEF software drawing.
 */
object LinuxSteamGpu {
    const val ASSET = "linux-steam/gpu.tgz"
    /** Fixed guest path; the ICD JSON in the component names its library there. */
    const val GUEST_ROOT = "/opt/steampsp-gpu"

    private fun marker(context: Context) = File(context.filesDir, "linux-steam/gpu-display")

    fun bundled(context: Context): Boolean =
        runCatching { context.assets.open(ASSET).close(); true }.getOrDefault(false)

    fun requested(context: Context) = marker(context).isFile

    fun enabled(context: Context) = requested(context) && bundled(context)

    /** Takes effect at the next client start. */
    fun request(context: Context, on: Boolean) {
        val file = marker(context)
        if (on) file.apply { parentFile?.mkdirs() }.writeText("on\n") else file.delete()
    }

    /** Steam forwards these two as --ignore-gpu-blocklist and --disable-gpu(-compositing). */
    fun cefArguments(gpu: Boolean): List<String> =
        if (gpu) listOf("-cef-force-gpu") else listOf("-cef-disable-gpu")
}
