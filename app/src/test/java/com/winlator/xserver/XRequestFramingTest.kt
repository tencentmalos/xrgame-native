package com.winlator.xserver

import com.winlator.xconnector.Client
import com.winlator.xconnector.XInputStream
import com.winlator.xconnector.XOutputStream
import com.winlator.xconnector.XStreamLock
import org.junit.Assert.*
import org.junit.Test
import org.mockito.Answers.CALLS_REAL_METHODS
import org.mockito.kotlin.mock
import org.mockito.kotlin.whenever
import java.nio.ByteBuffer
import java.nio.ByteOrder

class XRequestFramingTest {
    private fun set(target: Any, owner: Class<*>, name: String, value: Any) {
        owner.getDeclaredField(name).apply { isAccessible = true }.set(target, value)
    }

    private fun replyTo(bytes: ByteBuffer, server: XServer? = null): ByteBuffer {
        bytes.flip()
        val input = XInputStream(128)
        set(input, XInputStream::class.java, "activeBuffer", bytes)
        val output = object : XOutputStream(128) {
            override fun lock(): XStreamLock = XStreamLock {}
        }.apply { setByteOrder(ByteOrder.LITTLE_ENDIAN) }
        val xclient = mock<XClient>(defaultAnswer = CALLS_REAL_METHODS)
        if (server != null) set(xclient, XClient::class.java, "xServer", server)
        set(xclient, XClient::class.java, "inputStream", input)
        set(xclient, XClient::class.java, "outputStream", output)
        xclient.setAuthenticated(true)
        val transport = Client(null, null).apply { tag = xclient }
        set(transport, Client::class.java, "inputStream", input)
        set(transport, Client::class.java, "outputStream", output)
        assertTrue(XClientRequestHandler().handleRequest(transport))
        assertEquals(0, input.available())
        return output.buffer
    }

    private fun colorServer(): Pair<XServer, ColormapManager> {
        val manager = ColormapManager(Visual(1, true, 32, 8, 0xff0000, 0xff00, 0xff))
        val server = mock<XServer>()
        set(server, XServer::class.java, "colormapManager", manager)
        whenever(server.lock(XServer.Lockable.WINDOW_MANAGER)).thenReturn(mock<XLock>())
        return server to manager
    }

