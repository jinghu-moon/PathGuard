package com.folder.manager.gui

import com.folder.manager.gui.data.RulesDiff
import org.junit.Assert.*
import org.junit.Test

class RulesDiffTest {

    private fun diff(old: String, new: String) = RulesDiff.diff(old, new)

    @Test
    fun `identical texts produce zero diff`() {
        val text = "[com.example.app]\nmode = blacklist\n- Download/secret"
        val result = diff(text, text)
        assertEquals(0, result.added)
        assertEquals(0, result.removed)
        assertTrue(result.lines.all { it.type == RulesDiff.DiffType.UNCHANGED })
    }

    @Test
    fun `adding a line is detected as ADDED`() {
        val old = "[com.example.app]\nmode = blacklist"
        val new = "[com.example.app]\nmode = blacklist\n- Download/secret"
        val result = diff(old, new)
        assertEquals(1, result.added)
        assertEquals(0, result.removed)
        val added = result.lines.filter { it.type == RulesDiff.DiffType.ADDED }
        assertEquals(1, added.size)
        assertEquals("- Download/secret", added[0].line)
    }

    @Test
    fun `removing a line is detected as REMOVED`() {
        val old = "[com.example.app]\nmode = blacklist\n- Download/secret"
        val new = "[com.example.app]\nmode = blacklist"
        val result = diff(old, new)
        assertEquals(0, result.added)
        assertEquals(1, result.removed)
        val removed = result.lines.filter { it.type == RulesDiff.DiffType.REMOVED }
        assertEquals("- Download/secret", removed[0].line)
    }

    @Test
    fun `empty old text treats all lines as ADDED`() {
        val new = "[com.example.app]\n- Download/secret"
        val result = diff("", new)
        // Kotlin "".lines() == [""]，所以空串被视为1行空行
        assertEquals(2, result.added)
        assertTrue(result.removed <= 1) // 最多1行空行被移除
    }

    @Test
    fun `empty new text treats all lines as REMOVED`() {
        val old = "[com.example.app]\n- Download/secret"
        val result = diff(old, "")
        // Kotlin "".lines() == [""]，空串被视为1行
        assertEquals(2, result.removed)
        assertTrue(result.added <= 1)
    }

    @Test
    fun `line numbers are tracked correctly`() {
        val old = "line1\nline2\nline3"
        val new = "line1\nline3"
        val result = diff(old, new)
        val removed = result.lines.filter { it.type == RulesDiff.DiffType.REMOVED }
        assertEquals(1, removed.size)
        assertEquals("line2", removed[0].line)
        assertEquals(2, removed[0].lineNo)
    }

    @Test
    fun `added count matches result lines count`() {
        val old = "a\nb\nc"
        val new = "a\nb\nc\nd\ne"
        val result = diff(old, new)
        assertEquals(result.added, result.lines.count { it.type == RulesDiff.DiffType.ADDED })
        assertEquals(result.removed, result.lines.count { it.type == RulesDiff.DiffType.REMOVED })
    }
}
