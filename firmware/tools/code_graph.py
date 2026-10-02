#!/usr/bin/env python3
"""Generate the HT source map using a deliberately bounded C parser."""

import argparse
import hashlib
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


IDENTIFIER = re.compile(r"^[A-Za-z_]\w*$")
TOKENS = re.compile(r"[A-Za-z_]\w*|->|[^\s]")
EXPRESSION_PREFIXES = {
    "if", "else", "for", "while", "switch", "return", "sizeof", "_Alignof",
    "alignof", "typeof", "__typeof__", "_Generic", "_Static_assert", "defined",
    "asm", "__asm__", "__attribute__",
}
NON_CALLS = EXPRESSION_PREFIXES | {
    "void", "char", "int", "float", "double", "long", "short", "signed",
    "unsigned", "_Bool", "bool", "struct", "union", "enum", "_Atomic",
}
LIMITATIONS = [
    "仅扫描 firmware/src 与 firmware/bootloader/src 下的原创 .c/.h；后者归 bootloader 模块，不扫描第三方 MCUboot。",
    "源码按 UTF-8 解码（允许 BOM），CRLF/CR 统一为 LF 后再解析和计算 SHA256；换行格式不影响图谱。",
    "有界词法解析支持普通函数定义及 ISR_DIRECT_DECLARE；不是 C 编译器，不展开宏或计算条件编译。",
    "直接调用边只连接可唯一定位的源码函数定义；注释、字符串和预处理指令不参与调用识别。",
    "函数指针、成员调用、外部库和未知宏调用不能据此确定目标；同名冲突会列入诊断。",
    "没有完整类型与作用域分析，复杂声明、宏生成代码或间接调用需人工核对，不能声称完整调用图。",
    "设备树、线程调度、回调注册、DMA/中断触发及人工硬件依赖不属于自动调用证明。",
]


def mask_c(source, mask_literals=True):
    """Blank comments/literals while preserving offsets and line numbers."""
    chars = list(source)
    pos = 0
    while pos < len(source):
        if source.startswith("//", pos):
            end = source.find("\n", pos)
            while end >= 0 and source[pos:end].rstrip("\r").endswith("\\"):
                end = source.find("\n", end + 1)
            end = len(source) if end < 0 else end
        elif source.startswith("/*", pos):
            end = source.find("*/", pos + 2)
            if end < 0:
                raise ValueError("unterminated C comment")
            end += 2
        elif source[pos] in "\"'":
            quote = source[pos]
            end = pos + 1
            while end < len(source):
                if source[end] == "\\":
                    end += 2
                elif source[end] == quote:
                    end += 1
                    break
                else:
                    end += 1
            else:
                raise ValueError("unterminated C literal")
            if not mask_literals:
                pos = end
                continue
        else:
            pos += 1
            continue
        for index in range(pos, min(end, len(chars))):
            if chars[index] != "\n":
                chars[index] = " "
        pos = end
    return "".join(chars)


def without_directives(source):
    result = []
    continued = False
    for line in source.splitlines(keepends=True):
        directive = continued or line.lstrip().startswith("#")
        continued = directive and line.rstrip("\r\n").rstrip().endswith("\\")
        result.append("".join("\n" if char == "\n" else " " for char in line)
                      if directive else line)
    return "".join(result)


def closing(tokens, start, opening, ending):
    depth = 0
    for index in range(start, len(tokens)):
        value = tokens[index][0]
        if value == opening:
            depth += 1
        elif value == ending:
            depth -= 1
            if depth == 0:
                return index
    raise ValueError(f"unbalanced {opening}{ending}")


def source_brief(source):
    # Only a leading comment supplies a responsibility; filenames are not evidence.
    comment = re.match(r"\s*/\*(.*?)\*/", source, re.DOTALL)
    if not comment:
        return "待源码文件头 @brief 说明"
    brief = re.search(r"[@\\]brief\s+([^\r\n]+)", comment.group(1))
    return brief.group(1).strip().rstrip("*").strip() if brief else "待源码文件头 @brief 说明"


