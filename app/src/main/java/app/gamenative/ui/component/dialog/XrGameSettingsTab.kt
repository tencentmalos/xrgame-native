package app.gamenative.ui.component.dialog

import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import app.gamenative.R
import app.gamenative.ui.theme.settingsTileColors
import com.alorma.compose.settings.ui.SettingsGroup
import com.alorma.compose.settings.ui.SettingsMenuLink

/**
 * XRGame's first page of the container settings: only the options XRGame added (VR display,
 * composite, presentation). The full upstream tab set stays one tap away under "Legacy settings".
 */
@Composable
fun XrGameSettingsTabContent(state: ContainerConfigState, onOpenLegacy: () -> Unit) {
    SettingsGroup(title = { Text(stringResource(R.string.xrgame_settings_vr)) }) {
        XrDisplaySection(state, default = false)
    }
    SettingsGroup(title = { Text(stringResource(R.string.xrgame_settings_presentation)) }) {
        XrGamePresentSection(state)
    }
    SettingsGroup {
        SettingsMenuLink(
            colors = settingsTileColors(),
            title = { Text(stringResource(R.string.xrgame_settings_legacy)) },
            subtitle = { Text(stringResource(R.string.xrgame_settings_legacy_desc)) },
            onClick = onOpenLegacy,
        )
    }
}
