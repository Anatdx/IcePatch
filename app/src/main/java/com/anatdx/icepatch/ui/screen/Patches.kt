package com.anatdx.icepatch.ui.screen

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.wrapContentWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Healing
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Route
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.compose.material3.AlertDialogDefaults
import androidx.compose.material3.BasicAlertDialog
import androidx.compose.material3.Surface
import androidx.compose.material3.TextButton
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import com.anatdx.icepatch.util.ui.APDialogBlurBehindUtils
import androidx.compose.material3.Button
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.viewmodel.compose.viewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.anatdx.icepatch.R
import com.anatdx.icepatch.ui.viewmodel.KPModel
import com.anatdx.icepatch.ui.viewmodel.PatchesViewModel
import com.anatdx.icepatch.util.Version
import com.anatdx.icepatch.util.reboot

private const val TAG = "Patches"

@Destination<RootGraph>
@Composable
fun Patches(mode: PatchesViewModel.PatchMode, autoStartPatch: Boolean = false) {
    val scrollState = rememberScrollState()
    val scope = rememberCoroutineScope()

    val viewModel = viewModel<PatchesViewModel>(
        viewModelStoreOwner = LocalContext.current as FragmentActivity
    )
    SideEffect {
        viewModel.prepare(mode)
    }

    LaunchedEffect(autoStartPatch) {
        if (!autoStartPatch) return@LaunchedEffect
        if (viewModel.patching || viewModel.patchdone) return@LaunchedEffect
        when (mode) {
            PatchesViewModel.PatchMode.UNPATCH -> viewModel.doUnpatch()
            else -> if (viewModel.superkey.isNotEmpty()) viewModel.doPatch(mode)
        }
    }

    Scaffold(topBar = {
        TopBar()
    }, floatingActionButton = {
        if (viewModel.needReboot) {
            val reboot = stringResource(id = R.string.reboot)
            ExtendedFloatingActionButton(
                onClick = {
                    scope.launch {
                        withContext(Dispatchers.IO) {
                            reboot()
                        }
                    }
                },
                icon = { Icon(Icons.Filled.Refresh, reboot) },
                text = { Text(text = reboot) },
            )
        }
    }) { innerPadding ->
        Column(
            modifier = Modifier
                .padding(innerPadding)
                .padding(horizontal = 16.dp)
                .verticalScroll(scrollState),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            val context = LocalContext.current

            // request permissions
            val permissions = arrayOf(
                Manifest.permission.WRITE_EXTERNAL_STORAGE,
                Manifest.permission.READ_EXTERNAL_STORAGE
            )
            val permissionsToRequest = permissions.filter {
                ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
            }
            if (permissionsToRequest.isNotEmpty()) {
                ActivityCompat.requestPermissions(
                    context as Activity,
                    permissionsToRequest.toTypedArray(),
                    1001
                )
            }

            // 统一两页式：第二页只显示错误、日志、重启
            ErrorView(viewModel.error)

            // patch log
            if (viewModel.patching || viewModel.patchdone) {
                val clipboardManager = LocalClipboardManager.current
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End
                ) {
                    TextButton(
                        onClick = {
                            clipboardManager.setText(AnnotatedString(viewModel.patchLog))
                        },
                        enabled = viewModel.patchLog.isNotBlank()
                    ) {
                        Icon(
                            imageVector = Icons.Filled.ContentCopy,
                            contentDescription = null,
                            modifier = Modifier.size(16.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(text = stringResource(id = R.string.crash_handle_copy))
                    }
                }
                SelectionContainer {
                    Text(
                        modifier = Modifier.padding(8.dp),
                        text = viewModel.patchLog,
                        fontSize = MaterialTheme.typography.bodySmall.fontSize,
                        fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                        lineHeight = MaterialTheme.typography.bodySmall.lineHeight,
                    )
                }
                LaunchedEffect(viewModel.patchLog) {
                    scrollState.animateScrollTo(scrollState.maxValue)
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            // loading progress
            if (viewModel.running) {
                Box(
                    modifier = Modifier
                        .padding(innerPadding)
                        .align(Alignment.CenterHorizontally)
                ) {
                    CircularProgressIndicator(
                        modifier = Modifier
                            .size(50.dp)
                            .padding(16.dp)
                            .align(Alignment.BottomCenter)
                    )
                }
            }
        }
    }
}


@Composable
internal fun StartButton(text: String, onClick: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth(),
        horizontalAlignment = Alignment.End
    ) {
        Button(
            onClick = onClick,
            content = {
                Text(text = text)
            }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ExtraConfigDialog(kpmInfo: KPModel.KPMInfo, onDismiss: () -> Unit) {
    var event by remember { mutableStateOf(kpmInfo.event) }
    var args by remember { mutableStateOf(kpmInfo.args) }

    BasicAlertDialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(
            decorFitsSystemWindows = true,
            usePlatformDefaultWidth = false,
        )
    ) {
        Surface(
            modifier = Modifier
                .width(310.dp)
                .wrapContentHeight(),
            shape = RoundedCornerShape(30.dp),
            tonalElevation = AlertDialogDefaults.TonalElevation,
            color = AlertDialogDefaults.containerColor,
        ) {
            Column(modifier = Modifier.padding(PaddingValues(all = 24.dp))) {
                Text(
                    text = stringResource(id = R.string.kpm_control_dialog_title),
                    style = MaterialTheme.typography.headlineSmall,
                    modifier = Modifier.padding(bottom = 16.dp)
                )

                OutlinedTextField(
                    value = event,
                    onValueChange = {
                        event = it
                        kpmInfo.event = it
                    },
                    label = { Text(stringResource(id = R.string.patch_item_extra_event)) },
                    modifier = Modifier.fillMaxWidth()
                )

                Spacer(modifier = Modifier.height(16.dp))

                OutlinedTextField(
                    value = args,
                    onValueChange = {
                        args = it
                        kpmInfo.args = it
                    },
                    label = { Text(stringResource(id = R.string.patch_item_extra_args)) },
                    modifier = Modifier.fillMaxWidth()
                )

                Spacer(modifier = Modifier.height(24.dp))

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End
                ) {
                    TextButton(onClick = onDismiss) {
                        Text(stringResource(id = android.R.string.ok))
                    }
                }
            }
        }
        val dialogWindowProvider = LocalView.current.parent as DialogWindowProvider
        APDialogBlurBehindUtils.setupWindowBlurListener(dialogWindowProvider.window)
    }
}

@Composable
internal fun ExtraItem(extra: KPModel.IExtraInfo, existed: Boolean, onDelete: () -> Unit) {
    var showConfigDialog by remember { mutableStateOf(false) }

    if (showConfigDialog && extra is KPModel.KPMInfo) {
        ExtraConfigDialog(extra, onDismiss = { showConfigDialog = false })
    }

    ElevatedCard(
        colors = CardDefaults.elevatedCardColors(containerColor = run {
            MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 1f)
        }),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
        ) {
            Row(modifier = Modifier.align(Alignment.CenterHorizontally)) {
                Text(
                    text = stringResource(
                        id =
                            if (existed) R.string.patch_item_existed_extra_kpm else R.string.patch_item_new_extra_kpm
                    ) +
                            " " + extra.type.toString().uppercase(),
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier
                        .weight(1f)
                        .wrapContentWidth(Alignment.CenterHorizontally)
                )
                if (extra.type == KPModel.ExtraType.KPM) {
                    Icon(
                        imageVector = Icons.Default.Settings,
                        contentDescription = "Config",
                        modifier = Modifier
                            .padding(end = 8.dp)
                            .clickable { showConfigDialog = true }
                    )
                }
                Icon(
                    imageVector = Icons.Default.Delete,
                    contentDescription = "Delete",
                    modifier = Modifier
                        .padding(end = 8.dp)
                        .clickable { onDelete() })
            }
            if (extra.type == KPModel.ExtraType.KPM) {
                val kpmInfo: KPModel.KPMInfo = extra as KPModel.KPMInfo
                Text(
                    text = "${stringResource(id = R.string.patch_item_extra_name) + " "} ${kpmInfo.name}",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "${stringResource(id = R.string.patch_item_extra_version) + " "} ${kpmInfo.version}",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "${stringResource(id = R.string.patch_item_extra_kpm_license) + " "} ${kpmInfo.license}",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "${stringResource(id = R.string.patch_item_extra_author) + " "} ${kpmInfo.author}",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "${stringResource(id = R.string.patch_item_extra_kpm_desciption) + " "} ${kpmInfo.description}",
                    style = MaterialTheme.typography.bodyMedium
                )
            }
        }
    }
}


@Composable
internal fun SetSuperKeyView(viewModel: PatchesViewModel) {
    var skey by remember { mutableStateOf(viewModel.superkey) }
    var showWarn by remember { mutableStateOf(!viewModel.checkSuperKeyValidation(skey)) }
    var keyVisible by remember { mutableStateOf(false) }
    ElevatedCard(
        colors = CardDefaults.elevatedCardColors(containerColor = run {
            MaterialTheme.colorScheme.secondaryContainer
        })
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth(),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text(
                    text = stringResource(id = R.string.patch_item_skey),
                    style = MaterialTheme.typography.bodyLarge
                )
            }
            if (showWarn) {
                Spacer(modifier = Modifier.height(3.dp))
                Text(
                    color = Color.Red,
                    text = stringResource(id = R.string.patch_item_set_skey_label),
                    style = MaterialTheme.typography.bodyMedium
                )
            }
            Column {
                //Spacer(modifier = Modifier.height(8.dp))
                Box(
                    contentAlignment = Alignment.CenterEnd,
                ) {
                    OutlinedTextField(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(top = 6.dp),
                        value = skey,
                        label = { Text(stringResource(id = R.string.patch_set_superkey)) },
                        visualTransformation = if (keyVisible) VisualTransformation.None else PasswordVisualTransformation(),
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                        shape = RoundedCornerShape(50.0f),
                        onValueChange = {
                            skey = it
                            if (viewModel.checkSuperKeyValidation(it)) {
                                viewModel.superkey = it
                                showWarn = false
                            } else {
                                viewModel.superkey = ""
                                showWarn = true
                            }
                        },
                    )
                    IconButton(
                        modifier = Modifier
                            .size(40.dp)
                            .padding(top = 15.dp, end = 5.dp),
                        onClick = { keyVisible = !keyVisible }
                    ) {
                        Icon(
                            imageVector = if (keyVisible) Icons.Default.Visibility else Icons.Default.VisibilityOff,
                            contentDescription = null,
                            tint = Color.Gray
                        )
                    }
                }
            }
        }
    }
}

