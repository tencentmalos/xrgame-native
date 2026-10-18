// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import app.gamenative.xrgame.XrGameCommandLine
import app.gamenative.xrgame.XrGameOfficialSteam
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SteamGameHandoffParseTest {
    private fun request(command: JSONObject? = null, appId: Any = 1145350) =
        JSONObject().put("appId", appId).put("pid", 42).apply { command?.let { put("command", it) } }.toString()

    private fun command(executable: String = "Ship/Hades2.exe", arguments: List<Any> = listOf("/c", "../"), work: String = "Ship") =
        JSONObject().put("executable", executable).put("arguments", JSONArray(arguments)).put("workingDirectory", work)

    @Test fun `the command Steam issued reaches the launcher`() {
        assertEquals(1145350 to XrGameOfficialSteam.Command("Ship/Hades2.exe", "/c ../", "Ship"),
            SteamGameHandoff.parse(request(command())))
    }

    @Test fun `arguments keep their boundaries on the Windows command line`() {
        val parsed = SteamGameHandoff.parse(request(command(arguments = listOf("-name", "My Hero", "", "say \"hi\""))))!!
        val line = parsed.second!!.arguments
        assertEquals("-name \"My Hero\" \"\" \"say \\\"hi\\\"\"", line)
        assertArrayEquals(arrayOf("-name", "My Hero", "", "say \"hi\""), XrGameCommandLine.split(line))
    }

    @Test fun `a request without a command still launches by app ID`() {
        assertEquals(1145350 to null, SteamGameHandoff.parse(request()))
    }

    @Test fun `an unusable command is dropped, never the launch`() {
        for (bad in listOf(
            command(executable = "/data/game.exe"), command(executable = "C:/game.exe"),
            command(executable = "../other/game.exe"), command(executable = "Ship\\Hades2.exe"),
            command(executable = ""), command(work = "../"), command(work = "/tmp"),
            command(arguments = listOf("a\u0000b")), command(arguments = listOf(1)),
            command(arguments = List(65) { "x" }),
            JSONObject().put("executable", "Ship/Hades2.exe").put("workingDirectory", "Ship"),
        )) assertEquals(bad.toString(), 1145350 to null, SteamGameHandoff.parse(request(bad)))
    }

    @Test fun `invalid app IDs are rejected`() {
        for (id in listOf<Any>(0, -1, 2147483648L, 4294967338L, "x")) assertNull(SteamGameHandoff.parse(request(appId = id)))
        assertNull(SteamGameHandoff.parse("not json"))
    }
}
