package com.anatdx.icepatch.ui

import android.annotation.SuppressLint
import android.content.Intent
import android.content.res.Configuration
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.animation.AnimatedContentTransitionScope
import androidx.compose.animation.Crossfade
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.livedata.observeAsState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.navigation.NavBackStackEntry
import androidx.navigation.NavHostController
import androidx.navigation.compose.rememberNavController
import coil.Coil
import coil.ImageLoader
import com.ramcosta.composedestinations.DestinationsNavHost
import com.ramcosta.composedestinations.animations.NavHostAnimatedDestinationStyle
import com.ramcosta.composedestinations.generated.NavGraphs
import com.ramcosta.composedestinations.rememberNavHostEngine
import com.ramcosta.composedestinations.utils.isRouteOnBackStackAsState
import com.ramcosta.composedestinations.utils.rememberDestinationsNavigator
import com.anatdx.icepatch.APApplication
import android.util.Log
import com.anatdx.icepatch.Natives
import com.anatdx.icepatch.R
import com.anatdx.icepatch.ui.component.KpmInstallConfirmDialog
import com.anatdx.icepatch.ui.component.ModuleInstallConfirmDialog
import com.anatdx.icepatch.ui.screen.BottomBarDestination
import com.anatdx.icepatch.ui.theme.APatchTheme
import com.anatdx.icepatch.ui.theme.rememberBackgroundConfig
import com.anatdx.icepatch.ui.viewmodel.SuperUserViewModel
import com.ramcosta.composedestinations.generated.destinations.InstallModeSelectScreenDestination
import com.ramcosta.composedestinations.generated.destinations.InstallScreenDestination
import com.ramcosta.composedestinations.generated.destinations.ExecuteAPMActionScreenDestination
import com.anatdx.icepatch.ui.screen.MODULE_TYPE
import com.anatdx.icepatch.util.KpmInfo
import com.anatdx.icepatch.util.KpmInfoReader
import com.anatdx.icepatch.util.ModuleZipInfo
import com.anatdx.icepatch.util.ZipModuleDetector
import com.anatdx.icepatch.util.ui.LocalSnackbarHost
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import me.zhanghai.android.appiconloader.coil.AppIconFetcher
import me.zhanghai.android.appiconloader.coil.AppIconKeyer
import androidx.core.net.toUri

class MainActivity : AppCompatActivity() {

    private var isLoading = true
    private val tag = "MainActivity"

