package www.netproxy.web.ui.webview

import java.net.URLConnection

/**
 * MIME 类型工具类
 * 
 * 根据文件名猜测 MIME 类型，优先使用系统 API，
 * 回退到硬编码的扩展名映射表
 */
internal object MimeUtil {
    
    /**
     * 根据文件名获取 MIME 类型
     * @param fileName 文件名
     * @return MIME 类型，无法识别时返回 null
     */
    fun getMimeFromFileName(fileName: String?): String? {
        if (fileName == null) return null
        
        // 优先使用系统 API
        val mimeType = URLConnection.guessContentTypeFromName(fileName)
        if (mimeType != null) return mimeType
        
        return guessHardcodedMime(fileName)
    }
    
    /**
     * 使用硬编码映射表猜测 MIME 类型
     */
    private fun guessHardcodedMime(fileName: String): String? {
        val lastDot = fileName.lastIndexOf('.')
        if (lastDot == -1) return null
        
        val extension = fileName.substring(lastDot + 1).lowercase()
        
        return when (extension) {
            // 视频
            "webm" -> "video/webm"
            "mpeg", "mpg" -> "video/mpeg"
            "mp4", "m4v" -> "video/mp4"
            "ogv", "ogm" -> "video/ogg"
            
            // 音频
            "mp3" -> "audio/mpeg"
            "flac" -> "audio/flac"
            "ogg", "oga", "opus" -> "audio/ogg"
            "wav" -> "audio/wav"
            "m4a" -> "audio/x-m4a"
            
            // 图片
            "gif" -> "image/gif"
            "jpeg", "jpg", "jfif", "pjpeg", "pjp" -> "image/jpeg"
            "png" -> "image/png"
            "apng" -> "image/apng"
            "svg", "svgz" -> "image/svg+xml"
            "webp" -> "image/webp"
            "ico" -> "image/x-icon"
            "bmp" -> "image/bmp"
            "tiff", "tif" -> "image/tiff"
            
            // 文本
            "css" -> "text/css"
            "html", "htm", "shtml", "shtm", "ehtml" -> "text/html"
            "xml" -> "text/xml"
            
            // 应用
            "js", "mjs" -> "application/javascript"
            "json" -> "application/json"
            "wasm" -> "application/wasm"
            "xhtml", "xht", "xhtm" -> "application/xhtml+xml"
            "pdf" -> "application/pdf"
            "zip" -> "application/zip"
            "gz", "tgz" -> "application/gzip"
            "woff" -> "application/font-woff"
            
            // 其他
            "mht", "mhtml" -> "multipart/related"
            
            else -> null
        }
    }
}
