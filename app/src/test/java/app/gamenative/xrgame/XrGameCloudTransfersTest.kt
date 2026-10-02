package app.gamenative.xrgame

import java.io.IOException
import java.util.concurrent.CancellationException
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.Response
import okhttp3.ResponseBody
import okio.Buffer
import okio.BufferedSource
import okio.ForwardingSource
import okio.buffer
import org.junit.Assert.*
import org.junit.Test

class XrGameCloudTransfersTest {
    private val request = Request.Builder().url("https://cloud.example/save").build()

    @Test fun connectionFailureReturnsFailedTransfer() {
        val client = OkHttpClient.Builder().addInterceptor { throw IOException("connection refused") }.build()
        assertFalse(XrGameCloudTransfers.uploadBlock(client, request))
    }

    @Test fun httpResultsCloseTheBodyAndOnlySuccessCanBeCommitted() {
        for (code in listOf(200, 403, 503)) {
            var closed = false
            val source = object : ForwardingSource(Buffer().writeUtf8("response")) {
                override fun close() { closed = true; super.close() }
            }.buffer()
            val body = object : ResponseBody() {
                override fun contentType() = null
                override fun contentLength() = 8L
                override fun source(): BufferedSource = source
            }
            val client = OkHttpClient.Builder().addInterceptor {
                Response.Builder().request(it.request()).protocol(Protocol.HTTP_1_1)
                    .code(code).message("test").body(body).build()
            }.build()
            assertEquals(code == 200, XrGameCloudTransfers.uploadBlock(client, request))
            assertTrue(closed)
        }
    }

    @Test fun cancellationIsNotReportedAsATransferFailure() {
        val client = OkHttpClient.Builder().addInterceptor { throw CancellationException("cancelled") }.build()
        assertThrows(CancellationException::class.java) { XrGameCloudTransfers.uploadBlock(client, request) }
    }
}
