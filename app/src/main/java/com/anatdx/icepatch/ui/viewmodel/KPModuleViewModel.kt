package com.anatdx.icepatch.ui.viewmodel

import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.anatdx.icepatch.Natives
import com.anatdx.icepatch.util.HanziToPinyin
import java.text.Collator
import java.util.Locale

class KPModuleViewModel : ViewModel() {
    companion object {
        private const val TAG = "KPModuleViewModel"
        private var modules by mutableStateOf<List<KPModel.KPMInfo>>(emptyList())
    }

    private var filterJob: Job? = null
    private var fetchJob: Job? = null

    private var _search by mutableStateOf("")
    var search: String
        get() = _search
        set(value) {
            _search = value
            recomputeModuleList()
        }

    var isRefreshing by mutableStateOf(false)
        private set

    var moduleList by mutableStateOf<List<KPModel.KPMInfo>>(emptyList())
        private set

    var isNeedRefresh by mutableStateOf(false)
        private set

    init {
        recomputeModuleList()
    }

    fun markNeedRefresh() {
        isNeedRefresh = true
    }

    fun fetchModuleList() {
        if (fetchJob?.isActive == true) return

        fetchJob = viewModelScope.launch(Dispatchers.IO) {
            withContext(Dispatchers.Main) {
                isRefreshing = true
            }

            val start = SystemClock.elapsedRealtime()
            val oldModuleList = modules

            val newModules = kotlin.runCatching {
                var names = Natives.kernelPatchModuleList()
                if (Natives.kernelPatchModuleNum() <= 0) {
                    names = ""
                }

                names
                    .lineSequence()
                    .map { it.trim() }
                    .filter { it.isNotEmpty() }
                    .map { moduleId ->
                        val infoLines = Natives.kernelPatchModuleInfo(moduleId).lineSequence().toList()
                        val name = infoLines.find { it.startsWith("name=") }?.removePrefix("name=")
                        val version = infoLines.find { it.startsWith("version=") }?.removePrefix("version=")
                        val license = infoLines.find { it.startsWith("license=") }?.removePrefix("license=")
                        val author = infoLines.find { it.startsWith("author=") }?.removePrefix("author=")
                        val description = infoLines.find { it.startsWith("description=") }
                            ?.removePrefix("description=")
                        val args = infoLines.find { it.startsWith("args=") }?.removePrefix("args=")

                        KPModel.KPMInfo(
                            KPModel.ExtraType.KPM,
                            name.orEmpty(),
                            "",
                            args.orEmpty(),
                            version.orEmpty(),
                            license.orEmpty(),
                            author.orEmpty(),
                            description.orEmpty()
                        )
                    }
                    .toList()
            }.onFailure { e ->
                Log.e(TAG, "fetchModuleList: ", e)
            }.getOrElse {
                oldModuleList
            }

            withContext(Dispatchers.Main) {
                modules = newModules
                isNeedRefresh = false
                isRefreshing = false
                recomputeModuleList()
            }

            Log.i(
                TAG,
                "load cost: ${SystemClock.elapsedRealtime() - start}, modules: ${newModules.size}"
            )
        }
    }

    private fun recomputeModuleList() {
        val query = search.trim()
        val source = modules
        filterJob?.cancel()
        filterJob = viewModelScope.launch(Dispatchers.Default) {
            val pinyin = HanziToPinyin.getInstance()
            val comparator = compareBy(
                comparator = Collator.getInstance(Locale.getDefault()),
                selector = KPModel.KPMInfo::name
            )

            val filtered = source.filter { module ->
                query.isBlank() || module.name.contains(query, ignoreCase = true) ||
                    (pinyin.toPinyinString(module.name)?.contains(query, ignoreCase = true) == true)
            }.sortedWith(comparator)

            withContext(Dispatchers.Main) {
                moduleList = filtered
            }
        }
    }
}
