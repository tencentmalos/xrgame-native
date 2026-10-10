package app.gamenative.xrgame

import app.gamenative.data.UserFileInfo
import app.gamenative.enums.PathType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class XrGameCloudRpcTest {
    private fun file(name: String) = UserFileInfo(PathType.GameInstall, "game/hlvr/save/", name, 0L, ByteArray(20))

    private val synced = (1..11).map { file("f$it") }

    @Test fun transientFilesRemovedByTheGameAreDeleted() {
        val deleted = listOf(file("a1_intro_world.hl1"), file("a1_intro_world.hl2"), file("a1_intro_world.hl3"))
        assertTrue(XrGameCloudRpc.allowDeletes(deleted, synced.take(8), synced))
    }

    @Test fun emptyScanOrMassLossKeepsCloudFiles() {
        assertFalse(XrGameCloudRpc.allowDeletes(synced.take(3), emptyList(), synced))
        assertFalse(XrGameCloudRpc.allowDeletes(synced.take(6), synced.take(5), synced))
        val many = (1..40).map { file("g$it") }
        assertFalse(XrGameCloudRpc.allowDeletes(many.take(9), many.drop(9), many))
    }

    @Test fun nothingToDeleteIsAlwaysAllowed() {
        assertTrue(XrGameCloudRpc.allowDeletes(emptyList(), emptyList(), emptyList()))
    }

    @Test fun blockMethodsFollowEHttpMethod() {
        assertEquals("PUT", XrGameCloudRpc.methodName(0))
        assertEquals("POST", XrGameCloudRpc.methodName(3))
        assertEquals("PUT", XrGameCloudRpc.methodName(4))
        assertEquals("GET", XrGameCloudRpc.methodName(1))
    }
}
