package app.gamenative.ui.screen.xr

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import app.gamenative.R
import com.winlator.container.Container
import com.winlator.renderer.VulkanRenderer
import java.util.Locale

/** Android preview controls; gamepad, keyboard and mouse stay with XServerScreen. */
@Composable
fun SbsTheaterOverlay(container: Container, renderer: VulkanRenderer?, visible: Boolean, modifier: Modifier = Modifier) {
    var expanded by rememberSaveable(container.id) { mutableStateOf(false) }
    var stereo by rememberSaveable(container.id) { mutableStateOf(true) }
    var scale by rememberSaveable(container.id) { mutableFloatStateOf(SbsTheaterSettings.scale(container)) }
    var distance by rememberSaveable(container.id) { mutableFloatStateOf(SbsTheaterSettings.distance(container)) }
    LaunchedEffect(renderer, stereo, scale, distance) {
        renderer?.setSbsTheater(stereo, ImmersiveControls.BASE_WIDTH_METERS * scale, distance)
    }
    val persist = {
        container.putExtra(SbsTheaterSettings.SCALE, scale.toString())
        container.putExtra(SbsTheaterSettings.DISTANCE, distance.toString())
        container.saveData()
    }
    if (visible) Column(modifier.padding(6.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            FilledTonalButton(onClick = { expanded = !expanded }) { Text(stringResource(R.string.xrgame_theater_controls)) }
            if (expanded) FilledTonalButton(onClick = { stereo = !stereo }) {
                Text(stringResource(if (stereo) R.string.xrgame_theater_preview_flat else R.string.xrgame_theater_preview_sbs))
            }
        }
        if (expanded) Surface(shape = MaterialTheme.shapes.medium, tonalElevation = 4.dp) {
            Column(Modifier.width(280.dp).padding(12.dp)) {
                Text(stringResource(R.string.xrgame_theater_size, String.format(Locale.ROOT, "%.1f", scale)))
                Slider(value = scale, onValueChange = { scale = it }, onValueChangeFinished = { persist() },
                    valueRange = ImmersiveControls.MIN_SCALE..ImmersiveControls.MAX_SCALE)
                Text(stringResource(R.string.xrgame_theater_distance, String.format(Locale.ROOT, "%.1f", distance)))
                Slider(value = distance, onValueChange = { distance = it }, onValueChangeFinished = { persist() },
                    valueRange = ImmersiveControls.MIN_DISTANCE..ImmersiveControls.MAX_DISTANCE)
                TextButton(onClick = {
                    scale = ImmersiveControls.DEFAULT_SCALE
                    distance = ImmersiveControls.DEFAULT_DISTANCE
                    persist()
                }) { Text(stringResource(R.string.xrgame_theater_reset)) }
            }
        }
    }
}