@Composable
internal fun PatchPolicyView(viewModel: PatchesViewModel) {
    ElevatedCard(
        colors = CardDefaults.elevatedCardColors(
            containerColor = MaterialTheme.colorScheme.secondaryContainer
        )
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                text = stringResource(R.string.patch_policy_title),
                style = MaterialTheme.typography.bodyLarge
            )
            Text(
                text = stringResource(R.string.patch_policy_summary),
                style = MaterialTheme.typography.bodySmall
            )

            PatchesViewModel.PolicyProfile.entries.forEach { option ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { viewModel.policyProfile = option },
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    RadioButton(
                        selected = viewModel.policyProfile == option,
                        onClick = { viewModel.policyProfile = option }
                    )
                    Text(
                        text = stringResource(option.labelRes),
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }

            if (viewModel.policyProfile != PatchesViewModel.PolicyProfile.MINIMAL) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Column(
                        modifier = Modifier
                            .weight(1f)
                            .padding(end = 12.dp)
                    ) {
                        Text(
                            text = stringResource(R.string.patch_policy_no_su),
                            style = MaterialTheme.typography.bodyMedium
                        )
                        Text(
                            text = stringResource(R.string.patch_policy_no_su_summary),
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                    Switch(
                        checked = viewModel.policyNoSu,
                        onCheckedChange = { viewModel.policyNoSu = it }
                    )
                }
            }

            Button(
                modifier = Modifier.fillMaxWidth(),
                onClick = { viewModel.applyPolicyNow() },
                enabled = !viewModel.policyApplyingNow && viewModel.checkSuperKeyValidation(viewModel.superkey),
            ) {
                if (viewModel.policyApplyingNow) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(16.dp),
                        strokeWidth = 2.dp,
                        color = MaterialTheme.colorScheme.onPrimary
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                }
                Text(text = stringResource(R.string.patch_policy_apply_now))
            }

            if (viewModel.policyApplyNowLog.isNotBlank()) {
                Text(
                    text = stringResource(R.string.patch_policy_apply_now_result),
                    style = MaterialTheme.typography.bodySmall
                )
                SelectionContainer {
                    Text(
                        text = viewModel.policyApplyNowLog,
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
        }
    }
}

private val LINUX_VERSION_REGEX = Regex("""Linux version ([^(]+)""")

private fun formatKernelBanner(banner: String): String =
    LINUX_VERSION_REGEX.find(banner)?.groupValues?.get(1)?.trim() ?: banner

private fun formatKpimgVersion(version: String): String {
    val raw = version.trim()
    if (raw.isEmpty()) return raw
    if (raw.contains('.')) return raw
    val hex = raw.removePrefix("0x").removePrefix("0X")
    return runCatching { Version.uInt2String(hex.toUInt(16)) }.getOrElse { raw }
}

@Composable
internal fun PatchInfoCard(
    kpimgInfo: KPModel.KPImgInfo,
    bootSlot: String,
    kimgInfo: KPModel.KImgInfo
) {
    val hasKp = kpimgInfo.version.isNotEmpty()
    val hasSlot = bootSlot.isNotEmpty()
    val hasKernel = kimgInfo.banner.isNotEmpty()
    if (!hasKp && !hasSlot && !hasKernel) return

    ElevatedCard(
        colors = CardDefaults.elevatedCardColors(containerColor = run {
            MaterialTheme.colorScheme.secondaryContainer
        })
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            if (hasKp) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(
                        imageVector = Icons.Filled.Healing,
                        contentDescription = null,
                        modifier = Modifier.size(28.dp)
                    )
                    Spacer(modifier = Modifier.width(12.dp))
                    Text(
                        text = stringResource(id = R.string.patch_item_kpimg_version) + " " +
                            formatKpimgVersion(kpimgInfo.version) + "  " +
                            stringResource(id = R.string.patch_item_kpimg_comile_time) + " " + kpimgInfo.compileTime,
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }
            if (hasSlot) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(
                        imageVector = Icons.Filled.Route,
                        contentDescription = null,
                        modifier = Modifier.size(28.dp)
                    )
                    Spacer(modifier = Modifier.width(12.dp))
                    Text(
                        text = stringResource(id = R.string.patch_item_bootimg_slot) + " " + bootSlot,
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }
            if (hasKernel) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(
                        imageVector = Icons.Filled.Memory,
                        contentDescription = null,
                        modifier = Modifier.size(28.dp)
                    )
                    Spacer(modifier = Modifier.width(12.dp))
                    Text(
                        text = formatKernelBanner(kimgInfo.banner),
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }
        }
    }
}


