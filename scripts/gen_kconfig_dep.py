#!/usr/bin/env python3
"""
gen_kconfig_dep.py — Kernel Configuration Dependency Graph Generator

Parses a kernel Makefile (or source tree) for conditional compilation patterns
and generates a dependency graph of CONFIG_* options. Supports two output modes:

  text  — ASCII tree view (default)
  dot   — Graphviz DOT format (visualisable with dot/pydot/Graphviz)

Usage:
  python3 scripts/gen_kconfig_dep.py [Makefile] [options]

Options:
  -o, --output FILE   Write output to FILE (default: stdout)
  -f, --format FORMAT Output format: 'text' (default) or 'dot'
  -s, --source-dir    Also scan .c/.h files for #ifdef CONFIG_ patterns
  --no-makefile       Skip Makefile parsing (use with --source-dir)
  --min-occurences N  Only show CONFIG_ symbols that appear at least N times
  -h, --help          Show this help message

Output (text mode):
  Each CONFIG_ symbol is shown as a tree with its dependents indented below it.

Output (dot mode):
  Generates a directed graph in Graphviz DOT format. CONFIG_ symbols are
  rendered as ellipses; edges go from CONFIG_ to the object it controls.

Examples:
  # Parse the top-level Makefile, output ASCII tree
  python3 scripts/gen_kconfig_dep.py Makefile

  # Generate graphviz output for visualisation
  python3 scripts/gen_kconfig_dep.py Makefile -f dot -o deps.dot

  # Also scan source files for ifdef/CONFIG_ usage
  python3 scripts/gen_kconfig_dep.py Makefile -s
"""

import re
import sys
import os
import argparse
from collections import defaultdict


# ── Pattern definitions ──────────────────────────────────────────────────

# obj-$(CONFIG_XXX) in Makefiles
RE_OBJ_CONFIG = re.compile(
    r'obj-\$\(CONFIG_([A-Za-z0-9_]+)\)\s*(\+|)\s*([^\\\n#]+)'
)

# Conditionals: ifdef CONFIG_XXX or ifeq ($(CONFIG_XXX),y)
RE_IFDEF_CONFIG = re.compile(
    r'(?:ifdef|ifndef|ifeq)\s*[($]{0,2}CONFIG_([A-Za-z0-9_]+)'
)

# #ifdef CONFIG_XXX in C source files
RE_C_IFDEF = re.compile(
    r'#\s*if(?:n?)def\s+CONFIG_([A-Za-z0-9_]+)'
)

# #if defined(CONFIG_XXX) in C source
RE_C_IF_DEFINED = re.compile(
    r'#\s*if\s+defined\s*\(\s*CONFIG_([A-Za-z0-9_]+)\s*\)'
)


# ── Parser classes ───────────────────────────────────────────────────────

class ConfigDependency:
    """Represents a single CONFIG_ symbol and what it controls."""

    def __init__(self, name):
        self.name = name
        self.objects = []        # object files controlled by this config
        self.conditions = []     # conditional expressions involving this config
        self.references = 0      # how many times the symbol appears
        self.is_module = False   # tracked if =m vs =y

    def __repr__(self):
        return f"<Config {self.name} refs={self.references} objs={self.objects}>"


class DependencyGraph:
    """Graph of CONFIG_ symbols and their dependents."""

    def __init__(self):
        self.symbols = {}               # name -> ConfigDependency
        self.dependents = defaultdict(list)  # config_name -> [sub_configs/files]
        self.parents = defaultdict(list)     # sub_config -> [config_name]

    def get_or_create(self, name):
        if name not in self.symbols:
            self.symbols[name] = ConfigDependency(name)
        return self.symbols[name]

    def add_object(self, config_name, obj_file):
        cfg = self.get_or_create(config_name)
        cfg.objects.append(obj_file.strip())
        cfg.references += 1
        self.dependents[config_name].append(f"obj={obj_file.strip()}")

    def add_dependency(self, parent_config, child_config):
        self.dependents[parent_config].append(f"dep={child_config}")
        self.parents[child_config].append(parent_config)

    def add_reference(self, config_name):
        cfg = self.get_or_create(config_name)
        cfg.references += 1

    def merge(self, other):
        """Merge another DependencyGraph into this one."""
        for name, cfg in other.symbols.items():
            if name not in self.symbols:
                self.symbols[name] = cfg
            else:
                self.symbols[name].references += cfg.references
                self.symbols[name].objects.extend(cfg.objects)
        for k, v in other.dependents.items():
            self.dependents[k].extend(v)
        for k, v in other.parents.items():
            self.parents[k].extend(v)


# ── Makefile parser ──────────────────────────────────────────────────────