    /** 用于 Composable 观察；onNewIntent 时更新，以便分享到已打开的 app 时能处理新 intent */
    var latestIntent by mutableStateOf<Intent?>(null)
        private set

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        latestIntent = intent
    }

    @SuppressLint("UnusedMaterial3ScaffoldPaddingParameter")
    override fun onCreate(savedInstanceState: Bundle?) {
        latestIntent = intent

        installSplashScreen().setKeepOnScreenCondition { isLoading }

        enableEdgeToEdge()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            window.isNavigationBarContrastEnforced = false
        }

        super.onCreate(savedInstanceState)

        // For testing / automation: allow passing SuperKey via intent extra.
        intent?.getStringExtra("superkey")?.takeIf { it.isNotBlank() }?.let { key ->
            APApplication.superKey = key
            Log.d(tag, "superkey injected via intent (len=${key.length})")
            // Emit a quick diag to logcat for KP validation.
            runCatching { Log.d(tag, "kp nativeReady=" + Natives.nativeReady(key)) }
        }

        setContent {
            APatchTheme {
                val navController = rememberNavController()
                val navigator = navController.rememberDestinationsNavigator()
                val snackBarHostState = remember { SnackbarHostState() }
                val configuration = LocalConfiguration.current

                var showModuleInstallDialog by remember { mutableStateOf(false) }
                var pendingModules by remember { mutableStateOf<List<ModuleZipInfo>>(emptyList()) }
                var showKpmInstallDialog by remember { mutableStateOf(false) }
                var pendingKpmInfo by remember { mutableStateOf<KpmInfo?>(null) }
                var intentHandled by remember { mutableStateOf(false) }

                val currentIntent = latestIntent
                val incomingUris = remember(currentIntent) {
                    when (currentIntent?.action) {
                        Intent.ACTION_SEND -> {
                            @Suppress("DEPRECATION")
                            val uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                                currentIntent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                            } else {
                                currentIntent.getParcelableExtra(Intent.EXTRA_STREAM)
                            }
                            uri?.let { listOf(it) }
                                ?: currentIntent.data?.let { listOf(it) }
                                ?: (currentIntent.clipData?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.uri)?.let { listOf(it) }
                                ?: emptyList()
                        }
                        Intent.ACTION_SEND_MULTIPLE -> {
                            @Suppress("DEPRECATION")
                            val uris = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                                currentIntent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)
                            } else {
                                currentIntent.getParcelableArrayListExtra(Intent.EXTRA_STREAM)
                            }
                            uris?.takeIf { it.isNotEmpty() }
                                ?: (currentIntent.clipData?.let { cd -> (0 until cd.itemCount).mapNotNull { cd.getItemAt(it).uri }.takeIf { it.isNotEmpty() } })
                                ?: emptyList()
                        }
                        else -> {
                            currentIntent?.data?.let { listOf(it) }
                                ?: (currentIntent?.clipData?.let { cd -> (0 until cd.itemCount).mapNotNull { cd.getItemAt(it).uri } })?.takeIf { it.isNotEmpty() }
                                ?: emptyList()
                        }
                    }
                }
                val kpmUris = remember(incomingUris) {
                    incomingUris.filter { it.lastPathSegment.orEmpty().endsWith(".kpm", ignoreCase = true) }
                }
                val zipUris = remember(incomingUris) {
                    incomingUris.filter { !it.lastPathSegment.orEmpty().endsWith(".kpm", ignoreCase = true) }
                }
                val bottomBarRoutes = remember {
                    BottomBarDestination.entries.map { it.direction.route }.toSet()
                }
                val bottomBarRouteIndex = remember {
                    BottomBarDestination.entries
                        .mapIndexed { index, destination -> destination.direction.route to index }
                        .toMap()
                }
                val kpState by APApplication.kpStateLiveData.observeAsState(APApplication.State.UNKNOWN_STATE)
                val apState by APApplication.apStateLiveData.observeAsState(APApplication.State.UNKNOWN_STATE)
                val rootlessMode by APApplication.rootlessModeLiveData.observeAsState(false)
                val kPatchReady = kpState != APApplication.State.UNKNOWN_STATE
                val aPatchReady = apState == APApplication.State.ANDROIDPATCH_INSTALLED
                val visibleDestinations = remember(kpState, apState, rootlessMode) {
                    BottomBarDestination.entries.filter { destination ->
                        !(destination.kPatchRequired && !kPatchReady) &&
                            !(destination.aPatchRequired && !aPatchReady) &&
                            !(rootlessMode && destination == BottomBarDestination.AModule)
                    }
                }

                val defaultTransitions = remember(bottomBarRoutes, bottomBarRouteIndex) {
                    object : NavHostAnimatedDestinationStyle() {
                        override val enterTransition: AnimatedContentTransitionScope<NavBackStackEntry>.() -> EnterTransition =
                            {
                                val fromIndex = bottomBarRouteIndex[initialState.destination.route]
                                val toIndex = bottomBarRouteIndex[targetState.destination.route]
                                if (fromIndex != null && toIndex != null) {
                                    val slideFrom = if (toIndex > fromIndex) { { width: Int -> width / 3 } } else { { width: Int -> -width / 3 } }
                                    slideInHorizontally(
                                        initialOffsetX = slideFrom,
                                        animationSpec = tween(durationMillis = 300, easing = FastOutSlowInEasing)
                                    ) + fadeIn(animationSpec = tween(durationMillis = 240, easing = FastOutSlowInEasing))
                                } else if (targetState.destination.route !in bottomBarRoutes) {
                                    slideInHorizontally(
                                        initialOffsetX = { it / 2 },
                                        animationSpec = tween(durationMillis = 320, easing = FastOutSlowInEasing)
                                    ) + fadeIn(animationSpec = tween(durationMillis = 260, easing = FastOutSlowInEasing))
                                } else {
                                    fadeIn(animationSpec = tween(220, easing = FastOutSlowInEasing))
                                }
                            }

                        override val exitTransition: AnimatedContentTransitionScope<NavBackStackEntry>.() -> ExitTransition =
                            {
                                val fromIndex = bottomBarRouteIndex[initialState.destination.route]
                                val toIndex = bottomBarRouteIndex[targetState.destination.route]
                                if (fromIndex != null && toIndex != null) {
                                    val slideTo = if (toIndex > fromIndex) { { width: Int -> -width / 6 } } else { { width: Int -> width / 6 } }
                                    slideOutHorizontally(
                                        targetOffsetX = slideTo,
                                        animationSpec = tween(durationMillis = 300, easing = FastOutSlowInEasing)
                                    ) + fadeOut(animationSpec = tween(durationMillis = 210, easing = FastOutSlowInEasing))
                                } else if (initialState.destination.route in bottomBarRoutes && targetState.destination.route !in bottomBarRoutes) {
                                    slideOutHorizontally(
                                        targetOffsetX = { -it / 6 },
                                        animationSpec = tween(durationMillis = 300, easing = FastOutSlowInEasing)
                                    ) + fadeOut(
                                        animationSpec = tween(durationMillis = 220, easing = FastOutSlowInEasing)
                                    )
                                } else {
                                    fadeOut(animationSpec = tween(durationMillis = 220, easing = FastOutSlowInEasing))
                                }
                            }

                        override val popEnterTransition: AnimatedContentTransitionScope<NavBackStackEntry>.() -> EnterTransition =
                            {
                                if (targetState.destination.route in bottomBarRoutes) {
                                    slideInHorizontally(
                                        initialOffsetX = { -it / 6 },
                                        animationSpec = tween(durationMillis = 300, easing = FastOutSlowInEasing)
                                    ) + fadeIn(
                                        animationSpec = tween(durationMillis = 240, easing = FastOutSlowInEasing)
                                    )
                                } else {
                                    fadeIn(animationSpec = tween(durationMillis = 220, easing = FastOutSlowInEasing))
                                }
                            }

                        override val popExitTransition: AnimatedContentTransitionScope<NavBackStackEntry>.() -> ExitTransition =
                            {
                                if (initialState.destination.route !in bottomBarRoutes) {
                                    scaleOut(
                                        targetScale = 0.96f,
                                        animationSpec = tween(durationMillis = 260, easing = FastOutSlowInEasing)
                                    ) + fadeOut(animationSpec = tween(durationMillis = 220, easing = FastOutSlowInEasing))
                                } else {
                                    fadeOut(animationSpec = tween(durationMillis = 180, easing = FastOutSlowInEasing))
                                }
                            }
                    }
                }
                val navHostEngine = rememberNavHostEngine(navHostContentAlignment = Alignment.TopCenter)

                ShortcutIntentHandler(currentIntent = currentIntent, navigator = navigator)

                LaunchedEffect(Unit) {
                    if (SuperUserViewModel.apps.isEmpty()) {
                        SuperUserViewModel().fetchAppList()
                    }
                }

                // 新 intent（例如再次分享）时重置已处理标记，以便能处理新分享
                LaunchedEffect(currentIntent) {
                    intentHandled = false
                }

                LaunchedEffect(kpmUris, zipUris, intentHandled) {
                    if (intentHandled) return@LaunchedEffect
                    if (kpmUris.isNotEmpty()) {
                        val info = KpmInfoReader.readKpmInfo(this@MainActivity, kpmUris.first())
                        if (info != null) {
                            pendingKpmInfo = info
                            showKpmInstallDialog = true
                        } else {
                            lifecycleScope.launch {
                                snackBarHostState.showSnackbar(
                                    getString(R.string.kpm_unsupported_format),
                                    withDismissAction = true
                                )
                            }
                        }
                        intentHandled = true
                        return@LaunchedEffect
                    }
                    if (zipUris.isNotEmpty()) {
                        // 分享进来的单文件可能是 content Uri，lastPathSegment 未必带 .kpm，先尝试按 KPM 解析
                        if (zipUris.size == 1) {
                            val kpmInfo = KpmInfoReader.readKpmInfo(this@MainActivity, zipUris.first())
                            if (kpmInfo != null) {
                                pendingKpmInfo = kpmInfo
                                showKpmInstallDialog = true
                                intentHandled = true
                                return@LaunchedEffect
                            }
                        }
                        val modules = ZipModuleDetector.detectModuleZips(this@MainActivity, zipUris)
                        if (modules.isNotEmpty()) {
                            pendingModules = modules
                            showModuleInstallDialog = true
                        } else {
                            lifecycleScope.launch {
                                snackBarHostState.showSnackbar(
                                    getString(R.string.zip_unsupported_format),
                                    withDismissAction = true
                                )
                            }
                        }
                        intentHandled = true
                    }
                }

                ModuleInstallConfirmDialog(
                    show = showModuleInstallDialog,
                    modules = pendingModules,
                    onConfirm = { modules ->
                        showModuleInstallDialog = false
                        val first = modules.firstOrNull() ?: return@ModuleInstallConfirmDialog
                        navigator.navigate(InstallScreenDestination(first.uri, MODULE_TYPE.APM))
                        pendingModules = emptyList()
                    },
                    onDismiss = {
                        showModuleInstallDialog = false
                        pendingModules = emptyList()
                    }
                )

                KpmInstallConfirmDialog(
                    show = showKpmInstallDialog,
                    info = pendingKpmInfo,
                    onInstall = {
                        val uri = pendingKpmInfo?.uri ?: return@KpmInstallConfirmDialog
                        showKpmInstallDialog = false
                        pendingKpmInfo = null
                        navigator.navigate(InstallScreenDestination(uri, MODULE_TYPE.KPM))
                    },
                    onEmbed = {
                        showKpmInstallDialog = false
                        pendingKpmInfo = null
                        navigator.navigate(InstallModeSelectScreenDestination())
                    },
                    onDismiss = {
                        showKpmInstallDialog = false
                        pendingKpmInfo = null
                    }
                )

                Scaffold(
                    snackbarHost = { SnackbarHost(hostState = snackBarHostState) },
                    bottomBar = {
                        if (configuration.orientation == Configuration.ORIENTATION_PORTRAIT) {
                            BottomBar(navController, visibleDestinations)
                        }
                    },
                    contentWindowInsets = WindowInsets(0, 0, 0, 0)
                ) { innerPadding ->
                    CompositionLocalProvider(
                        LocalSnackbarHost provides snackBarHostState,
                    ) {
                        if (configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
                            Row(modifier = Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.systemBars.only(WindowInsetsSides.Horizontal))) {
                                SideBar(
                                    navController = navController,
                                    modifier = Modifier.windowInsetsPadding(WindowInsets.systemBars.only(WindowInsetsSides.Top)),
                                    visibleDestinations = visibleDestinations
                                )
                                Box(
                                    modifier = Modifier
                                        .weight(1f)
                                        .consumeWindowInsets(WindowInsets.safeDrawing.only(WindowInsetsSides.Start))
                                ) {
                                    DestinationsNavHost(
                                        navGraph = NavGraphs.root,
                                        navController = navController,
                                        engine = navHostEngine,
                                        defaultTransitions = defaultTransitions
                                    )
                                }
                            }
                        } else {
                            Box(
                                modifier = Modifier
                                    .fillMaxSize()
                                    .padding(innerPadding)
                                    .consumeWindowInsets(innerPadding)
                            ) {
                                DestinationsNavHost(
                                    navGraph = NavGraphs.root,
                                    navController = navController,
                                    engine = navHostEngine,
                                    defaultTransitions = defaultTransitions
                                )
                            }
                        }
                    }
                }
            }
        }

        // Initialize Coil
        val iconSize = resources.getDimensionPixelSize(android.R.dimen.app_icon_size)
        Coil.setImageLoader(
            ImageLoader.Builder(this)
                .components {
                    add(AppIconKeyer())
                    add(AppIconFetcher.Factory(iconSize, false, this@MainActivity))
                }
                .build()
        )

        isLoading = false
    }
}