def parse_source(source, path):
    visible = mask_c(source, mask_literals=False)
    includes = re.findall(r'^\s*#\s*include\s*"([^"\r\n]+)"', visible, re.MULTILINE)
    macros = set(re.findall(r"^\s*#\s*define\s+([A-Za-z_]\w*)", visible, re.MULTILINE))
    clean = without_directives(mask_c(source))
    tokens = [(match.group(), match.start()) for match in TOKENS.finditer(clean)]
    functions = []
    calls = []
    index = 0
    declaration_start = 0
    while index < len(tokens):
        name, offset = tokens[index]
        if name in {";", "}"}:
            declaration_start = index + 1
        if name == "{":
            index = closing(tokens, index, "{", "}") + 1
            declaration_start = index
            continue
        if (IDENTIFIER.match(name) and name not in NON_CALLS
                and index + 1 < len(tokens) and tokens[index + 1][0] == "("):
            parameters_end = closing(tokens, index + 1, "(", ")")
            body_start = parameters_end + 1
            if body_start < len(tokens) and tokens[body_start][0] == "{":
                form = "function"
                if name == "ISR_DIRECT_DECLARE":
                    parameters = tokens[index + 2:parameters_end]
                    if len(parameters) != 1 or not IDENTIFIER.match(parameters[0][0]):
                        raise ValueError(f"{path}: unsupported ISR_DIRECT_DECLARE")
                    name = parameters[0][0]
                    form = "ISR_DIRECT_DECLARE"
                body_end = closing(tokens, body_start, "{", "}")
                line = source.count("\n", 0, offset) + 1
                function_id = f"{path}:{line}:{name}"
                prefix = [token[0] for token in tokens[declaration_start:index]]
                functions.append({
                    "id": function_id, "name": name, "path": path, "line": line,
                    "linkage": "static" if "static" in prefix else "external",
                    "definition_form": form,
                })
                parameter_names = {token[0] for token in tokens[index + 2:parameters_end]
                                   if IDENTIFIER.match(token[0])} if form == "function" else set()
                body_text = clean[tokens[body_start][1]:tokens[body_end][1]]
                pointer_names = set(re.findall(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)", body_text))
                local_declarations = re.finditer(
                    r"(?:^|[;{}])\s*(?:[A-Za-z_]\w*\s+|\*+\s*)+"
                    r"([A-Za-z_]\w*)\s*(?:=|;|,|\[)", body_text)
                local_names = {match.group(1) for match in local_declarations
                               if match.group().lstrip(";{} \t\r\n").split()[0] not in EXPRESSION_PREFIXES}
                for call_index in range(body_start + 1, body_end):
                    call_name, call_offset = tokens[call_index]
                    if (not IDENTIFIER.match(call_name) or call_name in NON_CALLS
                            or tokens[call_index + 1][0] != "("):
                        continue
                    previous = tokens[call_index - 1][0]
                    statement_start = call_index - 1
                    while statement_start > body_start and tokens[statement_start][0] not in {";", "{", "}"}:
                        statement_start -= 1
                    prefix = [token[0] for token in tokens[statement_start + 1:call_index]]
                    if (prefix and prefix[0] not in EXPRESSION_PREFIXES
                            and all(IDENTIFIER.match(token) or token == "*" for token in prefix)):
                        call_end = closing(tokens, call_index + 1, "(", ")")
                        if call_end + 1 < body_end and tokens[call_end + 1][0] == ";":
                            continue  # A block-scope function declaration, not an invocation.
                    kind = "candidate"
                    if previous in {".", "->"} or call_name in parameter_names | pointer_names | local_names:
                        kind = "indirect"
                    elif call_name in macros:
                        kind = "macro"
                    calls.append({"caller": function_id, "name": call_name,
                                  "line": source.count("\n", 0, call_offset) + 1, "kind": kind})
                for match in re.finditer(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(", body_text):
                    prefix = re.split(r"[;{}]", body_text[:match.start()])[-1].strip()
                    if prefix and prefix.split()[0] not in EXPRESSION_PREFIXES and re.fullmatch(r"[\w\s*]+", prefix):
                        continue
                    call_offset = tokens[body_start][1] + match.start()
                    calls.append({"caller": function_id, "name": match.group(1),
                                  "line": source.count("\n", 0, call_offset) + 1,
                                  "kind": "indirect"})
                index = body_end + 1
                declaration_start = index
                continue
            index = parameters_end + 1
            continue
        index += 1
    return includes, functions, calls


def build_graph(repo):
    source_root = repo / "firmware" / "src"
    boot_root = repo / "firmware" / "bootloader" / "src"
    paths = sorted(path for root in (source_root, boot_root) for path in root.rglob("*")
                   if path.suffix in {".c", ".h"})
    if not paths:
        raise ValueError("firmware/src 下没有 C/H 源码，暂不生成空图谱")
    files, functions, candidates, diagnostics = [], [], [], []
    for path in paths:
        relative = path.relative_to(repo).as_posix()
        if path.is_relative_to(boot_root):
            module = "bootloader"
        else:
            source_relative = path.relative_to(source_root)
            module = source_relative.parts[0] if len(source_relative.parts) > 1 else "."
        source = path.read_bytes().decode("utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")
        includes, definitions, calls = parse_source(source, relative)
        files.append({"path": relative, "module": module, "brief": source_brief(source),
                      "sha256": hashlib.sha256(source.encode("utf-8")).hexdigest(), "includes": includes})
        for function in definitions:
            function["module"] = module
        functions.extend(definitions)
        candidates.extend(calls)

    by_name, by_id = defaultdict(list), {}
    for function in functions:
        if function["id"] in by_id:
            raise ValueError(f"函数节点 ID 冲突：{function['id']}")
        by_id[function["id"]] = function
        by_name[function["name"]].append(function)
    for name, definitions in sorted(by_name.items()):
        external = [item for item in definitions if item["linkage"] == "external"]
        repeated_files = [path for path, count in Counter(item["path"] for item in definitions).items()
                          if count > 1]
        if len(external) > 1 or repeated_files:
            diagnostics.append({"code": "duplicate_function_name", "name": name,
                                "definitions": [item["id"] for item in definitions],
                                "detail": "可能来自条件编译；未预处理，歧义调用不连边。"})

    direct, unresolved = [], []
    for call in candidates:
        caller = by_id[call["caller"]]
        definitions = by_name.get(call["name"], [])
        same_file = [item for item in definitions if item["path"] == caller["path"]]
        targets = same_file or [item for item in definitions if item["linkage"] == "external"]
        if call["kind"] == "candidate" and len(targets) == 1:
            direct.append({"caller": call["caller"], "callee": targets[0]["id"], "line": call["line"]})
        else:
            unresolved.append({**call, "kind": call["kind"] if call["kind"] != "candidate"
                               else "ambiguous" if len(targets) > 1 else "external_or_unknown"})

    file_map = {item["path"]: item for item in files}
    include_edges = []
    for file in files:
        for include in file["includes"]:
            search = [(repo / file["path"]).parent / include, source_root / include]
            found = []
            for target in search:
                try:
                    target_path = target.resolve().relative_to(repo.resolve()).as_posix()
                except ValueError:
                    continue
                if target_path in file_map and target_path not in found:
                    found.append(target_path)
            if found:
                include_edges.append({"source": file["path"], "target": found[0], "include": include})
            else:
                diagnostics.append({"code": "unresolved_include", "path": file["path"],
                                    "include": include, "detail": "仅按当前目录和 firmware/src 查找。"})

    modules = sorted({item["module"] for item in files})
    module_edges = defaultdict(set)
    for edge in direct:
        pair = (by_id[edge["caller"]]["module"], by_id[edge["callee"]]["module"])
        if pair[0] != pair[1]:
            module_edges[pair].add("call")
    for edge in include_edges:
        pair = (file_map[edge["source"]]["module"], file_map[edge["target"]]["module"])
        if pair[0] != pair[1]:
            module_edges[pair].add("include")
    return {"schema_version": 1, "generator": "firmware/tools/code_graph.py",
            "source_root": "firmware/src", "extra_source_roots": ["firmware/bootloader/src"],
            "limitations": LIMITATIONS, "modules": modules,
            "files": files, "functions": functions, "include_edges": include_edges,
            "direct_calls": sorted(direct, key=lambda item: (item["caller"], item["line"], item["callee"])),
            "unresolved_calls": sorted(unresolved, key=lambda item: (item["caller"], item["line"], item["name"])),
            "module_edges": [{"source": pair[0], "target": pair[1], "kinds": sorted(kinds)}
                             for pair, kinds in sorted(module_edges.items())],
            "diagnostics": diagnostics}


def table_text(text):
    return text.replace("|", "\\|").replace("\n", " ")


def mermaid_text(text):
    return text.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;").replace(">", "&gt;")


def source_link(path, line=None):
    target = "../../" + path + (f"#L{line}" if line else "")
    label = path.removeprefix("firmware/src/") + (f":{line}" if line else "")
    return f"[{table_text(label)}](<{target}>)"


def render_markdown(graph):
    module_ids = {name: f"M{index}" for index, name in enumerate(graph["modules"])}
    function_ids = {item["id"]: f"F{index}" for index, item in enumerate(graph["functions"])}
    if len(set(module_ids.values())) != len(module_ids) or len(set(function_ids.values())) != len(function_ids):
        raise ValueError("Mermaid 节点 ID 冲突")
    lines = ["# HT 固件代码图谱", "", "由 `firmware/tools/code_graph.py` 从实际源码生成，请勿手工修改。", "",
             "在仓库根目录运行：", "", "```powershell", "python firmware/tools/code_graph.py",
             "python firmware/tools/code_graph.py --check", "```", "",
             "`--check` 比较 Markdown 和 JSON；源码新增、删除或内容变化后未更新会返回非零状态。", "",
             "机器数据：[代码图谱.json](代码图谱.json)。源码链接相对本文件，并带定义行号。", "",
             "## 解析边界", ""]
    lines.extend(f"- {item}" for item in graph["limitations"])
    lines.extend(["", "## 模块关系", "", "箭头由使用方指向被引用/调用方，节点名称来自源码目录。", "", "```mermaid", "flowchart LR"])
    for module, node in module_ids.items():
        label = "入口（src 根目录）" if module == "." else module
        lines.append(f'    {node}["{mermaid_text(label)}"]')
    for edge in graph["module_edges"]:
        label = " + ".join(edge["kinds"])
        lines.append(f'    {module_ids[edge["source"]]} -->|{label}| {module_ids[edge["target"]]}')
    lines.extend(["```", "", "## 职责与文件索引", "", "职责仅提取文件头 `@brief`，不从目录名推断。", "",
                  "| 模块 | 源码 | 职责 | 函数定义数 |", "|---|---|---|---:|"])
    counts = Counter(item["path"] for item in graph["functions"])
    for file in graph["files"]:
        module = "入口" if file["module"] == "." else file["module"]
        lines.append(f'| {table_text(module)} | {source_link(file["path"])} | {table_text(file["brief"])} | {counts[file["path"]]} |')
    lines.extend(["", "## 函数定义", "", "| 函数 | 定义位置 | 链接属性 | 可定位的直接被调函数 |", "|---|---|---|---|"])
    called = defaultdict(set)
    by_id = {item["id"]: item for item in graph["functions"]}
    for edge in graph["direct_calls"]:
        called[edge["caller"]].add(edge["callee"])
    for function in graph["functions"]:
        targets = [f'`{by_id[target]["name"]}` ({source_link(by_id[target]["path"], by_id[target]["line"])})'
                   for target in sorted(called[function["id"]])]
        lines.append(f'| `{function["name"]}` | {source_link(function["path"], function["line"])} | {function["linkage"]} | {"、".join(targets) or "无已定位调用"} |')
    lines.extend(["", "## 源码直接调用图", ""])
    edges = sorted({(item["caller"], item["callee"]) for item in graph["direct_calls"]})
    if edges:
        lines.extend(["只画参与已定位调用的函数；同名函数通过文件路径区分。", "", "```mermaid", "flowchart TD"])
        participating = {node for edge in edges for node in edge}
        name_counts = Counter(item["name"] for item in graph["functions"])
        for function in graph["functions"]:
            if function["id"] in participating:
                label = function["name"]
                if name_counts[label] > 1:
                    label += " / " + function["path"].removeprefix("firmware/src/")
                lines.append(f'    {function_ids[function["id"]]}["{mermaid_text(label)}"]')
        for caller, callee in edges:
            lines.append(f"    {function_ids[caller]} --> {function_ids[callee]}")
        lines.append("```")
    else:
        lines.append("当前未找到可唯一定位目标的源码直接调用边。")
    lines.extend(["", "## 未解析项与诊断", "",
                  f'未解析调用形式共 {len(graph["unresolved_calls"])} 处，详见 JSON 的 `unresolved_calls`。这不表示调用错误或无效。', ""])
    if graph["diagnostics"]:
        for item in graph["diagnostics"]:
            detail = json.dumps(item, ensure_ascii=False, sort_keys=True)
            lines.append(f"- `{detail}`")
    else:
        lines.append("未发现函数命名冲突或无法定位的项目内引号引用。")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="只检查图谱是否与源码一致，不写文件")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    try:
        graph = build_graph(repo)
        outputs = {
            repo / "docs/02-digital/代码图谱.md": render_markdown(graph),
            repo / "docs/02-digital/代码图谱.json": json.dumps(graph, ensure_ascii=False, indent=2) + "\n",
        }
        stale = []
        for path, content in outputs.items():
            if args.check:
                if not path.is_file() or path.read_text(encoding="utf-8") != content:
                    stale.append(path.relative_to(repo).as_posix())
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8", newline="\n")
                print(f"已生成 {path.relative_to(repo).as_posix()}")
        if stale:
            print("图谱缺失或已过时：" + "、".join(stale), file=sys.stderr)
            return 1
        if args.check:
            print("代码图谱与当前源码一致")
        return 0
    except (OSError, UnicodeError, ValueError) as error:
        print(f"代码图谱生成失败：{error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