def parse_makefile(path):
    """Parse a kernel Makefile for CONFIG_ patterns.

    Returns a DependencyGraph.
    """
    graph = DependencyGraph()

    try:
        with open(path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
    except FileNotFoundError:
        print(f"error: file not found: {path}", file=sys.stderr)
        return graph
    except PermissionError:
        print(f"error: permission denied: {path}", file=sys.stderr)
        return graph

    # Remove line continuations for simpler pattern matching
    content_flat = re.sub(r'\\\s*\n', ' ', content)

    # Find obj-$(CONFIG_XXX) += / := pattern
    for match in RE_OBJ_CONFIG.finditer(content_flat):
        config_name = match.group(1)
        operator = match.group(2).strip()  # + or empty
        obj_value = match.group(3).strip()

        cfg = graph.get_or_create(config_name)
        cfg.objects.append(obj_value)
        cfg.references += 1

        # Determine if it's a module (=m) or built-in (=y)
        if obj_value.endswith('.ko') or config_name.endswith('_MODULE'):
            cfg.is_module = True

        graph.dependents[config_name].append(f"obj={obj_value}")

    # Find conditional blocks: ifdef CONFIG_XXX / ifeq ($(CONFIG_XXX),y)
    for match in RE_IFDEF_CONFIG.finditer(content):
        config_name = match.group(1)
        graph.add_reference(config_name)

    # Also look for plain CONFIG_ references (like for vermagic)
    for match in re.finditer(r'\bCONFIG_([A-Za-z0-9_]+)\b', content):
        if match.group(1) not in graph.symbols or True:
            graph.add_reference(match.group(1))

    return graph


# ── C source parser ─────────────────────────────────────────────────────

def parse_source_files(src_dir):
    """Walk a source directory and parse .c and .h files for CONFIG_ patterns.

    Returns a DependencyGraph.
    """
    graph = DependencyGraph()

    if not os.path.isdir(src_dir):
        print(f"warning: source directory not found: {src_dir}", file=sys.stderr)
        return graph

    for root, dirs, files in os.walk(src_dir):
        # Skip hidden directories and build artifacts
        dirs[:] = [d for d in dirs if not d.startswith('.') and d != 'build']

        for fname in files:
            if not fname.endswith(('.c', '.h')):
                continue
            fpath = os.path.join(root, fname)
            relpath = os.path.relpath(fpath, src_dir)

            try:
                with open(fpath, 'r', encoding='utf-8', errors='ignore') as f:
                    content = f.read()
            except (IOError, OSError):
                continue

            # Find #ifdef CONFIG_XXX and #if defined(CONFIG_XXX)
            for match in RE_C_IFDEF.finditer(content):
                config_name = match.group(1)
                graph.add_reference(config_name)

            for match in RE_C_IF_DEFINED.finditer(content):
                config_name = match.group(1)
                graph.add_reference(config_name)

            # Also find direct CONFIG_ references not in comments
            for match in re.finditer(
                r'(?://.*)?\bCONFIG_([A-Za-z0-9_]+)\b',
                content
            ):
                config_name = match.group(1)
                graph.add_reference(config_name)

    return graph


# ── Output formatters ────────────────────────────────────────────────────

def format_text(graph, min_occurrences=1):
    """Format the dependency graph as an ASCII tree."""
    lines = []
    lines.append("Kernel Configuration Dependency Graph")
    lines.append("=" * 50)
    lines.append("")

    # Sort symbols by name (or by reference count descending)
    sorted_symbols = sorted(
        graph.symbols.values(),
        key=lambda c: (-c.references, c.name)
    )

    for cfg in sorted_symbols:
        if cfg.references < min_occurrences:
            continue

        module_tag = " [module]" if cfg.is_module else ""
        lines.append(f"● CONFIG_{cfg.name}{module_tag}")
        lines.append(f"  │ References: {cfg.references}")

        if cfg.objects:
            lines.append(f"  │ Controls ({len(cfg.objects)} file(s)):")
            for obj in cfg.objects:
                lines.append(f"  ├── {obj}")

        if cfg.name in graph.dependents:
            deps = graph.dependents[cfg.name]
            if deps:
                lines.append(f"  │ Dependents:")
                for dep in deps:
                    lines.append(f"  ├── {dep}")

        lines.append("")

    # Summary footer
    total_symbols = sum(1 for c in sorted_symbols if c.references >= min_occurrences)
    total_refs = sum(c.references for c in sorted_symbols if c.references >= min_occurrences)
    lines.append(f"─── Summary ───────────────────────────────")
    lines.append(f"  Total CONFIG_ symbols:   {total_symbols}")
    lines.append(f"  Total references:        {total_refs}")
    lines.append(f"  Total object files:      {sum(1 for c in sorted_symbols for o in c.objects if c.references >= min_occurrences)}")

    return "\n".join(lines)


def format_dot(graph, min_occurrences=1):
    """Format the dependency graph as Graphviz DOT."""
    lines = []
    lines.append("digraph kconfig_deps {")
    lines.append("    rankdir=LR;")
    lines.append("    graph [fontname=\"monospace\" bgcolor=\"#ffffff\"];")
    lines.append("    node  [fontname=\"monospace\" shape=ellipse style=filled fillcolor=\"#e0f0ff\"];")
    lines.append("    edge  [fontname=\"monospace\" arrowhead=vee color=\"#444444\"];")
    lines.append("")
    lines.append("    // ── Legend ──")
    lines.append("    legend [label=\"CONFIG_* = Kernel Config Symbol\\nobj = Object file\\nEdge: config controls object\" shape=box fillcolor=\"#f0f0f0\"];")
    lines.append("")

    # Collect nodes and edges
    edges = set()
    node_names = set()

    for name, cfg in graph.symbols.items():
        if cfg.references < min_occurrences:
            continue

        node_id = f"cfg_{name}"
        label = f"CONFIG_{name}"
        if cfg.is_module:
            label += " (m)"

        fillcolor = "#e0ffe0" if cfg.is_module else "#e0f0ff"
        lines.append(f'    {node_id} [label="{label}" fillcolor="{fillcolor}"];')
        node_names.add(node_id)

        for obj in cfg.objects:
            obj_node = f"obj_{re.sub(r'[^a-zA-Z0-9_]', '_', obj)}"
            lines.append(f'    {obj_node} [label="{obj}" shape=box fillcolor="#f5f5dc"];')
            node_names.add(obj_node)
            edge = (node_id, obj_node)
            if edge not in edges:
                edges.add(edge)
                lines.append(f"    {node_id} -> {obj_node};")

        # Add edges for dependent relationships
        if name in graph.dependents:
            for dep in graph.dependents[name]:
                if dep.startswith("dep="):
                    dep_name = dep[4:]
                    if dep_name in graph.symbols and graph.symbols[dep_name].references >= min_occurrences:
                        dep_node = f"cfg_{dep_name}"
                        edge = (node_id, dep_node)
                        if edge not in edges:
                            edges.add(edge)
                            lines.append(f"    {node_id} -> {dep_node} [style=dashed color=\"#888888\"];")

    lines.append("")

    # Rank the legend separately
    lines.append("    { rank=source; legend; }")
    lines.append("}")

    return "\n".join(lines)


# ── Main ─────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Kernel configuration dependency graph generator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )

    parser.add_argument(
        "makefile",
        nargs="?",
        default="Makefile",
        help="Path to kernel Makefile (default: Makefile)"
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output file path (default: stdout)"
    )
    parser.add_argument(
        "-f", "--format",
        choices=["text", "dot"],
        default="text",
        help="Output format (default: text)"
    )
    parser.add_argument(
        "-s", "--source-dir",
        default=None,
        const=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"),
        nargs="?",
        help="Also scan source files for #ifdef CONFIG_ patterns. "
             "Optionally specify path (default: ../src relative to this script)"
    )
    parser.add_argument(
        "--no-makefile",
        action="store_true",
        help="Skip Makefile parsing (useful with --source-dir only)"
    )
    parser.add_argument(
        "--min-occurrences",
        type=int,
        default=1,
        metavar="N",
        help="Minimum occurrence count to include a symbol (default: 1)"
    )
    parser.add_argument(
        "--show-modules-only",
        action="store_true",
        help="Only show symbols that build modules (.ko files)"
    )

    args = parser.parse_args()

    graph = DependencyGraph()

    # Parse Makefile
    if not args.no_makefile:
        if not os.path.exists(args.makefile):
            print(f"error: Makefile not found: {args.makefile}", file=sys.stderr)
            sys.exit(1)
        make_graph = parse_makefile(args.makefile)
        graph.merge(make_graph)

    # Parse source files if requested
    if args.source_dir:
        src_graph = parse_source_files(args.source_dir)
        graph.merge(src_graph)

    if not graph.symbols:
        print("warning: no CONFIG_ symbols found. The Makefile may not use", file=sys.stderr)
        print("         obj-$(CONFIG_XXX) patterns. Try --source-dir to scan", file=sys.stderr)
        print("         C source files for #ifdef CONFIG_XXX directives.", file=sys.stderr)
        sys.exit(0)

    # Filter if --show-modules-only
    if args.show_modules_only:
        for name in list(graph.symbols.keys()):
            if not graph.symbols[name].is_module:
                del graph.symbols[name]

    # Generate output
    if args.format == "dot":
        output = format_dot(graph, args.min_occurrences)
    else:
        output = format_text(graph, args.min_occurrences)

    # Write output
    if args.output:
        try:
            with open(args.output, 'w', encoding='utf-8') as f:
                f.write(output)
                f.write("\n")
            print(f"wrote {args.format} output to {args.output}", file=sys.stderr)
        except IOError as e:
            print(f"error writing output: {e}", file=sys.stderr)
            sys.exit(1)
    else:
        print(output)


if __name__ == "__main__":
    main()