@Composable
private fun ShortcutIntentHandler(
    currentIntent: Intent?,
    navigator: com.ramcosta.composedestinations.navigation.DestinationsNavigator
) {
    val context = LocalContext.current
    var handled by remember(currentIntent) { mutableStateOf(false) }

    LaunchedEffect(currentIntent, handled) {
        if (handled) return@LaunchedEffect
        val intent = currentIntent ?: return@LaunchedEffect
        when (intent.getStringExtra("shortcut_type")) {
            "module_action" -> {
                val moduleId = intent.getStringExtra("module_id") ?: return@LaunchedEffect
                navigator.navigate(ExecuteAPMActionScreenDestination(moduleId)) {
                    launchSingleTop = true
                }
                handled = true
            }

            "module_webui" -> {
                val moduleId = intent.getStringExtra("module_id") ?: return@LaunchedEffect
                val moduleName = intent.getStringExtra("module_name") ?: moduleId
                val webIntent = Intent(context, WebUIActivity::class.java)
                    .setData("apatch://webui/$moduleId".toUri())
                    .putExtra("id", moduleId)
                    .putExtra("name", moduleName)
                    .putExtra("from_webui_shortcut", true)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                context.startActivity(webIntent)
                handled = true
            }
        }
    }
}


@Composable
private fun BottomBar(navController: NavHostController, visibleDestinations: List<BottomBarDestination>) {
    val navigator = navController.rememberDestinationsNavigator()
    val background = rememberBackgroundConfig()

    Crossfade(
        targetState = visibleDestinations,
        label = "BottomBarStateCrossfade"
    ) { visibleDestinations ->
        NavigationBar(
            tonalElevation = if (background.enabled) 0.dp else 8.dp,
            containerColor = if (background.enabled) Color.Transparent else MaterialTheme.colorScheme.surface
        ) {
            visibleDestinations.forEach { destination ->
                val isCurrentDestOnBackStack by navController.isRouteOnBackStackAsState(destination.direction)

                NavigationBarItem(
                    selected = isCurrentDestOnBackStack,
                    onClick = {
                        if (isCurrentDestOnBackStack) {
                            navigator.popBackStack(destination.direction, false)
                            return@NavigationBarItem
                        }
                        navigator.navigate(destination.direction) {
                            popUpTo(NavGraphs.root) {
                                saveState = true
                            }
                            launchSingleTop = true
                            restoreState = true
                        }
                    },
                    icon = {
                        if (isCurrentDestOnBackStack) {
                            Icon(destination.iconSelected, stringResource(destination.label))
                        } else {
                            Icon(destination.iconNotSelected, stringResource(destination.label))
                        }
                    },
                    label = {
                        Text(
                            text = stringResource(destination.label),
                            overflow = TextOverflow.Visible,
                            maxLines = 1,
                            softWrap = false
                        )
                    },
                    alwaysShowLabel = false
                )
            }
        }
    }
}

