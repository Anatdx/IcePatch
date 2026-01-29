package com.anatdx.icepatch.ui.theme

import android.content.Context
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.livedata.observeAsState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import coil.compose.AsyncImage
import com.anatdx.icepatch.APApplication
import java.io.File

data class BackgroundConfig(
    val enabled: Boolean,
    val uri: String?,
    val alpha: Float,
    val dim: Float
)

data class CardStyleConfig(
    val alpha: Float,
    val dim: Float
)

object StylePrefs {
    const val KEY_BG_ENABLED = "custom_background_enabled"
    const val KEY_BG_URI = "custom_background_uri"
    const val KEY_BG_ALPHA = "custom_background_alpha"
    const val KEY_BG_DIM = "custom_background_dim"
    const val KEY_CARD_ALPHA = "card_alpha"
    const val KEY_CARD_DIM = "card_dim"

    fun loadBackground(): BackgroundConfig {
        val prefs = APApplication.sharedPreferences
        val storedUri = prefs.getString(KEY_BG_URI, null)
        return BackgroundConfig(
            enabled = prefs.getBoolean(KEY_BG_ENABLED, false),
            uri = storedUri?.takeIf { it.isNotBlank() },
            alpha = prefs.getFloat(KEY_BG_ALPHA, 0.92f),
            dim = prefs.getFloat(KEY_BG_DIM, 0.35f)
        )
    }

    fun loadCardStyle(): CardStyleConfig {
        val prefs = APApplication.sharedPreferences
        return CardStyleConfig(
            alpha = prefs.getFloat(KEY_CARD_ALPHA, 0.92f),
            dim = prefs.getFloat(KEY_CARD_DIM, 0.0f)
        )
    }
}

object BackgroundManager {
    private const val FILE_NAME = "custom_background"

    fun saveBackground(context: Context, source: Uri): String? {
        return try {
            val input = context.contentResolver.openInputStream(source) ?: return null
            val target = File(context.filesDir, FILE_NAME)
            target.outputStream().use { out ->
                input.use { it.copyTo(out) }
            }
            target.absolutePath
        } catch (_: Exception) {
            null
        }
    }

    fun clearBackground(context: Context) {
        val target = File(context.filesDir, FILE_NAME)
        if (target.exists()) {
            target.delete()
        }
    }
}

@Composable
fun rememberBackgroundConfig(): BackgroundConfig {
    var config by remember { mutableStateOf(StylePrefs.loadBackground()) }
    val refreshThemeObserver by refreshTheme.observeAsState(false)
    if (refreshThemeObserver == true) {
        config = StylePrefs.loadBackground()
    }
    return config
}

@Composable
fun rememberCardStyleConfig(): CardStyleConfig {
    var config by remember { mutableStateOf(StylePrefs.loadCardStyle()) }
    val refreshThemeObserver by refreshTheme.observeAsState(false)
    if (refreshThemeObserver == true) {
        config = StylePrefs.loadCardStyle()
    }
    return config
}

@Composable
fun BackgroundLayer(
    config: BackgroundConfig,
    darkTheme: Boolean,
    content: @Composable () -> Unit
) {
    Box(modifier = Modifier.fillMaxSize()) {
        val model = resolveBackgroundModel(config.uri)
        if (config.enabled && model != null) {
            AsyncImage(
                model = model,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier
                    .fillMaxSize()
                    .graphicsLayer(alpha = config.alpha)
            )
        }
        if (config.enabled) {
            val overlay = if (darkTheme) Color.Black else Color.White
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(
                        Brush.verticalGradient(
                            listOf(
                                overlay.copy(alpha = config.dim),
                                overlay.copy(alpha = config.dim * 0.7f)
                            )
                        )
                    )
            )
        }
        content()
    }
}

private fun resolveBackgroundModel(value: String?): Any? {
    if (value.isNullOrBlank()) return null
    if (value.startsWith("file://")) {
        val path = Uri.parse(value).path ?: return null
        val file = File(path)
        return if (file.exists()) file else null
    }
    if (value.startsWith("/")) {
        val file = File(value)
        return if (file.exists()) file else null
    }
    return value
}
