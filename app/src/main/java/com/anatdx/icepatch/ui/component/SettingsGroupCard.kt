package com.anatdx.icepatch.ui.component

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.unit.dp
import com.anatdx.icepatch.ui.theme.rememberCardStyleConfig

@Composable
fun SettingsGroupCard(
    title: String? = null,
    modifier: Modifier = Modifier,
    alphaOverride: Float? = null,
    dimOverride: Float? = null,
    content: @Composable ColumnScope.() -> Unit
) {
    val shape = RoundedCornerShape(20.dp)
    val style = rememberCardStyleConfig()
    val alpha = alphaOverride ?: style.alpha
    val dim = dimOverride ?: style.dim
    val base = MaterialTheme.colorScheme.surfaceVariant
    val dimTarget = if (base.luminance() < 0.5f) Color.Black else Color.White
    val dimmed = if (dim <= 0f) base else lerp(base, dimTarget, dim)
    val containerColor = dimmed.copy(alpha = alpha)
    val contentColor = if (containerColor.luminance() < 0.5f) {
        MaterialTheme.colorScheme.onSurface
    } else {
        MaterialTheme.colorScheme.onSurfaceVariant
    }

    Surface(
        modifier = modifier
            .fillMaxWidth()
            .clip(shape),
        color = containerColor,
        contentColor = contentColor,
        tonalElevation = 0.dp,
        shadowElevation = 0.dp,
        shape = shape
    ) {
        Column(modifier = Modifier.fillMaxWidth().padding(12.dp)) {
            if (!title.isNullOrBlank()) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.padding(bottom = 8.dp)
                )
            }
            content()
        }
    }
}
