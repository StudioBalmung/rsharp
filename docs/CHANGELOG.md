# R# Changelog

## 1.0.2 (July 2026)

- Fixed parser support for `TypeName { field: value }` struct literals.
- Fixed `examples/ownership.rss` struct literal syntax from `len = 64` to `len: 64`.
- Fixed sema scope symbol table growth when local symbol count exceeds capacity.
- Added basic ownership move tracking for non-copy values assigned or passed by value.
- Hardened arena allocation alignment and `arena_calloc` overflow handling.
- Wired `resources.rc` and `rsharp-logo.ico` into Windows executables.
- Added medium LLVM/backend readiness documentation; native LLVM remains optional.

## 1.0.1 (July 2026)

### Language
- `fn name(params) => RetType { body }` syntax
- Short form: `fn name() => expr` (expression body)
- All file extensions: `.rsl`, `.rsh`, `.rss`, `.rsp`, `.rslib`
- Single ownership + borrow checker
- Arena-only allocator (no GC, no hidden malloc)
- `Result<T,E>` + `try`/`catch` error handling
- `?T` optional type + `??` null-coalescing
- `comptime` generics (zero runtime overhead)
- `defer` for guaranteed cleanup
- `unsafe` blocks for raw pointer operations
- `match` expressions (exhaustive)
- `async`/`await` (stackless coroutines)
- `spawn` for concurrent tasks
- `Channel<T>` for message passing
- `interface` (traits) with default method implementations
- Multi-target: x86_64, aarch64, wasm32

### Toolchain
- `rsl` / `rsharp` — unified CLI (new, build, run, install, add, fmt, doc, test)
- `rsc` — compiler driver (LLVM backend)
- `rsrun` — script runner (JIT/interpret)
- `rsfmt` — formatter
- `rsdoc` — documentation generator
- `Rsharp.toml` — project manifest (TOML)
- GitHub Linguist support (`.gitattributes`)

### Standard Library
- `core`: primitives, arena, option, result
- `io`: file I/O, stdin/stdout/stderr
- `math`: f64 math, trig, interpolation
- `collections`: `Vec<T>`, `Map<K,V>`
- `smart_ptr`: `Box<T>`, `Rc<T>`, `Arc<T>`
