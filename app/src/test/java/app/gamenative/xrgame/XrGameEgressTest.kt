package app.gamenative.xrgame

import java.net.InetSocketAddress
import java.net.Proxy
import java.net.ProxySelector
import java.net.URI
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class XrGameEgressTest {

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
        try {
            XrGameEgress.install()
            val selector = ProxySelector.getDefault()

            val blocked = selector.select(URI("https://api.gamenative.app/api/v1/x")).single()
            assertEquals(Proxy.Type.HTTP, blocked.type())
            val address = blocked.address() as InetSocketAddress
            assertTrue(address.address.isLoopbackAddress)
            assertEquals(9, address.port)

            val allowed = selector.select(URI("https://api.steampowered.com/ISteamDirectory/"))
            assertTrue(allowed.all { it.type() == Proxy.Type.DIRECT })
        } finally {
            ProxySelector.setDefault(original)
        }
    }
}
