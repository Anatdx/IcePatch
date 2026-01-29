package com.anatdx.icepatch.ui.theme

import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CardElevation
import androidx.compose.material3.CardColors
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.unit.dp

object CardStyleProvider {
    @Composable
    fun styledContainerColor(base: Color): Color {
        val style = rememberCardStyleConfig()
        val dimTarget = if (base.luminance() < 0.5f) Color.Black else Color.White
        val dimmed = if (style.dim <= 0f) base else lerp(base, dimTarget, style.dim)
        return dimmed.copy(alpha = style.alpha)
    }

    @Composable
    fun contentColorFor(background: Color): Color {
        return if (background.luminance() < 0.5f) {
            MaterialTheme.colorScheme.onSurface
        } else {
            MaterialTheme.colorScheme.onSurfaceVariant
        }
    }

    @Composable
    fun elevatedCardColors(): CardColors {
        val container = styledContainerColor(MaterialTheme.colorScheme.surface)
        val content = contentColorFor(container)
        return CardDefaults.elevatedCardColors(
            containerColor = container,
            contentColor = content
        )
    }

    @Composable
    fun elevatedCardElevation(): CardElevation {
        val style = rememberCardStyleConfig()
        val baseElevation = if (style.alpha < 1f) 0.dp else 1.dp
        return CardDefaults.elevatedCardElevation(defaultElevation = baseElevation)
    }
}
