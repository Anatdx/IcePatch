package com.anatdx.icepatch.ui.screen

import android.app.Activity
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.StringRes
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.compose.dropUnlessResumed
import androidx.lifecycle.viewmodel.compose.viewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.PatchesDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.icepatch.R
import com.anatdx.icepatch.ui.component.rememberConfirmDialog
import com.anatdx.icepatch.ui.viewmodel.PatchesViewModel
import com.anatdx.icepatch.util.isABDevice
import com.anatdx.icepatch.util.rootAvailable

var selectedBootImage: Uri? = null
var pendingEmbedKpmUri: Uri? = null

private fun installMethodToMode(m: InstallMethod?): PatchesViewModel.PatchMode? = when (m) {
    is InstallMethod.DirectInstall -> PatchesViewModel.PatchMode.PATCH_AND_INSTALL
    is InstallMethod.DirectInstallToInactiveSlot -> PatchesViewModel.PatchMode.INSTALL_TO_NEXT_SLOT
    is InstallMethod.SelectFile -> PatchesViewModel.PatchMode.PATCH_ONLY
    is InstallMethod.Unpatch -> PatchesViewModel.PatchMode.UNPATCH
    null -> null
}

@Destination<RootGraph>
@Composable
fun InstallModeSelectScreen(
    navigator: DestinationsNavigator,
    presetMode: PatchesViewModel.PatchMode? = null
) {
    var installMethod by remember { mutableStateOf<InstallMethod?>(null) }
    val context = LocalContext.current
    val activity = context as FragmentActivity
    val viewModel = viewModel<PatchesViewModel>(viewModelStoreOwner = activity)

    LaunchedEffect(presetMode) {
        when (presetMode) {
            PatchesViewModel.PatchMode.UNPATCH -> installMethod = InstallMethod.Unpatch
            else -> Unit
        }
    }
    LaunchedEffect(Unit) {
        if (pendingEmbedKpmUri != null) {
            viewModel.pendingEmbedKpmUri = pendingEmbedKpmUri
            pendingEmbedKpmUri = null
            installMethod = InstallMethod.DirectInstall
        }
    }

    LaunchedEffect(installMethod) {
        when (val m = installMethod) {
            is InstallMethod.DirectInstall -> viewModel.prepare(PatchesViewModel.PatchMode.PATCH_AND_INSTALL)
            is InstallMethod.DirectInstallToInactiveSlot -> viewModel.prepare(PatchesViewModel.PatchMode.INSTALL_TO_NEXT_SLOT)
            is InstallMethod.SelectFile -> m.uri?.let { viewModel.copyAndParseBootimg(it) }
            is InstallMethod.Unpatch -> viewModel.prepare(PatchesViewModel.PatchMode.UNPATCH)
            null -> Unit
        }
    }

    val selectImageLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) {
        if (it.resultCode == Activity.RESULT_OK) {
            it.data?.data?.let { uri ->
                installMethod = InstallMethod.SelectFile(uri)
                selectedBootImage = uri
            }
        } else {
            installMethod = null
            selectedBootImage = null
        }
    }

    val nextConfirmDialog = rememberConfirmDialog(
        onConfirm = {
            navigator.navigate(PatchesDestination(PatchesViewModel.PatchMode.INSTALL_TO_NEXT_SLOT, autoStartPatch = true))
        },
        onDismiss = null
    )
    val nextWarningTitle = stringResource(android.R.string.dialog_alert_title)
    val nextWarningContent = stringResource(R.string.mode_select_page_install_inactive_slot_warning)

    val mode = installMethodToMode(installMethod)
    val canProceed = when (mode) {
        PatchesViewModel.PatchMode.UNPATCH -> viewModel.kimgInfo.banner.isNotEmpty()
        else -> mode != null &&
            viewModel.kimgInfo.banner.isNotEmpty() &&
            viewModel.checkSuperKeyValidation(viewModel.superkey)
    }

    val onNext = {
        when (val m = installMethod) {
            is InstallMethod.DirectInstall ->
                navigator.navigate(PatchesDestination(PatchesViewModel.PatchMode.PATCH_AND_INSTALL, autoStartPatch = true))
            is InstallMethod.DirectInstallToInactiveSlot ->
                nextConfirmDialog.showConfirm(nextWarningTitle, nextWarningContent)
            is InstallMethod.SelectFile -> {
                m.uri?.let { selectedBootImage = it }
                navigator.navigate(PatchesDestination(PatchesViewModel.PatchMode.PATCH_ONLY, autoStartPatch = true))
            }
            is InstallMethod.Unpatch ->
                navigator.navigate(PatchesDestination(PatchesViewModel.PatchMode.UNPATCH, autoStartPatch = true))
            null -> { }
        }
    }

    val onBack: () -> Unit = {
        viewModel.clearCache()
        selectedBootImage = null
        pendingEmbedKpmUri = null
        navigator.popBackStack()
    }
    BackHandler(onBack = onBack)
    Scaffold(topBar = {
        TopBar(onBack = dropUnlessResumed { onBack() })
    }) { paddingValues ->
        Column(
            modifier = Modifier
                .padding(paddingValues)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp)
        ) {
            Spacer(Modifier.height(8.dp))
            SelectInstallMethod(
                selectedMethod = installMethod,
                onSelected = { installMethod = it },
                onRequestSelectFile = {
                    selectImageLauncher.launch(
                        Intent(Intent.ACTION_GET_CONTENT).apply {
                            type = "application/octet-stream"
                        }
                    )
                }
            )

            AnimatedVisibility(
                visible = installMethod != null,
                enter = fadeIn() + expandVertically(),
                exit = shrinkVertically() + fadeOut()
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    if (mode != null) {
                        ErrorView(viewModel.error)
                        PatchInfoCard(
                            kpimgInfo = viewModel.kpimgInfo,
                            bootSlot = viewModel.bootSlot,
                            kimgInfo = viewModel.kimgInfo
                        )

                        if (installMethod is InstallMethod.SelectFile && viewModel.kimgInfo.banner.isEmpty()) {
                            SelectFileButton(
                                text = stringResource(R.string.patch_select_bootimg_btn),
                                onSelected = { _, uri ->
                                    installMethod = InstallMethod.SelectFile(uri)
                                    selectedBootImage = uri
                                    viewModel.copyAndParseBootimg(uri)
                                }
                            )
                        }

                        if (mode != PatchesViewModel.PatchMode.UNPATCH) {
                            SetSuperKeyView(viewModel)
                            PatchPolicyView(viewModel)
                        }

                        if (mode == PatchesViewModel.PatchMode.PATCH_AND_INSTALL || mode == PatchesViewModel.PatchMode.INSTALL_TO_NEXT_SLOT) {
                            viewModel.existedExtras.forEach { extra ->
                                ExtraItem(extra = extra, existed = true, onDelete = {
                                    viewModel.existedExtras.remove(extra)
                                })
                            }
                        }

                        viewModel.newExtras.forEach { extra ->
                            ExtraItem(extra = extra, existed = false, onDelete = {
                                val idx = viewModel.newExtras.indexOf(extra)
                                if (idx >= 0) {
                                    viewModel.newExtras.removeAt(idx)
                                    if (idx < viewModel.newExtrasFileName.size) {
                                        viewModel.newExtrasFileName.removeAt(idx)
                                    }
                                }
                            })
                        }

                        if (viewModel.superkey.isNotEmpty() && mode != PatchesViewModel.PatchMode.UNPATCH) {
                            SelectFileButton(
                                text = stringResource(R.string.patch_embed_kpm_btn),
                                onSelected = { _, uri -> viewModel.embedKPM(uri) }
                            )
                        }
                    }

                    Button(
                        modifier = Modifier.fillMaxWidth(),
                        enabled = canProceed,
                        onClick = onNext,
                        shape = RoundedCornerShape(12.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.primary,
                            contentColor = MaterialTheme.colorScheme.onPrimary,
                            disabledContainerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.6f),
                            disabledContentColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                        )
                    ) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.Center,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                stringResource(R.string.mode_select_page_next),
                                style = MaterialTheme.typography.bodyLarge
                            )
                            Spacer(Modifier.width(8.dp))
                            Icon(Icons.Filled.ChevronRight, contentDescription = null)
                        }
                    }
                }
            }
        }
    }
}

