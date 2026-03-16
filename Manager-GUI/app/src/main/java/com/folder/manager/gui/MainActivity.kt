package com.folder.manager.gui

import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.DocumentsContract
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import com.folder.manager.gui.ui.MainScreen
import com.folder.manager.gui.ui.theme.GeistTheme
import java.util.concurrent.Executors

class MainActivity : ComponentActivity() {

    private val executor = Executors.newSingleThreadExecutor()
    private var onTreeUriPicked: ((String) -> Unit)? = null
    private var onImportContent: ((String) -> Unit)? = null
    private var onExportReady: (() -> String)? = null

    private val pickTreeLauncher =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri == null) return@registerForActivityResult
            val path = resolveTreeUriToPath(uri) ?: return@registerForActivityResult
            onTreeUriPicked?.invoke(path)
        }

    private val importLauncher =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri == null) return@registerForActivityResult
            executor.execute {
                val content = readContentUri(uri) ?: return@execute
                Handler(Looper.getMainLooper()).post { onImportContent?.invoke(content) }
            }
        }

    private val exportLauncher =
        registerForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri ->
            if (uri == null) return@registerForActivityResult
            executor.execute { writeContentUri(uri, onExportReady?.invoke() ?: "") }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            GeistTheme {
                MainScreen(
                    executor     = executor,
                    onPickDir    = { cb -> onTreeUriPicked = cb; pickTreeLauncher.launch(null) },
                    onImport     = { cb -> onImportContent = cb; importLauncher.launch(arrayOf("text/*")) },
                    onExport     = { gc -> onExportReady = gc; exportLauncher.launch("rules.ini") },
                    onGetPackages = {
                        packageManager.getInstalledApplications(0)
                            .map { "${packageManager.getApplicationLabel(it)} (${it.packageName})" }
                            .sorted()
                    },
                )
            }
        }
    }

    override fun onDestroy() { executor.shutdownNow(); super.onDestroy() }

    private fun resolveTreeUriToPath(uri: Uri): String? {
        if (!DocumentsContract.isTreeUri(uri)) return null
        val docId = DocumentsContract.getTreeDocumentId(uri)
        if (!docId.startsWith("primary:", ignoreCase = true)) return null
        val sub = docId.removePrefix("primary:")
        return if (sub.isBlank()) EXTERNAL_ROOT else "$EXTERNAL_ROOT/$sub"
    }

    private fun readContentUri(uri: Uri): String? = runCatching {
        contentResolver.openInputStream(uri)?.use { it.bufferedReader().readText() }
    }.getOrNull()

    private fun writeContentUri(uri: Uri, content: String) = runCatching {
        contentResolver.openOutputStream(uri)?.use { it.write(content.toByteArray(Charsets.UTF_8)) }
    }.isSuccess

    companion object { private const val EXTERNAL_ROOT = "/storage/emulated/0" }
}
