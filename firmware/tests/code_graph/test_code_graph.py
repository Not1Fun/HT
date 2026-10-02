"""Focused checks for source-map parsing boundaries."""

import importlib.util
import shutil
import subprocess
import sys
import unittest
import uuid
from contextlib import contextmanager
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "code_graph.py"
SPEC = importlib.util.spec_from_file_location("code_graph", SCRIPT)
GRAPH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GRAPH)


@contextmanager
def temporary_source_tree():
    base = Path(__file__).resolve().parent
    temporary = base / ("_graph_test_" + uuid.uuid4().hex)
    temporary.mkdir()
    try:
        yield temporary
    finally:
        if not temporary.resolve().is_relative_to(base):
            raise ValueError("test cleanup escaped its directory")
        shutil.rmtree(temporary)


class CodeGraphTests(unittest.TestCase):
    def test_comments_literals_macros_and_prototypes_do_not_make_edges(self):
        source = r'''
#include "module.h"
// #include "fake.h"
#define WRAPPED() real()
void prototype(void);
/* void fake(void) { real(); } */
static void real(void) {}
void run(void) {
    const char *message = "real(); fake() { }";
    /* real(); */
    real();
    WRAPPED();
}
'''
        includes, functions, calls = GRAPH.parse_source(source, "firmware/src/test.c")
        self.assertEqual(includes, ["module.h"])
        self.assertEqual([item["name"] for item in functions], ["real", "run"])
        self.assertEqual([(item["name"], item["kind"]) for item in calls],
                         [("real", "candidate"), ("WRAPPED", "macro")])
        self.assertEqual(functions[0]["linkage"], "static")

    def test_direct_isr_and_indirect_calls(self):
        source = '''
void run(void (*callback)(void)) { callback(); (*callback)(); object->work(); }
ISR_DIRECT_DECLARE(stop_irq) { stop_output(); return 0; }
'''
        _, functions, calls = GRAPH.parse_source(source, "firmware/src/test.c")
        self.assertEqual([item["name"] for item in functions], ["run", "stop_irq"])
        self.assertEqual(functions[1]["definition_form"], "ISR_DIRECT_DECLARE")
        self.assertEqual(sum(item["kind"] == "indirect" for item in calls), 3)
        self.assertEqual([item["name"] for item in calls if item["kind"] == "candidate"],
                         ["stop_output"])

    def test_local_declarations_and_callback_alias(self):
        source = '''void run(void) {
    extern void real(void);
    callback_t target = real;
    void (*another)(void) = real;
    target(); another(); real();
}'''
        _, _, calls = GRAPH.parse_source(source, "firmware/src/test.c")
        self.assertEqual([(item["name"], item["kind"]) for item in calls],
                         [("target", "indirect"), ("another", "indirect"), ("real", "candidate")])

    def test_same_named_static_functions_and_duplicate_external_names(self):
        with temporary_source_tree() as repo:
            source = repo / "firmware/src"
            source.mkdir(parents=True)
            (source / "a.c").write_text('''/** @brief Entry A. */
static void helper(void) {}
void duplicate(void) {}
void run_a(void) { helper(); }
''', encoding="utf-8")
            (source / "b.c").write_text('''static void helper(void) {}
void duplicate(void) {}
void run_b(void) { helper(); duplicate(); }
''', encoding="utf-8")
            (source / "c.c").write_text('void run_c(void) { duplicate(); }', encoding="utf-8")
            graph = GRAPH.build_graph(repo)
            self.assertEqual(len(graph["direct_calls"]), 3)
            self.assertTrue(any(item["kind"] == "ambiguous" for item in graph["unresolved_calls"]))
            self.assertEqual(graph["diagnostics"][0]["code"], "duplicate_function_name")
            self.assertEqual(graph["files"][0]["brief"], "Entry A.")
            self.assertIn("helper / a.c", GRAPH.render_markdown(graph))
            self.assertEqual(graph, GRAPH.build_graph(repo))

    def test_bootloader_sources_exclude_external_mcuboot(self):
        with temporary_source_tree() as repo:
            for folder in ("firmware/src", "firmware/bootloader/src", ".tools/bootloader/mcuboot"):
                (repo / folder).mkdir(parents=True)
            (repo / "firmware/src/io.c").write_text("void safe_io(void) {}", encoding="utf-8")
            (repo / "firmware/bootloader/src/safe.c").write_text(
                "void boot_hook(void) { safe_io(); }", encoding="utf-8")
            (repo / ".tools/bootloader/mcuboot/main.c").write_text(
                "void external_boot(void) {}", encoding="utf-8")
            graph = GRAPH.build_graph(repo)
            self.assertEqual(len(graph["files"]), 2)
            self.assertEqual(len(graph["direct_calls"]), 1)
            self.assertIn("bootloader", graph["modules"])
            self.assertEqual(graph["extra_source_roots"], ["firmware/bootloader/src"])
            self.assertNotIn("external_boot", {f["name"] for f in graph["functions"]})

    def test_missing_source_is_an_error(self):
        with temporary_source_tree() as temporary:
            with self.assertRaises(ValueError):
                GRAPH.build_graph(Path(temporary))

    def test_continued_comment_hides_the_next_line(self):
        source = 'void run(void) {\n// comment \\\n    fake();\n    real();\n}\n'
        _, _, calls = GRAPH.parse_source(source, "firmware/src/test.c")
        self.assertEqual([item["name"] for item in calls], ["real"])

    def test_check_detects_source_changes_and_missing_outputs(self):
        with temporary_source_tree() as repo:
            source = repo / "firmware/src/main.c"
            source.parent.mkdir(parents=True)
            source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            script = repo / "firmware/tools/code_graph.py"
            script.parent.mkdir()
            shutil.copyfile(SCRIPT, script)
            command = [sys.executable, str(script)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(subprocess.run(command + ["--check"], capture_output=True).returncode, 0)
            source.write_text("int main(void) { return 1; }\n", encoding="utf-8")
            self.assertEqual(subprocess.run(command + ["--check"], capture_output=True).returncode, 1)
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            (repo / "docs/02-digital/代码图谱.json").unlink()
            self.assertEqual(subprocess.run(command + ["--check"], capture_output=True).returncode, 1)

    def test_newline_styles_have_identical_graphs_and_pass_check(self):
        with temporary_source_tree() as repo:
            source = repo / "firmware/src/main.c"
            source.parent.mkdir(parents=True)
            text = "/** @brief 换行测试。 */\nstatic void helper(void) {}\nint main(void) { helper(); return 0; }\n"
            source.write_bytes(text.encode("utf-8"))
            expected = GRAPH.build_graph(repo)
            script = repo / "firmware/tools/code_graph.py"
            script.parent.mkdir()
            shutil.copyfile(SCRIPT, script)
            command = [sys.executable, str(script)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            for newline in ("\n", "\r\n", "\r"):
                with self.subTest(newline=repr(newline)):
                    source.write_bytes(text.replace("\n", newline).encode("utf-8"))
                    self.assertEqual(GRAPH.build_graph(repo), expected)
                    result = subprocess.run(command + ["--check"], capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
