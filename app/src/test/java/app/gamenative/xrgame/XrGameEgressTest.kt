package app.gamenative.xrgame

import java.net.InetSocketAddress
import java.net.Proxy
import java.net.ProxySelector
import java.net.URI
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.assertSame
import okhttp3.OkHttpClient
import org.junit.Test

class XrGameEgressTest {

    @Test fun tokyoCloudStorageIsAllowedOnlyForItsAuthenticatedHttpsOrigin() {
        val host = "steamcloud-tyo.s3.dualstack.ap-northeast-1.amazonaws.com"
        val base = OkHttpClient()
        val client = XrGameEgress.steamCloudClient(base, "https://$host/signed-save")
        assertFalse(XrGameEgress.isAllowed(host))
        assertFalse(client.followRedirects)
        assertFalse(client.followSslRedirects)
        assertEquals(XrGameEgress.componentProxySelector().select(URI("https://$host/save")),
            client.proxySelector.select(URI("https://$host/save")))
        for (url in listOf("https://other.s3.dualstack.ap-northeast-1.amazonaws.com/save",
            "https://$host.evil.example/save", "http://$host/save", "https://$host:444/save")) {
            assertEquals(9, (client.proxySelector.select(URI(url)).single().address() as InetSocketAddress).port)
            assertSame(base, XrGameEgress.steamCloudClient(base, url))
        }
    }

    @Test fun steamCloudStorageExceptionIsLimitedToOneAuthenticatedReplyOrigin() {
        val host = "steamcloud-hkg.oss-accelerate.aliyuncs.com"
        val base = OkHttpClient()
        val client = XrGameEgress.steamCloudClient(base, "https://$host/signed-save")
        assertFalse(XrGameEgress.isAllowed(host))
        assertFalse(client.followRedirects)
        assertFalse(client.followSslRedirects)
        assertEquals(XrGameEgress.componentProxySelector().select(URI("https://$host/save")),
            client.proxySelector.select(URI("https://$host/save")))
        for (url in listOf("https://steamcloudhk2.blob.core.windows.net/save", "https://other.aliyuncs.com/",
            "https://$host.evil.example/", "http://$host/save", "https://$host:444/save")) {
            assertEquals(9, (client.proxySelector.select(URI(url)).single().address() as InetSocketAddress).port)
        }
        for (url in listOf("https://other.blob.core.windows.net/save", "http://$host/save", "https://$host:444/save")) {
            assertSame(base, XrGameEgress.steamCloudClient(base, url))
        }
    }

    @Test
    fun valveHostsAreAllowed() {
        listOf(
            "api.steampowered.com",
            "store.steampowered.com",
            "steamcommunity.com",
            "cdn.akamai.steamstatic.com",
            "shared.steamstatic.com",
            "cache1-fra1.steamcontent.com",
            "ext1-ams1.steamserver.net",
            "cloud-3.steamusercontent.com",
            "steamcdn-a.akamaihd.net",
            "STEAMCOMMUNITY.COM.", // case and trailing dot
        ).forEach { assertTrue(it, XrGameEgress.isAllowed(it)) }
    }

    @Test
    fun localAndLiteralAddressesAreAllowed() {
        listOf("localhost", "127.0.0.1", "192.168.1.5", "[::1]", "10.0.0.2").forEach {
            assertTrue(it, XrGameEgress.isAllowed(it))
        }
    }

    @Test
    fun upstreamAndThirdPartyHostsAreBlocked() {
        listOf(
            "api.gamenative.app",
            "downloads.gamenative.app",
            "relay.gamenative.app",
            "pub-9fcd5294bd0d4b85a9d73615bf98f3b5.r2.dev",
            "raw.githubusercontent.com",
            "dns.google",
            "us.i.posthog.com",
            "www.pcgamingwiki.com",
            "howlongtobeat.com",
            "img.youtube.com",
            "www.steamgriddb.com",
        ).forEach { assertFalse(it, XrGameEgress.isAllowed(it)) }
    }

    @Test
    fun lookalikeHostsAreBlocked() {
        listOf(
            "evilsteampowered.com",
            "steampowered.com.evil.example",
            "akamaihd.net",
            "other-a.akamaihd.net",
        ).forEach { assertFalse(it, XrGameEgress.isAllowed(it)) }
    }

    @Test
    fun selectorRoutesBlockedHostsToClosedLoopbackPort() {
        val original = ProxySelector.getDefault()
        val allowedUri = URI("https://api.steampowered.com/ISteamDirectory/")
        // Allowed hosts retain the system proxy configuration (which need not be DIRECT).
        val originalRoute = original?.select(allowedUri) ?: listOf(Proxy.NO_PROXY)
        try {
            XrGameEgress.install()
            val selector = ProxySelector.getDefault()

            val blocked = selector.select(URI("https://api.gamenative.app/api/v1/x")).single()
            assertEquals(Proxy.Type.HTTP, blocked.type())
            val address = blocked.address() as InetSocketAddress
            assertTrue(address.address.isLoopbackAddress)
            assertEquals(9, address.port)

            assertEquals(originalRoute, selector.select(allowedUri))
        } finally {
            ProxySelector.setDefault(original)
        }
    }
}
