# R# Compiler Architecture

## Pipeline

```
source.rsl / .rsh / .rss / .rsp / .rslib
        │
        ▼
┌──────────────┐  C11
│     Lexer    │  compiler/lexer/lexer.c
│              │  Tokens: identifiers, literals, operators
│              │  Keywords: fn, let, var, struct, enum, impl, ...
│              │  Syntax: => (return type + match), ?? (null-coalesce)
└──────┬───────┘
       │ token stream
       ▼
┌──────────────┐  C11
│    Parser    │  compiler/parser/parser.c + ast.h
│              │  Recursive descent with Pratt expression parsing
│              │  Produces: AstFile → Decl[] → Stmt[] / Expr[]
└──────┬───────┘
       │ untyped AST
       ▼
┌──────────────┐  C11
│  Type Sema   │  compiler/sema/sema.c + sema.h
│              │  Two-pass: forward-declare → type-check bodies
│              │  Resolves: types, symbols, overloads
│              │  Produces: typed AST (Expr.type filled in)
└──────┬───────┘
       │ typed AST
       ▼
┌──────────────┐  C11
│  Ownership   │  compiler/sema/ownership.c (planned)
│  Checker     │  Single-ownership + borrow rules
│              │  Move tracking, borrow lifetime validation
└──────┬───────┘
       │ verified AST
       ▼
┌──────────────┐  C++20
│ LLVM Backend │  compiler/codegen/llvm_backend.cpp
│              │  AST → LLVM IR
│              │  Targets: x86_64, aarch64, wasm32
└──────┬───────┘
       │ LLVM IR (.ll)
       ▼
   llc (LLVM)
       │ object file (.o)
       ▼
   lld (LLVM linker)
       │
       ▼
  native binary / .wasm
```

## Memory Model

**Arena-only allocation.** No GC, no hidden malloc.

- Compiler itself uses a single `Arena` per compilation unit
- All AST nodes, strings, types allocated from it
- `arena_destroy()` at end = entire compile freed in one syscall
- Programs use `Arena.init()` / `Arena.deinit()` explicitly

## Ownership System

**Single Owner + Borrow Checker** (like Rust, less annotation burden):

```
                    Owner
                      │
            ┌────────┴────────┐
       Move (transfer)    Borrow (reference)
            │                 │
      new owner         ┌─────┴─────┐
      (old owner      shared     mutable
       invalid)       &T (n)    *T (1, exclusive)
```

- Moves tracked per variable across all code paths
- Borrows validated at compile time (no runtime cost)
- Lifetime annotations only needed for cross-function borrows returning refs
- Smart pointers (`Box`, `Rc`, `Arc`) are library types — not special syntax

## LLVM Targets

| Target Triple | Architecture | Status |
|---------------|-------------|--------|
| `x86_64-linux-gnu` | Intel/AMD 64-bit | Primary |
| `aarch64-linux-gnu` | ARM64 | Primary |
| `wasm32-wasi` | WebAssembly | Primary |
| `x86_64-windows-msvc` | Windows x64 | Planned |
| `aarch64-apple-darwin` | Apple Silicon | Planned |

## File Extensions

| Extension | Meaning | Example |
|-----------|---------|---------|
| `.rsl` | R# Language source (main) | `main.rsl` |
| `.rsh` | R# Header / interface definition | `vec3.rsh` |
| `.rss` | R# Script (interpreted/quick-run) | `build.rss` |
| `.rsp` | R# Program (standalone binary) | `tool.rsp` |
| `.rslib` | R# Library (pre-compiled) | `libjson.rslib` |

## Tools

| Tool | Binary | Description |
|------|--------|-------------|
| Build + PM | `rsl` / `rsharp` | Project management, package install |
| Compiler | `rsc` | Source → native binary |
| Script runner | `rsrun` | JIT/interpret `.rss` files |
| Formatter | `rsfmt` | Auto-format source |
| Doc generator | `rsdoc` | API docs from `///` comments |

## Smart Pointers vs Raw Pointers

```
Stack value  >  let x: i32 = 42              (zero overhead)
Arena alloc  >  arena.alloc(T, n)            (bump pointer, O(1))
Unique heap  >  Box<T>                        (one owner)
Shared heap  >  Rc<T>                         (ref count, single-thread)
Shared+MT   >  Arc<T>                        (atomic ref count)
Shared+mut  >  Arc<Mutex<T>>                 (thread-safe mutation)
Raw pointer  >  unsafe { let p: *T = ... }   (no safety guarantees)
```