sealed class InstallMethod {
    data class SelectFile(
        val uri: Uri? = null,
        @param:StringRes override val label: Int = R.string.mode_select_page_select_file,
    ) : InstallMethod()

    data object DirectInstall : InstallMethod() {
        override val label: Int
            get() = R.string.mode_select_page_patch_and_install
    }

    data object DirectInstallToInactiveSlot : InstallMethod() {
        override val label: Int
            get() = R.string.mode_select_page_install_inactive_slot
    }

    data object Unpatch : InstallMethod() {
        override val label: Int
            get() = R.string.patch_mode_uninstall_patch
    }

    abstract val label: Int
    open val summary: String? = null
}

@Composable
private fun SelectInstallMethod(
    selectedMethod: InstallMethod?,
    onSelected: (InstallMethod) -> Unit,
    onRequestSelectFile: () -> Unit,
) {
    val rootAvailable = rootAvailable()
    val isAbDevice = isABDevice()

    val radioOptions =
        mutableListOf<InstallMethod>(InstallMethod.SelectFile())
    if (rootAvailable) {
        radioOptions.add(InstallMethod.DirectInstall)
        if (isAbDevice) {
            radioOptions.add(InstallMethod.DirectInstallToInactiveSlot)
        }
        radioOptions.add(InstallMethod.Unpatch)
    }

    val confirmInactiveSlotDialog = rememberConfirmDialog(
        onConfirm = { onSelected(InstallMethod.DirectInstallToInactiveSlot) },
        onDismiss = null
    )
    val dialogTitle = stringResource(android.R.string.dialog_alert_title)
    val dialogContent = stringResource(R.string.mode_select_page_install_inactive_slot_warning)

    val onClick = { option: InstallMethod ->
        when (option) {
            is InstallMethod.SelectFile -> {
                selectedBootImage = null
                onSelected(InstallMethod.SelectFile(null))
                onRequestSelectFile()
            }
            is InstallMethod.DirectInstall -> onSelected(option)
            is InstallMethod.DirectInstallToInactiveSlot ->
                confirmInactiveSlotDialog.showConfirm(dialogTitle, dialogContent)
            is InstallMethod.Unpatch -> onSelected(option)
        }
    }

    Column(verticalArrangement = Arrangement.spacedBy(0.dp)) {
        radioOptions.forEach { option ->
            val isSelected = when {
                selectedMethod == null -> false
                option is InstallMethod.SelectFile && selectedMethod is InstallMethod.SelectFile -> true
                option is InstallMethod.DirectInstall && selectedMethod is InstallMethod.DirectInstall -> true
                option is InstallMethod.DirectInstallToInactiveSlot && selectedMethod is InstallMethod.DirectInstallToInactiveSlot -> true
                option is InstallMethod.Unpatch && selectedMethod is InstallMethod.Unpatch -> true
                else -> false
            }
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onClick(option) }
                    .padding(vertical = 4.dp)
            ) {
                RadioButton(
                    selected = isSelected,
                    onClick = { onClick(option) }
                )
                Column(modifier = Modifier.padding(start = 8.dp)) {
                    Text(
                        text = stringResource(id = option.label),
                        fontSize = MaterialTheme.typography.titleMedium.fontSize,
                        fontFamily = MaterialTheme.typography.titleMedium.fontFamily,
                        fontStyle = MaterialTheme.typography.titleMedium.fontStyle
                    )
                    option.summary?.let {
                        Text(
                            text = it,
                            fontSize = MaterialTheme.typography.bodySmall.fontSize,
                            fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                            fontStyle = MaterialTheme.typography.bodySmall.fontStyle
                        )
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(onBack: () -> Unit = {}) {
    TopAppBar(
        title = { Text(stringResource(R.string.mode_select_page_title)) },
        navigationIcon = {
            IconButton(onClick = onBack) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
            }
        },
    )
}
