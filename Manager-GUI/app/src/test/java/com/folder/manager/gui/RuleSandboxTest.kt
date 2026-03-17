package com.folder.manager.gui

import com.folder.manager.gui.ui.RuleSandbox
import org.junit.Assert.*
import org.junit.Test

class RuleSandboxTest {

    private val basicBlacklist = """
        [com.example.app]
        mode = blacklist
        - Download/secret
        + DCIM/Camera
        DCIM/WeChat -> Pictures/WeChat
    """.trimIndent()

    private val whitelist = """
        [com.example.app]
        mode = whitelist
        + DCIM/Camera
        + Pictures/
    """.trimIndent()

    @Test
    fun `block rule matches exact path`() {
        val result = RuleSandbox.test(basicBlacklist, "com.example.app", "Download/secret")
        assertEquals("BLOCK", result.action)
        assertTrue(result.matched)
    }

    @Test
    fun `block rule matches child path`() {
        val result = RuleSandbox.test(basicBlacklist, "com.example.app", "Download/secret/file.txt")
        assertEquals("BLOCK", result.action)
        assertTrue(result.matched)
    }

    @Test
    fun `redirect rule returns target path`() {
        val result = RuleSandbox.test(basicBlacklist, "com.example.app", "DCIM/WeChat/img.jpg")
        assertEquals("REDIRECT", result.action)
        assertTrue(result.matched)
        assertEquals("Pictures/WeChat", result.target)
    }

    @Test
    fun `allow rule matches in blacklist mode`() {
        val result = RuleSandbox.test(basicBlacklist, "com.example.app", "DCIM/Camera/photo.jpg")
        assertEquals("ALLOW", result.action)
        assertTrue(result.matched)
    }

    @Test
    fun `unmatched path in blacklist defaults to ALLOW`() {
        val result = RuleSandbox.test(basicBlacklist, "com.example.app", "Music/songs")
        assertEquals("ALLOW", result.action)
        assertFalse(result.matched)
    }

    @Test
    fun `unmatched path in whitelist defaults to BLOCK`() {
        val result = RuleSandbox.test(whitelist, "com.example.app", "Download/file.apk")
        assertEquals("BLOCK", result.action)
        assertFalse(result.matched)
    }

    @Test
    fun `matched path in whitelist returns ALLOW`() {
        val result = RuleSandbox.test(whitelist, "com.example.app", "DCIM/Camera/photo.jpg")
        assertEquals("ALLOW", result.action)
        assertTrue(result.matched)
    }

    @Test
    fun `unknown package returns NO_MATCH`() {
        val result = RuleSandbox.test(basicBlacklist, "com.unknown.app", "Download/secret")
        assertEquals("NO_MATCH", result.action)
        assertFalse(result.matched)
    }

    @Test
    fun `disabled section returns NO_MATCH`() {
        val text = """
            [com.example.app]
            enabled = false
            - Download/secret
        """.trimIndent()
        val result = RuleSandbox.test(text, "com.example.app", "Download/secret")
        assertEquals("NO_MATCH", result.action)
    }

    @Test
    fun `wildcard section matches any package`() {
        val text = """
            [*]
            - Download/secret
        """.trimIndent()
        val result = RuleSandbox.test(text, "com.any.app", "Download/secret")
        assertEquals("BLOCK", result.action)
        assertTrue(result.matched)
    }

    @Test
    fun `pkg placeholder in redirect target is resolved`() {
        val text = """
            [com.example.app]
            DCIM/Camera -> Android/data/<pkg>/cache/Camera
        """.trimIndent()
        val result = RuleSandbox.test(text, "com.example.app", "DCIM/Camera/photo.jpg")
        assertEquals("REDIRECT", result.action)
        assertEquals("Android/data/com.example.app/cache/Camera", result.target)
    }

    @Test
    fun `redirect takes priority over block`() {
        val text = """
            [com.example.app]
            mode = blacklist
            - DCIM/Camera
            DCIM/Camera -> Pictures/Sorted
        """.trimIndent()
        val result = RuleSandbox.test(text, "com.example.app", "DCIM/Camera/photo.jpg")
        // redirect 优先于 block
        assertEquals("REDIRECT", result.action)
    }

    @Test
    fun `leading slash in path is normalized`() {
        val result = RuleSandbox.test(basicBlacklist, "com.example.app", "/Download/secret")
        assertEquals("BLOCK", result.action)
    }
}
