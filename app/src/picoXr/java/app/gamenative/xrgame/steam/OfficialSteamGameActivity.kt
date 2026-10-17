// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

/** Wine/display host with its own navigation stack; never brings Custom's library to the front. */
class OfficialSteamGameActivity : app.gamenative.MainActivity() {
    override val hostsOfficialSteam = true
}