@Composable
private fun SideBar(navController: NavHostController, modifier: Modifier = Modifier, visibleDestinations: List<BottomBarDestination>) {
    val navigator = navController.rememberDestinationsNavigator()

    Crossfade(
        targetState = visibleDestinations,
        label = "SideBarStateCrossfade"
    ) { visibleDestinations ->
        NavigationRail(
            modifier = modifier,
            containerColor = MaterialTheme.colorScheme.background,
        ) {
            Column(
                modifier = Modifier.fillMaxHeight(),
                verticalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterVertically)
            ) {
                visibleDestinations.forEach { destination ->
                    val isCurrentDestOnBackStack by navController.isRouteOnBackStackAsState(destination.direction)
                    NavigationRailItem(
                        selected = isCurrentDestOnBackStack,
                        onClick = {
                            if (isCurrentDestOnBackStack) {
                                navigator.popBackStack(destination.direction, false)
                                return@NavigationRailItem
                            }
                            navigator.navigate(destination.direction) {
                                popUpTo(NavGraphs.root) {
                                    saveState = true
                                }
                                launchSingleTop = true
                                restoreState = true
                            }
                        },
                        icon = {
                            if (isCurrentDestOnBackStack) {
                                Icon(destination.iconSelected, stringResource(destination.label))
                            } else {
                                Icon(destination.iconNotSelected, stringResource(destination.label))
                            }
                        },
                        label = { Text(stringResource(destination.label)) },
                        alwaysShowLabel = false,
                        modifier = Modifier.padding(start = 8.dp)
                    )
                }
            }
        }
    }
}
