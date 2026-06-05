# R# Language Guide

> **R# 1.0 - Fast. Safe. Learnable.**

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Hello World](#hello-world)
3. [Variables & Types](#variables--types)
4. [Functions](#functions)
5. [Control Flow](#control-flow)
6. [Structs & Methods](#structs--methods)
7. [Enums & Pattern Matching](#enums--pattern-matching)
8. [Ownership & Borrowing](#ownership--borrowing)
9. [Error Handling](#error-handling)
10. [Generics & Comptime](#generics--comptime)
11. [Interfaces (Traits)](#interfaces-traits)
12. [Memory: Arena Allocator](#memory-arena-allocator)
13. [Smart Pointers](#smart-pointers)
14. [Concurrency](#concurrency)
15. [FFI: C Interop](#ffi-c-interop)
16. [Modules & Packages](#modules--packages)
17. [Builtins Reference](#builtins-reference)

---

## Getting Started

### Install

```bash
# Clone and build
git clone https://github.com/StudioBalmung/rsharp
cd rsharp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build        # installs rsl, rsc, rsharp to /usr/local/bin
```

### Verify

```bash
rsharp --version
# rsharp 1.0.2 (rsl 1.0.2, rsc 1.0.2, edition 2026)
```

### Create Your First Project

```bash
rsl new hello_world
cd hello_world
rsl run
# Hello from hello_world!
```

---

## Hello World

**Full form:**
```rsl
fn main() => i32 {
    @print("Hello, World!")
    0
}
```

**Short form** (expression body):
```rsl
fn main() => @print("Hello, World!")
```

Both are valid. The short form works for any function whose body is a single expression.

---

## Variables & Types

### Immutable binding (`let`)
```rsl
let x: i32 = 42          // annotated
let y = 3.14              // inferred as f64
let name = "R#"           // inferred as str
```

### Mutable binding (`var`)
```rsl
var counter: i32 = 0
counter += 1              // OK
```

### Constants
```rsl
const MAX_SIZE: usize = 1024
const PI: f64 = 3.14159265358979
```

### Primitive Types

| Type | Description | Range / Notes |
|------|-------------|---------------|
| `i8`–`i128` | Signed integers | 8–128 bit |
| `u8`–`u128` | Unsigned integers | 8–128 bit |
| `isize`/`usize` | Pointer-sized int | Platform-dependent |
| `f32`/`f64` | IEEE-754 floats | |
| `bool` | Boolean | `true` / `false` |
| `char` | Unicode scalar | 4 bytes |
| `str` | String slice | immutable `&[u8]` |
| `void` | No value | |

### Integer Literals
```rsl
let dec = 1_000_000      // decimal with separators
let hex = 0xFF_00        // hexadecimal
let bin = 0b1010_0011    // binary
let oct = 0o755          // octal
```

### Composite Types
```rsl
// Fixed array
let arr: [i32; 4] = [1, 2, 3, 4]

// Slice (fat pointer: ptr + len)
let s: []i32 = arr[1..3]     // [2, 3]

// Tuple
let t: (i32, f64, bool) = (1, 2.0, true)
let (a, b, c) = t            // destructure

// Optional (no null pointers anywhere else)
let maybe: ?i32 = null
let val = maybe ?? 0         // null-coalescing

// Result
let r: Result<i32, str> = Ok(42)
```

---

## Functions

### Basic
```rsl
fn add(a: i32, b: i32) => i32 {
    a + b                // last expression is returned (no `return` needed)
}
```

### Short (expression body)
```rsl
fn square(x: f64) => f64 { x * x }
fn greet(name: str) => @print("Hello, {}!", name)
```

### Multiple return / tuple
```rsl
fn min_max(a: i32, b: i32) => (i32, i32) {
    (if a < b { a } else { b }, if a > b { a } else { b })
}
let (lo, hi) = min_max(3, 7)
```

### Named parameters (clarity on call site)
```rsl
fn create_window(title: str, width: u32, height: u32) => void { ... }

create_window(title: "My App", width: 800, height: 600)
```

### Async functions
```rsl
async fn fetch(url: str) => Result<str, IoError> {
    let conn = await tcp::connect(url)?
    defer conn.close()
    await conn.read_all()
}
```

### Closures
```rsl
let double = |x: i32| => i32 { x * 2 }
let result = double(5)   // 10

// Captures from environment
var count = 0
let inc = || => void { count += 1 }
inc(); inc()
@print("{}", count)      // 2
```

---

## Control Flow

### if / else (expression)
```rsl
let grade = if score >= 90 { "A" }
            else if score >= 80 { "B" }
            else { "C" }
```

### for (iterator)
```rsl
for i in 0..10 { @print("{}", i) }        // 0..9
for i in 0..=10 { @print("{}", i) }       // 0..10 inclusive
for item in list { process(item) }
for (i, item) in list.enumerate() { ... }
```

### while
```rsl
while running { tick() }
while let Some { val } = iter.next() { use(val) }
```

### loop (infinite — must break)
```rsl
let result = loop {
    let val = compute()
    if val > 0 { break val }      // break with value
}
```

### match (exhaustive)
```rsl
match status {
    200 => "OK",
    404 => "Not Found",
    500 => "Server Error",
    _   => "Unknown",
}

match point {
    (0, 0) => "origin",
    (x, 0) => @fmt("on x-axis at {}", x),
    (0, y) => @fmt("on y-axis at {}", y),
    (x, y) => @fmt("({}, {})", x, y),
}
```

### defer (guaranteed cleanup)
```rsl
fn read_file(path: str) => Result<str, IoError> {
    let f = try File::open(path)
    defer f.close()          // runs when function exits, even on error
    f.read_all(&arena)
}
```

---

## Structs & Methods

```rsl
struct Vec2 {
    pub x: f32,
    pub y: f32,
}

impl Vec2 {
    // Constructor convention
    pub fn new(x: f32, y: f32) => Vec2 { Vec2 { x, y } }
    pub fn zero() => Vec2 { Vec2 { x: 0.0, y: 0.0 } }

    // Method: takes self by value (copy for small types)
    pub fn len(self) => f32 { @sqrt(self.x*self.x + self.y*self.y) }

    pub fn add(self, other: Vec2) => Vec2 {
        Vec2 { x: self.x + other.x, y: self.y + other.y }
    }

    pub fn scale(self, s: f32) => Vec2 {
        Vec2 { x: self.x * s, y: self.y * s }
    }

    pub fn dot(self, other: Vec2) => f32 {
        self.x * other.x + self.y * other.y
    }

    pub fn normalize(self) => Vec2 {
        let l = self.len()
        if l == 0.0 { return Vec2.zero() }
        self.scale(1.0 / l)
    }
}

// Usage
let a = Vec2.new(3.0, 4.0)
let b = Vec2.new(1.0, 0.0)
@print("len = {}", a.len())        // 5.0
@print("dot = {}", a.dot(b))      // 3.0
```

---

## Enums & Pattern Matching

```rsl
enum Color {
    Red,
    Green,
    Blue,
    Custom { r: u8, g: u8, b: u8 },
    Hex    { value: u32 },
}

fn to_rgb(c: Color) => (u8, u8, u8) {
    match c {
        .Red                    => (255, 0,   0),
        .Green                  => (0,   255, 0),
        .Blue                   => (0,   0,   255),
        .Custom { r, g, b }     => (r, g, b),
        .Hex    { value }       => (
            (value >> 16) as u8,
            (value >> 8 & 0xFF) as u8,
            (value & 0xFF) as u8,
        ),
    }
}

// Guards in match arms
match score {
    n if n >= 90 => "A",
    n if n >= 80 => "B",
    n if n >= 70 => "C",
    _            => "F",
}
```

---

## Ownership & Borrowing

R# uses **single ownership** — every value has exactly one owner.

### Move semantics
```rsl
let a = Box::new(&arena, 42)
let b = a            // a is MOVED to b
// @print(a)         // ERROR: use of moved value 'a'
//   note: 'a' was moved to 'b' on line 2
//   help: if you need both, use .clone() or restructure
```

### Borrowing (no ownership transfer)
```rsl
fn print_len(data: []i32) => void {    // borrows slice
    @print("len = {}", data.len)
}

let nums = [1, 2, 3, 4, 5]
print_len(nums)                        // borrow
print_len(nums)                        // still valid — not moved
```

### The borrow rules
- Any number of **shared borrows** (`&T`) at the same time, OR
- Exactly **one mutable borrow** (`*T`) — no shared borrows simultaneously
- Borrows cannot outlive the owner

```rsl
var x = 10
let r1 = &x            // shared borrow
let r2 = &x            // another shared borrow — OK
// let rm = &mut x     // ERROR: cannot borrow mutably while borrowed
@print("{} {}", r1.*, r2.*)
// r1, r2 dropped here — mutable borrow now allowed
```

### Unsafe
```rsl
unsafe {
    let ptr: *u8 = @cast(*u8, some_addr)
    ptr.* = 0xFF        // raw pointer write
}
```

---

## Error Handling

R# uses `Result<T, E>` — **no exceptions**.

### try (propagate errors)
```rsl
fn parse_and_open(path: str) => Result<File, AppError> {
    let validated = try validate_path(path)    // returns Err early if fails
    let f         = try File::open(validated)
    Ok(f)
}
```

### catch (handle inline)
```rsl
let content = read_file("config.rsl") catch |e| {
    @print("warning: {} — using defaults", e.to_str())
    DEFAULT_CONFIG
}
```

### match on Result
```rsl
match compute_result() {
    .Ok  { value } => @print("success: {}", value),
    .Err { error } => @print("failed: {}", error),
}
```

### Null safety with `?T`
```rsl
fn find_user(id: u32) => ?User { ... }

let user = find_user(42) ?? User.guest()
let name = find_user(1)?.name ?? "anonymous"   // chained optional access
```

---

## Generics & Comptime

```rsl
// Comptime type parameter — zero runtime cost
fn identity(comptime T: type, val: T) => T { val }

let x = identity(i32, 42)
let s = identity(str, "hello")
```

```rsl
// Generic data structure
fn Stack(comptime T: type, comptime Cap: u32) => type {
    struct {
        data:  [T; Cap],
        count: u32,
        fn push(self, v: T) => void { ... }
        fn pop(self) => ?T { ... }
    }
}

var s = Stack(i32, 16){}
s.push(1); s.push(2)
```

```rsl
// Comptime if — compile-time platform branching
fn platform_path_sep() => char {
    comptime if @OS == .Windows { '\' } else { '/' }
}
```

---

## Interfaces (Traits)

```rsl
interface Display {
    fn display(self) => str
}

interface Comparable {
    fn compare(self, other: @Self()) => i32   // -1, 0, 1
    fn eq(self, other: @Self()) => bool { self.compare(other) == 0 }
    fn lt(self, other: @Self()) => bool { self.compare(other) < 0 }
}

struct Temperature { celsius: f64 }

impl Display for Temperature {
    fn display(self) => str {
        @fmt("{:.1}°C ({:.1}°F)", self.celsius, self.celsius * 9.0/5.0 + 32.0)
    }
}

impl Comparable for Temperature {
    fn compare(self, other: Temperature) => i32 {
        if self.celsius < other.celsius { -1 }
        else if self.celsius > other.celsius { 1 }
        else { 0 }
    }
}

fn print_display(d: Display) => void { @print("{}", d.display()) }

let t = Temperature { celsius: 100.0 }
print_display(t)    // "100.0°C (212.0°F)"
```

---

## Memory: Arena Allocator

R# uses **explicit arena allocation**. There is no GC and no hidden `malloc`.

```rsl
use core::Arena

fn process(data: []u8) => void {
    let arena = Arena.init(1024 * 1024)   // 1 MB
    defer arena.deinit()                  // freed here

    // All allocations from this arena
    let buf  = arena.alloc(u8, 256)
    let list = Vec.new(&arena)

    list.push(42)
    list.push(99)

    // Checkpoint / rewind for temp allocations
    let cp = arena.checkpoint()
    let temp = arena.alloc(u8, 4096)   // scratch
    process_temp(temp)
    arena.rewind(cp)                    // temp freed, list still valid

    // arena.deinit() called by defer — everything freed
}
```

**Why arenas?**
- Zero fragmentation
- O(1) allocation (bump pointer)
- Free everything at once (O(1) deallocation)
- Cache-friendly (linear allocation)
- Explicit, predictable lifetimes

---

## Smart Pointers

| Type | Ownership | Thread-safe | Use case |
|------|-----------|-------------|----------|
| `Box<T>` | Unique | Yes | Heap value, recursive types |
| `Rc<T>` | Shared | ❌ No | Single-threaded shared state |
| `Arc<T>` | Shared | ✅ Yes | Multi-threaded shared state |
| `Arc<Mutex<T>>` | Shared + mut | ✅ Yes | Shared mutable state |

```rsl
use core::mem::{ Box, Rc, Arc }
use core::sync::Mutex

// Unique ownership
let b = Box::new(&arena, 42)
@print("{}", b.get().*)    // 42

// Shared (single-thread)
let rc1 = Rc::new(&arena, "hello")
let rc2 = rc1.clone()      // ref count: 2
@print("{}", rc2.get().*) // "hello"

// Shared + thread-safe + mutable
let state = Arc::new(&arena, Mutex::new(0i32))
let s2 = state.clone()
spawn fn() {
    let guard = s2.get().lock()
    guard.* += 1
}
```

---

## Concurrency

```rsl
// spawn a task
let handle = spawn async {
    do_work()
}
await handle

// Channel communication
let ch = Channel::bounded(16)
spawn async { ch.send(42) }
let val = await ch.recv()   // 42

// Mutex for shared state
let m = Mutex::new(0i32)
{
    let guard = m.lock()
    guard.* += 1
}   // guard dropped = lock released
```

---

## FFI: C Interop

```rsl
// Import C function
@extern("C") fn abs(x: i32) => i32
@extern("C") fn printf(fmt: *u8, ...) => i32

// C-compatible struct layout
@repr("C")
struct CPoint { x: f64, y: f64 }

// Export R# function to C
@export("my_fn")
pub fn compute(x: i32) => i32 { x * 2 }

// Raw pointer in unsafe block
unsafe {
    let ptr: *u8 = @cast(*u8, some_int_address)
    ptr.* = 0xFF
}
```

---

## Modules & Packages

```rsl
// src/math/vec3.rsl
mod math::vec3

pub struct Vec3 { pub x: f32, pub y: f32, pub z: f32 }
pub fn dot(a: Vec3, b: Vec3) => f32 { ... }
```

```rsl
// src/main.rsl
use math::vec3::{ Vec3, dot }
use math::vec3      // use math::vec3.dot(...)
use core::{ Arena, Result }
use core::*         // glob import
```

### Rsharp.toml (project manifest)
```toml
[package]
name        = "my_project"
version     = "0.1.0"
edition     = "2025"

[dependencies]
rmath    = "1.2"
rgl      = { path = "./lib/rgl.rslib" }
json     = { git = "https://github.com/rsharp/json", tag = "v0.3" }

[profile.release]
opt_level = 2
lto       = true
```

### rsl commands
```bash
rsl new myproject        # create project
rsl build                # debug build
rsl build --release      # release build
rsl build pkg            # build library
rsl run                  # build + run
rsl run -- arg1 arg2     # pass args to program
rsl check                # type-check only
rsl test                 # run tests
rsl fmt                  # format source
rsl doc                  # generate docs
rsl install qt5          # install package
rsl add serde@1.0        # add dependency
rsl clean                # clean build artifacts
rsharp --version         # print version
```

---

## Builtins Reference

Builtins are prefixed with `@` and are resolved at compile time.

| Builtin | Signature | Description |
|---------|-----------|-------------|
| `@print` | `(fmt: str, args...)` | Formatted print to stdout |
| `@eprint` | `(fmt: str, args...)` | Formatted print to stderr |
| `@fmt` | `(fmt: str, args...) => str` | Format to string |
| `@assert` | `(cond: bool, msg: str)` | Debug assertion (stripped in release) |
| `@panic` | `(msg: str) => !` | Unconditional abort |
| `@unreachable` | `() => !` | Mark code path as unreachable |
| `@sizeof` | `(T: type) => usize` | Size of type in bytes |
| `@alignof` | `(T: type) => usize` | Alignment of type |
| `@cast` | `(T: type, v) => T` | Explicit type cast |
| `@bitcast` | `(T: type, v) => T` | Reinterpret bits |
| `@sqrt` | `(x: f64) => f64` | Square root (intrinsic) |
| `@sin`/`@cos`/`@tan` | `(x: f64) => f64` | Trig intrinsics |
| `@min`/`@max` | `(a: T, b: T) => T` | Min/max |
| `@abs` | `(x: T) => T` | Absolute value |
| `@hash` | `(v: T) => u64` | Hash a value |
| `@eq` | `(a: T, b: T) => bool` | Equality (for generic code) |
| `@OS` | `=> OsKind` | Target OS (comptime) |
| `@CPU` | `=> CpuKind` | Target CPU arch (comptime) |
| `@import` | `(path: str)` | Import module |
| `@extern` | `("C")` | Declare C extern function |
| `@export` | `("name")` | Export function to C |
| `@repr` | `("C")` | C-compatible memory layout |
| `@This` | `() => type` | Current struct type |
| `@zero_init` | `() => T` | Zero-initialise any type |
| `@inf_f64` | `() => f64` | IEEE 754 infinity |
| `@nan_f64` | `() => f64` | IEEE 754 NaN |
| `@is_inf` | `(x: f64) => bool` | Check for infinity |
| `@parse_i32` | `(s: str) => Result<i32, str>` | Parse integer from string |
| `@int_to_str` | `(n: i32) => str` | Integer to string |
