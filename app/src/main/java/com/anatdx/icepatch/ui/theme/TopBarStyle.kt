package com.anatdx.icepatch.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.TopAppBarColors
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

object TopBarStyle {
    @Composable
    fun topAppBarColors(): TopAppBarColors {
        val background = rememberBackgroundConfig()
        val container = if (background.enabled) {
            Color.Transparent
        } else {
            MaterialTheme.colorScheme.surface
        }
        val content = MaterialTheme.colorScheme.onSurface
        return TopAppBarDefaults.topAppBarColors(
            containerColor = container,
            scrolledContainerColor = container,
            navigationIconContentColor = content,
            titleContentColor = content,
            actionIconContentColor = content
        )
    }
}