    private fun namedColor(opcode: Int, map: Int, name: String): ByteBuffer {
        val data = name.toByteArray(Charsets.ISO_8859_1)
        val length = 12 + ((data.size + 3) and -4)
        return ByteBuffer.allocate(length).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(opcode.toByte()).put(0).putShort((length / 4).toShort())
            putInt(map).putShort(data.size.toShort()).putShort(0).put(data)
            while (hasRemaining()) put(0)
        }
    }

    @Test fun `named colors return TrueColor pixels and exact wire replies`() {
        val (server, manager) = colorServer()
        for ((name, pixel) in listOf("black" to 0, "WhItE" to 0xffffff, "green" to 0x00ff00, "gray50" to 0x808080)) {
            val reply = replyTo(namedColor(85, manager.defaultColormap.id, name), server)
            assertEquals(32, reply.position())
            assertEquals(1, reply.get(0).toInt())
            assertEquals(pixel, reply.getInt(8))
            assertEquals(((pixel shr 16) and 255) * 257, reply.getShort(12).toInt() and 65535)
            assertEquals(((pixel shr 8) and 255) * 257, reply.getShort(20).toInt() and 65535)
        }
        val lookup = replyTo(namedColor(92, manager.defaultColormap.id, "blue"), server)
        assertEquals(32, lookup.position())
        assertEquals(65535, lookup.getShort(12).toInt() and 65535)
        assertEquals(65535, lookup.getShort(18).toInt() and 65535)
    }

    @Test fun `unknown colors and maps return protocol errors`() {
        val (server, manager) = colorServer()
        val unknown = replyTo(namedColor(85, manager.defaultColormap.id, "not-a-color"), server)
        assertEquals(15, unknown.get(1).toInt())
        val invalid = replyTo(namedColor(85, -1, "white"), server)
        assertEquals(12, invalid.get(1).toInt())
        assertEquals(-1, invalid.getInt(4))
    }

    @Test fun `allocated color quantizes to the same RGB returned by QueryColors`() {
        val (server, manager) = colorServer()
        val request = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
        request.put(84).put(0).putShort(4).putInt(manager.defaultColormap.id)
            .putShort(0x1234).putShort(0x5678).putShort(0x7fff).putShort(0)
        val alloc = replyTo(request, server)
        assertEquals(32, alloc.position())
        assertEquals(0x12567f, alloc.getInt(16))
        val query = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
        query.put(91).put(0).putShort(3).putInt(manager.defaultColormap.id).putInt(alloc.getInt(16))
        val colors = replyTo(query, server)
        assertEquals(40, colors.position())
        assertEquals(2, colors.getInt(4))
        assertEquals(1, colors.getShort(8).toInt())
        for (offset in listOf(0, 2, 4)) assertEquals(alloc.getShort(8 + offset), colors.getShort(32 + offset))
    }

    @Test fun `lookup of absent atom returns None without creating it`() {
        val name = "_STEAMPSP_ABSENT_ATOM_TEST"
        assertEquals(-1, Atom.getId(name))
        val data = name.toByteArray(Charsets.ISO_8859_1)
        val length = 8 + ((data.size + 3) and -4)
        val request = ByteBuffer.allocate(length).order(ByteOrder.LITTLE_ENDIAN)
        request.put(16).put(1).putShort((length / 4).toShort())
        request.putShort(data.size.toShort()).putShort(0).put(data)
        while (request.hasRemaining()) request.put(0)
        val reply = replyTo(request)
        assertEquals(32, reply.position())
        assertEquals(1, reply.get(0).toInt())
        assertEquals(0, reply.getInt(8))
        assertEquals(-1, Atom.getId(name))
    }

    @Test fun `invalid atom id returns BadAtom instead of an indexing crash`() {
        for (id in listOf(0, -1, Int.MAX_VALUE)) {
            val request = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
            request.put(17).put(0).putShort(2).putInt(id)
            val reply = replyTo(request)
            assertEquals(32, reply.position())
            assertEquals(0, reply.get(0).toInt())
            assertEquals(5, reply.get(1).toInt())
            assertEquals(id, reply.getInt(4))
        }
    }

    @Test fun `QueryKeymap flushes its complete forty byte reply`() {
        val bytes = ByteBuffer.wrap(byteArrayOf(44, 0, 1, 0)).order(ByteOrder.LITTLE_ENDIAN)
        val input = XInputStream(32)
        set(input, XInputStream::class.java, "activeBuffer", bytes)
        var flushes = 0
        val output = object : XOutputStream(64) {
            override fun lock(): XStreamLock = XStreamLock { flushes++ }
        }.apply { setByteOrder(ByteOrder.LITTLE_ENDIAN) }
        val server = mock<XServer>()
        val lock = mock<XLock>()
        whenever(server.lock(XServer.Lockable.WINDOW_MANAGER)).thenReturn(lock)
        val xclient = mock<XClient>(defaultAnswer = CALLS_REAL_METHODS)
        set(xclient, XClient::class.java, "xServer", server)
        set(xclient, XClient::class.java, "inputStream", input)
        set(xclient, XClient::class.java, "outputStream", output)
        xclient.setAuthenticated(true)
        val transport = Client(null, null).apply { tag = xclient }
        set(transport, Client::class.java, "inputStream", input)
        set(transport, Client::class.java, "outputStream", output)
        assertTrue(XClientRequestHandler().handleRequest(transport))
        assertEquals(1, flushes)
        assertEquals(40, output.buffer.position())
        assertEquals(1, output.buffer.get(0).toInt())
        assertEquals(1, output.buffer.getShort(2).toInt())
        assertEquals(2, output.buffer.getInt(4))
    }

    @Test fun `server grab and ungrab do not insert unsolicited replies`() {
        val bytes = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
        bytes.put(36).put(0).putShort(1) // GrabServer: no reply in xproto.
        bytes.put(37).put(0).putShort(1) // UngrabServer: no reply in xproto.
        bytes.flip()
        val input = XInputStream(32)
        set(input, XInputStream::class.java, "activeBuffer", bytes)
        val output = XOutputStream(64)
        val server = mock<XServer>()
        val lock = mock<XLock>()
        whenever(server.lockAll()).thenReturn(lock)
        val xclient = mock<XClient>(defaultAnswer = CALLS_REAL_METHODS)
        whenever(server.isGrabbedBy(xclient)).thenReturn(true)
        set(xclient, XClient::class.java, "xServer", server)
        set(xclient, XClient::class.java, "inputStream", input)
        set(xclient, XClient::class.java, "outputStream", output)
        xclient.setAuthenticated(true)
        val transport = Client(null, null).apply { tag = xclient }
        set(transport, Client::class.java, "inputStream", input)
        set(transport, Client::class.java, "outputStream", output)
        val handler = XClientRequestHandler()
        assertTrue(handler.handleRequest(transport))
        assertTrue(handler.handleRequest(transport))
        assertEquals(0, input.available())
        assertEquals(0, output.buffer.position())
        assertEquals(2, xclient.sequenceNumber.toInt())
    }

    @Test fun `unsupported payload cannot become a following CreateWindow request`() {
        val bytes = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
        bytes.put(75).put(0).putShort(4) // Unsupported PolyText16, 12 payload bytes.
        bytes.putInt(1).putInt(0x001b000f).putInt(12) // Looks like a bogus window request.
        bytes.put(127).put(0).putShort(1) // The real next request is NoOperation.
        bytes.flip()
        val input = XInputStream(32)
        set(input, XInputStream::class.java, "activeBuffer", bytes)
        val output = object : XOutputStream(64) {
            override fun lock(): XStreamLock = XStreamLock {} // Retain wire reply for assertions.
        }.apply { setByteOrder(ByteOrder.LITTLE_ENDIAN) }
        val xclient = mock<XClient>(defaultAnswer = CALLS_REAL_METHODS)
        set(xclient, XClient::class.java, "inputStream", input)
        set(xclient, XClient::class.java, "outputStream", output)
        xclient.setAuthenticated(true)
        val transport = Client(null, null).apply { tag = xclient }
        set(transport, Client::class.java, "inputStream", input)
        set(transport, Client::class.java, "outputStream", output)
        val handler = XClientRequestHandler()
        assertTrue(handler.handleRequest(transport))
        assertEquals(4, input.available())
        assertEquals(32, output.buffer.position())
        assertEquals(1, output.buffer.get(1).toInt()) // BadRequest.
        assertEquals(75, output.buffer.get(10).toInt())
        assertTrue(handler.handleRequest(transport))
        assertEquals(0, input.available())
        assertEquals(2, xclient.sequenceNumber.toInt())
        assertEquals(32, output.buffer.position()) // No reply to NoOperation.
    }
}
