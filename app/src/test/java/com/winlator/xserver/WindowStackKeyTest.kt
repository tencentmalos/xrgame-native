package com.winlator.xserver

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import java.nio.ByteBuffer
import java.nio.ByteOrder

// Window properties live in android.util.SparseArray.
@RunWith(RobolectricTestRunner::class)
class WindowStackKeyTest {
    private val atom = Atom.internAtom("_XRGAME_STACK_KEY")

    private fun window(id: Int, vararg key: Int) = Window(id, null, 0, 0, 1280, 720, null).apply {
        if (key.isNotEmpty()) {
            val data = ByteBuffer.allocate(key.size * 4).order(ByteOrder.LITTLE_ENDIAN)
            key.forEach { data.putInt(it) }
            addProperty(Property(atom, Atom.getId("CARDINAL"), Property.Format.INT_ARRAY, data.array()))
        }
    }

    @Test fun `surfaces follow their stacking on the other server, above this server's windows`() {
        val root = window(1)
        val xephyr = window(2)
        val popup = window(3, 1, 2, 1)
        val later = window(4, 1, 3)
        val page = window(5, 1, 2)
        listOf(xephyr, popup, later, page).forEach(root::addChild)

        val moved = root.restackKeyedChildren()

        assertEquals(listOf(xephyr, page, popup, later), root.children)
        assertEquals(listOf(page, popup, later), moved)
    }

    @Test fun `a window of this server created later goes below the surfaces`() {
        val root = window(1)
        val page = window(2, 1, 2)
        val xephyr = window(3)
        listOf(page, xephyr).forEach(root::addChild)

        root.restackKeyedChildren()

        assertEquals(listOf(xephyr, page), root.children)
        assertTrue(root.restackKeyedChildren().isEmpty())
    }

    @Test fun `keys compare unsigned and a prefix sorts first`() {
        assertTrue(Window.compareStackKeys(intArrayOf(1), intArrayOf(1, 0)) < 0)
        assertTrue(Window.compareStackKeys(intArrayOf(2), intArrayOf(1, 9)) > 0)
        assertTrue(Window.compareStackKeys(intArrayOf(-1), intArrayOf(1)) > 0)
    }
}
