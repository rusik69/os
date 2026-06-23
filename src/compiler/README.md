# Compiler — In-kernel C compiler: intrinsics, builtins, libgcc stubs

An in-kernel C compiler (small C) that can compile, link, and load C code at runtime. Includes a lexer, recursive-descent parser, x86-64 code emitter, ELF object writer, and static linker.

## Key Files

- **cc_lex.c** — Lexer: tokenises C source into keywords, identifiers, literals, operators.
- **cc_parse.c** — Recursive-descent parser with x86-64 code generation; produces an intermediate code buffer.
- **cc_obj.c** — ELF64 relocatable object (.o) writer: emits .text, .data, .symtab, .strtab, .rela.text, .shstrtab sections.
- **cc_elf.c** — ELF64 executable writer: emits ELF header, program headers (PT_LOAD for code and data), and binary payload.
- **cc_link.c** — Static linker: reads multiple .o files, resolves symbol references, applies relocations, produces a linked ELF64 executable.

## Architecture

Traditional multi-pass compiler pipeline: lex → parse/codegen → object emit → link. All passes operate on a `CompilerState` struct. The parser is a single-pass recursive-descent parser that emits code directly into a byte buffer; no intermediate IR is used. The linker supports standard ELF64 relocations and symbol resolution across multiple input objects. Designed for self-hosting and runtime code generation within the kernel.

## Cross-References

- **modules/** — Compiled code can be linked into loadable kernel modules.
- **elf/** — Reuses kernel ELF definitions for format structures.
- **vfs/** — Reads source files and writes output via the kernel VFS layer.