@Composable
internal fun SelectFileButton(text: String, onSelected: (data: Intent, uri: Uri) -> Unit) {
    val selectFileLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) {
        if (it.resultCode != Activity.RESULT_OK) {
            return@rememberLauncherForActivityResult
        }
        val data = it.data ?: return@rememberLauncherForActivityResult
        val uri = data.data ?: return@rememberLauncherForActivityResult
        onSelected(data, uri)
    }

    Button(
        modifier = Modifier.fillMaxWidth(),
        onClick = {
            val intent = Intent(Intent.ACTION_GET_CONTENT)
            intent.type = "*/*"
            selectFileLauncher.launch(intent)
        },
        content = { Text(text = text) }
    )
}

@Composable
internal fun ErrorView(error: String) {
    if (error.isEmpty()) return
    ElevatedCard(
        colors = CardDefaults.elevatedCardColors(containerColor = run {
            MaterialTheme.colorScheme.error
        })
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 12.dp, top = 12.dp, end = 12.dp, bottom = 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = stringResource(id = R.string.patch_item_error),
                style = MaterialTheme.typography.bodyLarge
            )
            Text(text = error, style = MaterialTheme.typography.bodyMedium)
        }
    }
}

@Composable
internal fun PatchMode(mode: PatchesViewModel.PatchMode) {
    ElevatedCard(
        colors = CardDefaults.elevatedCardColors(containerColor = run {
            MaterialTheme.colorScheme.secondaryContainer
        })
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(text = stringResource(id = mode.sId), style = MaterialTheme.typography.bodyLarge)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar() {
    TopAppBar(title = { Text(stringResource(R.string.patch_config_title)) })
}
