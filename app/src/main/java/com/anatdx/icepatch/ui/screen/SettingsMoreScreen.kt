package com.anatdx.icepatch.ui.screen

import android.net.Uri
import android.os.Build
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ColorLens
import androidx.compose.material.icons.filled.DarkMode
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.DeveloperMode
import androidx.compose.material.icons.filled.FormatColorFill
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.InvertColors
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.runtime.livedata.observeAsState
import androidx.core.content.edit
import com.anatdx.icepatch.APApplication
import com.anatdx.icepatch.R
import com.anatdx.icepatch.ui.component.SettingsGroupCard
import com.anatdx.icepatch.ui.component.SwitchItem
import com.anatdx.icepatch.ui.theme.BackgroundManager
import com.anatdx.icepatch.ui.theme.StylePrefs
import com.anatdx.icepatch.ui.theme.TopBarStyle
import com.anatdx.icepatch.ui.theme.refreshTheme
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator

@Destination<RootGraph>
@Composable
@OptIn(ExperimentalMaterial3Api::class)
fun SettingsMoreScreen(navigator: DestinationsNavigator) {
    val state by APApplication.apStateLiveData.observeAsState(APApplication.State.UNKNOWN_STATE)
    val kPatchReady = state != APApplication.State.UNKNOWN_STATE
    val aPatchReady =
        (state == APApplication.State.ANDROIDPATCH_INSTALLING || state == APApplication.State.ANDROIDPATCH_INSTALLED || state == APApplication.State.ANDROIDPATCH_NEED_UPDATE)
    val context = LocalContext.current
    val prefs = APApplication.sharedPreferences

    val showThemeChooseDialog = remember { mutableStateOf(false) }
    if (showThemeChooseDialog.value) {
        ThemeChooseDialog(showThemeChooseDialog)
    }

    val pickBackgroundLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.GetContent()
    ) { uri: Uri? ->
        if (uri != null) {
            val savedUri = BackgroundManager.saveBackground(context, uri)
            if (savedUri != null) {
                prefs.edit {
                    putString(StylePrefs.KEY_BG_URI, savedUri)
                    putBoolean(StylePrefs.KEY_BG_ENABLED, true)
                }
                refreshTheme.value = true
            } else {
                Toast.makeText(context, R.string.settings_custom_background_failed, Toast.LENGTH_SHORT).show()
            }
        }
    }

    Scaffold(
        containerColor = Color.Transparent,
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.settings_more)) },
                navigationIcon = {
                    IconButton(onClick = { navigator.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
                    }
                },
                colors = TopBarStyle.topAppBarColors()
            )
        }
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .padding(paddingValues)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
        ) {
            var nightFollowSystem by rememberSaveable {
                mutableStateOf(
                    prefs.getBoolean("night_mode_follow_sys", true)
                )
            }
            var backgroundEnabled by rememberSaveable {
                mutableStateOf(prefs.getBoolean(StylePrefs.KEY_BG_ENABLED, false))
            }
            var backgroundAlpha by rememberSaveable {
                mutableStateOf(prefs.getFloat(StylePrefs.KEY_BG_ALPHA, 0.92f))
            }
            var backgroundDim by rememberSaveable {
                mutableStateOf(prefs.getFloat(StylePrefs.KEY_BG_DIM, 0.35f))
            }
            var cardAlpha by rememberSaveable {
                mutableStateOf(prefs.getFloat(StylePrefs.KEY_CARD_ALPHA, 0.92f))
            }
            var cardDim by rememberSaveable {
                mutableStateOf(prefs.getFloat(StylePrefs.KEY_CARD_DIM, 0f))
            }

            SettingsGroupCard(
                title = stringResource(id = R.string.settings_appearance),
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
            ) {
                SwitchItem(
                    icon = Icons.Filled.InvertColors,
                    title = stringResource(id = R.string.settings_night_mode_follow_sys),
                    summary = stringResource(id = R.string.settings_night_mode_follow_sys_summary),
                    checked = nightFollowSystem
                ) {
                    prefs.edit { putBoolean("night_mode_follow_sys", it) }
                    nightFollowSystem = it
                    refreshTheme.value = true
                }

                if (!nightFollowSystem) {
                    var nightThemeEnabled by rememberSaveable {
                        mutableStateOf(
                            prefs.getBoolean("night_mode_enabled", false)
                        )
                    }
                    SwitchItem(
                        icon = Icons.Filled.DarkMode,
                        title = stringResource(id = R.string.settings_night_theme_enabled),
                        checked = nightThemeEnabled
                    ) {
                        prefs.edit { putBoolean("night_mode_enabled", it) }
                        nightThemeEnabled = it
                        refreshTheme.value = true
                    }
                }

                val isDynamicColorSupport = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                if (isDynamicColorSupport) {
                    var useSystemDynamicColor by rememberSaveable {
                        mutableStateOf(
                            prefs.getBoolean("use_system_color_theme", true)
                        )
                    }
                    SwitchItem(
                        icon = Icons.Filled.ColorLens,
                        title = stringResource(id = R.string.settings_use_system_color_theme),
                        summary = stringResource(id = R.string.settings_use_system_color_theme_summary),
                        checked = useSystemDynamicColor
                    ) {
                        prefs.edit { putBoolean("use_system_color_theme", it) }
                        useSystemDynamicColor = it
                        refreshTheme.value = true
                    }

                    if (!useSystemDynamicColor) {
                        ListItem(headlineContent = {
                            Text(text = stringResource(id = R.string.settings_custom_color_theme))
                        }, modifier = Modifier.clickable {
                            showThemeChooseDialog.value = true
                        }, supportingContent = {
                            val colorMode = prefs.getString("custom_color", "blue")
                            Text(
                                text = stringResource(colorNameToString(colorMode.toString())),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.outline
                            )
                        }, leadingContent = { Icon(Icons.Filled.FormatColorFill, null) })
                    }
                } else {
                    ListItem(headlineContent = {
                        Text(text = stringResource(id = R.string.settings_custom_color_theme))
                    }, modifier = Modifier.clickable {
                        showThemeChooseDialog.value = true
                    }, supportingContent = {
                        val colorMode = prefs.getString("custom_color", "blue")
                        Text(
                            text = stringResource(colorNameToString(colorMode.toString())),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.outline
                        )
                    }, leadingContent = { Icon(Icons.Filled.FormatColorFill, null) })
                }

                Text(
                    text = stringResource(id = R.string.settings_custom_background_title),
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                )
                SwitchItem(
                    icon = Icons.Filled.Image,
                    title = stringResource(id = R.string.settings_custom_background),
                    summary = stringResource(id = R.string.settings_custom_background_summary),
                    checked = backgroundEnabled
                ) {
                    prefs.edit { putBoolean(StylePrefs.KEY_BG_ENABLED, it) }
                    backgroundEnabled = it
                    refreshTheme.value = true
                }
                AnimatedVisibility(visible = backgroundEnabled) {
                    Column {
                        ListItem(
                            leadingContent = { Icon(Icons.Filled.Image, null) },
                            headlineContent = { Text(stringResource(id = R.string.settings_pick_background)) },
                            modifier = Modifier.clickable {
                                pickBackgroundLauncher.launch("image/*")
                            }
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.End
                        ) {
                            TextButton(onClick = {
                                BackgroundManager.clearBackground(context)
                                prefs.edit {
                                    putString(StylePrefs.KEY_BG_URI, "")
                                    putBoolean(StylePrefs.KEY_BG_ENABLED, false)
                                }
                                backgroundEnabled = false
                                refreshTheme.value = true
                            }) {
                                Icon(Icons.Filled.Delete, contentDescription = null)
                                Spacer(modifier = Modifier.width(4.dp))
                                Text(stringResource(id = R.string.settings_clear_background))
                            }
                        }

                        Text(text = stringResource(id = R.string.settings_background_alpha))
                        Slider(
                            value = backgroundAlpha,
                            valueRange = 0.4f..1f,
                            onValueChange = { backgroundAlpha = it },
                            onValueChangeFinished = {
                                prefs.edit { putFloat(StylePrefs.KEY_BG_ALPHA, backgroundAlpha) }
                                refreshTheme.value = true
                            }
                        )
                        Text(text = stringResource(id = R.string.settings_background_dim))
                        Slider(
                            value = backgroundDim,
                            valueRange = 0f..0.6f,
                            onValueChange = { backgroundDim = it },
                            onValueChangeFinished = {
                                prefs.edit { putFloat(StylePrefs.KEY_BG_DIM, backgroundDim) }
                                refreshTheme.value = true
                            }
                        )
                    }
                }

                Text(text = stringResource(id = R.string.settings_card_alpha))
                Slider(
                    value = cardAlpha,
                    valueRange = 0.4f..1f,
                    onValueChange = { cardAlpha = it },
                    onValueChangeFinished = {
                        prefs.edit { putFloat(StylePrefs.KEY_CARD_ALPHA, cardAlpha) }
                        refreshTheme.value = true
                    }
                )
                Text(text = stringResource(id = R.string.settings_card_dim))
                Slider(
                    value = cardDim,
                    valueRange = 0f..0.6f,
                    onValueChange = { cardDim = it },
                    onValueChangeFinished = {
                        prefs.edit { putFloat(StylePrefs.KEY_CARD_DIM, cardDim) }
                        refreshTheme.value = true
                    }
                )
            }

            if (aPatchReady && kPatchReady) {
                SettingsGroupCard(
                    title = stringResource(id = R.string.settings_developer_options),
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    var enableWebDebugging by rememberSaveable {
                        mutableStateOf(
                            prefs.getBoolean("enable_web_debugging", false)
                        )
                    }
                    SwitchItem(
                        icon = Icons.Filled.DeveloperMode,
                        title = stringResource(id = R.string.enable_web_debugging),
                        summary = stringResource(id = R.string.enable_web_debugging_summary),
                        checked = enableWebDebugging
                    ) {
                        APApplication.sharedPreferences.edit {
                            putBoolean("enable_web_debugging", it)
                        }
                        enableWebDebugging = it
                    }
                }
            }
        }
    }
}
