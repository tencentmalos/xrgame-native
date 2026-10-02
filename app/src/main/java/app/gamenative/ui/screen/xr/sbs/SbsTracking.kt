// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Gyro integration adapted from shadPS4 guest_vr_sensor.cpp (85a39824d9c7).
package app.gamenative.ui.screen.xr.sbs

import kotlin.math.*

/** Synthetic position + display-aligned inertial orientation. No physical 6DoF tracking. */
class SbsTracking {
    private var previousNs = 0L
    var orientation = floatArrayOf(0f, 0f, 0f, 1f)
        private set
    var recenterSerial = 0L
        private set

    fun recenter() {
        orientation = floatArrayOf(0f, 0f, 0f, 1f)
        previousNs = 0L
        recenterSerial++
    }

    fun suspendSampling() { previousNs = 0L }

    fun gyro(x: Float, y: Float, z: Float, timestampNs: Long) {
        if (!x.isFinite() || !y.isFinite() || !z.isFinite() || timestampNs <= previousNs || timestampNs <= 0) return
        val previous = previousNs
        previousNs = timestampNs
        if (previous == 0L || timestampNs - previous > 100_000_000L) return
        val speed = sqrt(x.toDouble()*x + y.toDouble()*y + z.toDouble()*z)
        if (speed == 0.0) return
        val halfAngle = speed * (timestampNs - previous) * 0.5e-9
        val scale = sin(halfAngle) / speed
        orientation = multiply(orientation, floatArrayOf(
            (x*scale).toFloat(), (y*scale).toFloat(), (z*scale).toFloat(), cos(halfAngle).toFloat(),
        ))
    }

    companion object {
        fun multiply(a: FloatArray, b: FloatArray): FloatArray {
            val q = floatArrayOf(
                a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
                a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
                a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
                a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2],
            )
            val norm = sqrt(q.sumOf { it.toDouble()*it }).toFloat()
            return if (norm.isFinite() && norm > 0f) q.map { it/norm }.toFloatArray()
                else floatArrayOf(0f, 0f, 0f, 1f)
        }
        fun rotate(q: FloatArray, x: Float, y: Float, z: Float): FloatArray {
            val tx = 2*(q[1]*z-q[2]*y)
            val ty = 2*(q[2]*x-q[0]*z)
            val tz = 2*(q[0]*y-q[1]*x)
            return floatArrayOf(x+q[3]*tx+q[1]*tz-q[2]*ty,
                y+q[3]*ty+q[2]*tx-q[0]*tz, z+q[3]*tz+q[0]*ty-q[1]*tx)
        }
        fun aim(yaw: Float, pitch: Float): FloatArray = multiply(
            floatArrayOf(0f, sin(yaw/2), 0f, cos(yaw/2)),
            floatArrayOf(sin(pitch/2), 0f, 0f, cos(pitch/2)),
        )
    }
}
