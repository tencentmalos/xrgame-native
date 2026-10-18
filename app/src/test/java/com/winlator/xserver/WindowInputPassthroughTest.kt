package com.winlator.xserver

import org.junit.Assert.assertSame
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

// Window properties live in android.util.SparseArray.
@RunWith(RobolectricTestRunner::class)
class WindowInputPassthroughTest {
    private fun mapped(id: Int) = Window(id, null, 0, 0, 1280, 720, null).apply { attributes.setMapped(true) }

    @Test fun `pointer input skips a window that only shows GPU frames`() {
        val root = mapped(1)
        val desktop = mapped(2)
        val frames = mapped(3)
        root.addChild(desktop)
        root.addChild(frames)
        assertSame(frames, root.getChildByCoords(10, 10))

        val atom = Atom.internAtom("_XRGAME_INPUT_PASSTHROUGH")
        frames.addProperty(Property(atom, Atom.getId("CARDINAL"), Property.Format.INT_ARRAY, byteArrayOf(1, 0, 0, 0)))
        assertSame(desktop, root.getChildByCoords(10, 10))

        frames.removeProperty(atom)
        assertSame(frames, root.getChildByCoords(10, 10))
    }
}
