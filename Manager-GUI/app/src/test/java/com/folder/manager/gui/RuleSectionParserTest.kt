package com.folder.manager.gui

import com.folder.manager.gui.ui.RuleSectionParser
import org.junit.Assert.*
import org.junit.Test

class RuleSectionParserTest {

    @Test
    fun `empty text returns empty list`() {
        assertTrue(RuleSectionParser.parse("").isEmpty())
    }

    @Test
    fun `parses single section with all rule types`() {
        val text = """
            [com.example.app]
            mode = blacklist
            enabled = true
            + DCIM/Camera
            - Download/secret
            DCIM/WeChat -> Pictures/WeChat
        """.trimIndent()
        val sections = RuleSectionParser.parse(text)
        assertEquals(1, sections.size)
        val s = sections[0]
        assertEquals("com.example.app", s.pkg)
        assertEquals("blacklist", s.mode)
        assertTrue(s.enabled)
        assertEquals(1, s.allowCount)
        assertEquals(1, s.blockCount)
        assertEquals(1, s.redirectCount)
    }

    @Test
    fun `parses multiple sections independently`() {
        val text = """
            [com.example.a]
            mode = whitelist
            + DCIM/
            + Pictures/

            [com.example.b]
            mode = blacklist
            - Download/private
        """.trimIndent()
        val sections = RuleSectionParser.parse(text)
        assertEquals(2, sections.size)
        assertEquals("com.example.a", sections[0].pkg)
        assertEquals(2, sections[0].allowCount)
        assertEquals(0, sections[0].blockCount)
        assertEquals("com.example.b", sections[1].pkg)
        assertEquals(0, sections[1].allowCount)
        assertEquals(1, sections[1].blockCount)
    }

    @Test
    fun `disabled section is detected`() {
        val text = """
            [com.example.app]
            enabled = false
            - Download/secret
        """.trimIndent()
        val sections = RuleSectionParser.parse(text)
        assertEquals(1, sections.size)
        assertFalse(sections[0].enabled)
    }

    @Test
    fun `comments are not counted as rules`() {
        val text = """
            [com.example.app]
            # this is a comment
            - Download/secret
        """.trimIndent()
        val sections = RuleSectionParser.parse(text)
        assertEquals(1, sections[0].blockCount)
    }

    @Test
    fun `redirect lines with arrow are counted`() {
        val text = """
            [com.example.app]
            DCIM/Camera -> Android/data/com.example.app/cache/Camera
            Download/*.pdf -> Documents/PDF/
        """.trimIndent()
        val sections = RuleSectionParser.parse(text)
        assertEquals(2, sections[0].redirectCount)
    }

    @Test
    fun `wildcard section is parsed`() {
        val text = """
            [*]
            /storage/emulated/0/Download/ -> /storage/emulated/0/Documents/Auto/
        """.trimIndent()
        val sections = RuleSectionParser.parse(text)
        assertEquals(1, sections.size)
        assertEquals("*", sections[0].pkg)
        assertEquals(1, sections[0].redirectCount)
    }

    @Test
    fun `lineStart tracks correct line index`() {
        val text = "[com.a]\n- path1\n\n[com.b]\n+ path2"
        val sections = RuleSectionParser.parse(text)
        assertEquals(2, sections.size)
        assertEquals(0, sections[0].lineStart)
        assertEquals(3, sections[1].lineStart)
    }
}
