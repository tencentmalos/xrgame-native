// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from shadPS4 85a39824d9c7; clamshell neutral-frame calibration.
package app.gamenative.ui.screen.xr.sbs

import kotlin.math.sqrt

/** Android sensors stay in natural-device coordinates. PSVR is right-handed:
 * X right (pitch), Y up (yaw), Z towards the viewer (roll), forward -Z.
 */
internal object VrGyroAxes {
    // Same basis as SensorManager.remapCoordinateSystem: ROTATION_90 uses
    // AXIS_Y / AXIS_MINUS_X. This is a vector basis change, not a screen rotation.
    fun toDisplay(x: Float, y: Float, z: Float, rotation: Int): FloatArray = when (rotation) {
        1 -> floatArrayOf(y, -x, z)
        2 -> floatArrayOf(-x, -y, z)
        3 -> floatArrayOf(-y, x, z)
        else -> floatArrayOf(x, y, z)
    }

    /** One neutral basis per input session. A clamshell's sensor may sit in a
     * nearly horizontal base even when its display faces the player. Screen Z
     * alone therefore cannot distinguish horizontal turning from head roll.
     * Calibrate up from gravity and right from the projected screen-right axis.
     * Freeze the basis: following gravity every frame would erase real pitch/roll.
     */
    class NeutralFrame {
        private var up: FloatArray? = null
        private var right: FloatArray? = null
        private var back: FloatArray? = null
        private var rotation = 0
        private var candidate: FloatArray? = null
        private var stableSamples = 0
        val calibrated: Boolean get() = up != null

        fun observeGravity(x: Float, y: Float, z: Float, displayRotation: Int): Boolean {
            if (calibrated) return false
            val g = toDisplay(x, y, z, displayRotation)
            if (!g.all { it.isFinite() }) return false
            val length = sqrt(dot(g, g))
            // Reject free fall and strong acceleration (accelerometer fallback).
            if (length !in 8f..12f) { candidate = null; stableSamples = 0; return false }
            for (i in 0..2) g[i] /= length
            val old = candidate
            if (old == null || rotation != displayRotation || dot(old, g) < 0.995f) {
                candidate = g; stableSamples = 1; rotation = displayRotation
                return false
            }
            if (++stableSamples < 4) return false
            // A vertical screen-right axis has no horizontal projection. Keep
            // the neutral pose until the device is held in a usable orientation.
            val r = floatArrayOf(1f - g[0]*g[0], -g[0]*g[1], -g[0]*g[2])
            val rLength = sqrt(dot(r, r))
            if (rLength < 0.2f) return false
            for (i in 0..2) r[i] /= rLength
            right = r
            up = g
            back = floatArrayOf(r[1]*g[2]-r[2]*g[1], r[2]*g[0]-r[0]*g[2], r[0]*g[1]-r[1]*g[0])
            return true
        }

        fun toHead(x: Float, y: Float, z: Float): FloatArray? {
            val u = up ?: return null
            val v = toDisplay(x, y, z, rotation)
            return floatArrayOf(dot(right!!, v), dot(u, v), dot(back!!, v))
        }

        private fun dot(a: FloatArray, b: FloatArray) = a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
    }
}
