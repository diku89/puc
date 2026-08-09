# SPDX-License-Identifier: BSD-2-Clause

"""Test conversion with a minimal but cross-linked Doxygen XML graph.

The fixture deliberately includes a compound-to-compound reference, a member
anchor, and a source location. Together they guard the properties needed when
the generated Markdown directory is moved into ``docs/code`` without depending
on Doxygen's much larger real output.
"""

import tempfile
import unittest
from pathlib import Path

from utils.scripts import doxygen_xml_to_markdown


INDEX_XML = """\
<doxygenindex>
  <compound refid="class_widget" kind="class">
    <name>puc::Widget</name>
    <member refid="class_widget_run" kind="function"><name>run</name></member>
  </compound>
  <compound refid="struct_options" kind="struct">
    <name>puc::Options</name>
  </compound>
  <compound refid="puc_protocol_contracts" kind="page">
    <name>puc_protocol_contracts</name>
  </compound>
  <compound refid="namespace_shell_parser_artifact" kind="namespace">
    <name>format</name>
  </compound>
</doxygenindex>
"""

WIDGET_XML = """\
<doxygen>
  <compounddef id="class_widget" kind="class">
    <compoundname>puc::Widget</compoundname>
    <sectiondef kind="public-func">
      <memberdef id="class_widget_run" kind="function">
        <definition>void puc::Widget::run</definition><argsstring>()</argsstring>
        <name>run</name>
        <briefdescription><para>Run with <ref refid="struct_options">Options</ref>.</para></briefdescription>
        <detaileddescription/>
        <location file="widget.hpp" line="12"/>
      </memberdef>
    </sectiondef>
    <briefdescription><para>A widget.</para></briefdescription>
    <detaileddescription>
      <para><image type="html" name="../assets/puc-widget-run-options-demo.gif" alt="Widget run-options demo" inline="yes"></image></para>
      <para><xrefsect id="puc_protocol_contracts_1_entry">
        <xreftitle>Protocol Contract</xreftitle>
        <xrefdescription><para><bold>Kind:</bold> Channel <linebreak/>
          <bold>Name:</bold> <computeroutput>//widgets/run</computeroutput>
        </para></xrefdescription>
      </xrefsect></para>
    </detaileddescription>
    <location file="widget.hpp" line="5"/>
  </compounddef>
</doxygen>
"""

OPTIONS_XML = """\
<doxygen>
  <compounddef id="struct_options" kind="struct">
    <compoundname>puc::Options</compoundname>
    <briefdescription><para>Widget options.</para></briefdescription>
    <detaileddescription/>
    <location file="widget.hpp" line="1"/>
  </compounddef>
</doxygen>
"""

PROTOCOL_CONTRACTS_XML = """\
<doxygen>
  <compounddef id="puc_protocol_contracts" kind="page">
    <compoundname>puc_protocol_contracts</compoundname>
    <title>PUC Protocol Contracts</title>
    <briefdescription/>
    <detaileddescription>
      <para><variablelist>
        <varlistentry><term>Member <ref refid="class_widget_run">puc::Widget::run</ref></term></varlistentry>
        <listitem><para><anchor id="puc_protocol_contracts_1_entry"/>
          <bold>Kind:</bold> Channel <linebreak/>
          <bold>Name:</bold> <computeroutput>//widgets/run</computeroutput> <linebreak/>
          <bold>Purpose:</bold> Runs a widget.
        </para></listitem>
      </variablelist></para>
    </detaileddescription>
    <location file="puc_protocol_contracts"/>
  </compounddef>
</doxygen>
"""

EMPTY_NAMESPACE_XML = """\
<doxygen>
  <compounddef id="namespace_shell_parser_artifact" kind="namespace">
    <compoundname>format</compoundname>
    <briefdescription/>
    <detaileddescription/>
    <location file="format.sh" line="1"/>
  </compounddef>
</doxygen>
"""


class DoxygenXmlToMarkdownTest(unittest.TestCase):
    """Verify graph conversion succeeds atomically or rejects incomplete input."""

    def test_converts_compounds_and_preserves_cross_references(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            xml_dir = root / "xml"
            output_dir = root / "markdown"
            xml_dir.mkdir()
            (xml_dir / "index.xml").write_text(INDEX_XML, encoding="utf-8")
            (xml_dir / "class_widget.xml").write_text(WIDGET_XML, encoding="utf-8")
            (xml_dir / "struct_options.xml").write_text(OPTIONS_XML, encoding="utf-8")
            (xml_dir / "puc_protocol_contracts.xml").write_text(
                PROTOCOL_CONTRACTS_XML, encoding="utf-8"
            )
            (xml_dir / "namespace_shell_parser_artifact.xml").write_text(
                EMPTY_NAMESPACE_XML, encoding="utf-8"
            )

            doxygen_xml_to_markdown.convert(xml_dir, output_dir)

            index = (output_dir / "README.md").read_text(encoding="utf-8")
            widget = (output_dir / "class_widget.md").read_text(encoding="utf-8")
            contracts = (output_dir / "puc_protocol_contracts.md").read_text(
                encoding="utf-8"
            )
            self.assertIn("[`puc::Widget`](class_widget.md)", index)
            self.assertIn(
                "[`PUC Protocol Contracts`](puc_protocol_contracts.md)", index
            )
            self.assertIn("[Options](struct_options.md)", widget)
            self.assertIn(
                "![Widget run-options demo](../assets/puc-widget-run-options-demo.gif)",
                widget,
            )
            self.assertIn(
                "[Protocol Contract](puc_protocol_contracts.md#symbol-puc_protocol_contracts_1_entry)",
                widget,
            )
            self.assertIn('<a id="symbol-class_widget_run"></a>', widget)
            self.assertIn("[Source](../../widget.hpp#L12)", widget)
            self.assertIn("# PUC Protocol Contracts", contracts)
            self.assertIn(
                "## Member [puc::Widget::run](class_widget.md#symbol-class_widget_run)",
                contracts,
            )
            self.assertIn(
                '<a id="symbol-puc_protocol_contracts_1_entry"></a>', contracts
            )
            self.assertIn("**Kind:** Channel  \n**Name:** `//widgets/run`", contracts)
            self.assertNotIn("[Source]", contracts)
            self.assertNotIn("format", index)
            self.assertFalse(
                (output_dir / "namespace_shell_parser_artifact.md").exists()
            )

    def test_rejects_missing_index(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaisesRegex(ValueError, "Doxygen index not found"):
                doxygen_xml_to_markdown.convert(root / "xml", root / "markdown")


if __name__ == "__main__":
    unittest.main()
