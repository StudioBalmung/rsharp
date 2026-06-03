# R# Language Specification 
## A Systems Programming Language: Zero-Overhead Safety, Minimal Ceremony

---

## Design Philosophy

R# occupies the space between Zig's explicit control and Rust's safety guarantees,
with Lua's approachability. The core tenets:

- **Explicit over implicit** - no hidden allocations, no hidden copies
- **Gradual safety** - safe by default, unsafe blocks opt-in (like Rust, unlike C)
- **Readable errors** - compiler errors are explanations, not codes
- **Single ownership, simple syntax** - ownership without lifetime annotations in most cases
- **Compiles to native** - LLVM backend, zero GC overhead in the hot path

---

## File Format

Source files use the `.rss` extension.
Entry point: `fn main() -> i32`

---

## Type System

### Primitives
```rss
i8, i16, i32, i64, i128     -- signed integers
u8, u16, u32, u64, u128     -- unsigned integers
f32, f64                     -- IEEE 754 floats
bool                         -- true / false
char                         -- Unicode scalar (4 bytes)
void                         -- no value
```

### Compound Types
```rss
-- Fixed array
let arr: [i32; 8] = [0; 8]

-- Slice (fat pointer: ptr + len)
let s: []i32 = arr[0..4]

-- Tuple
let t: (i32, f64, bool) = (1, 3.14, true)

-- Optional (replaces null)
let maybe: ?i32 = null
let val: i32   = maybe ?? 0    -- null-coalescing

-- Result (replaces exceptions)
let r: Result<i32, IoError> = file.read()
let n: i32 = r catch |e| { log(e); 0 }
```

### Structs
```rss
struct Vec3 {
    x: f32,
    y: f32,
    z: f32,
}

impl Vec3 {
    fn new(x: f32, y: f32, z: f32) -> Vec3 {
        Vec3 { x, y, z }
    }

    fn dot(self, other: Vec3) -> f32 {
        self.x * other.x + self.y * other.y + self.z * other.z
    }

    fn len(self) -> f32 {
        @sqrt(self.dot(self))
    }
}
```

### Enums (Algebraic)
```rss
enum Shape {
    Circle { radius: f32 },
    Rect   { w: f32, h: f32 },
    Point,
}

fn area(s: Shape) -> f32 {
    match s {
        .Circle { radius } => @PI * radius * radius,
        .Rect   { w, h }   => w * h,
        .Point             => 0.0,
    }
}
```

### Interfaces (Traits)
```rss
interface Drawable {
    fn draw(self, ctx: *RenderCtx) -> void
    fn bounds(self) -> Rect
}

impl Drawable for Vec3 {
    fn draw(self, ctx: *RenderCtx) -> void {
        ctx.point(self.x, self.y)
    }
    fn bounds(self) -> Rect { Rect.point(self.x, self.y) }
}
```

---

## Memory Model

### Ownership (Default — Safe Mode)
```rss
fn example() -> void {
    let a = Box.new(42)    -- heap allocation, owned
    let b = a              -- MOVE: a is no longer valid
    -- use a here → compile error: "a was moved to b on line 3"
}
```

### Borrowing (No Lifetime Syntax Needed in 95% of Cases)
```rss
fn sum(data: []i32) -> i64 {   -- borrows slice, no annotation needed
    var total: i64 = 0
    for x in data { total += x }
    total
}
```

### Explicit Lifetimes (Edge Cases Only)
```rss
-- Only needed when returning borrowed data
fn first<'a>(slice: []'a i32) -> ?'a i32 {
    if slice.len == 0 { null } else { slice[0] }
}
```

### Unsafe Block
```rss
unsafe {
    let raw: *u8 = @cast(*u8, ptr)
    *raw = 0xFF
}
```

### Memory Arenas (Built-in)
```rss
let arena = Arena.init(1024 * 1024)   -- 1 MB arena
defer arena.deinit()

let buf = arena.alloc(u8, 256)        -- arena-local, freed with arena
```

---

## Control Flow

```rss
-- if / else (expression-oriented)
let msg = if score > 90 { "A" } else if score > 80 { "B" } else { "C" }

-- for (iterator-based)
for i in 0..10 { @print("{}", i) }
for item in list { process(item) }

-- while
while running { tick() }

-- loop (infinite, must break or return)
let result = loop {
    if found { break value }
}

-- match (exhaustive)
match status {
    .Ok    => handle_ok(),
    .Error => handle_error(),
}

-- defer (RAII-style cleanup)
fn read_file(path: []u8) -> Result<[]u8, IoError> {
    let fd = try fs.open(path)
    defer fd.close()
    fd.read_all()
}
```

---

## 6. Error Handling

```rss
-- try propagates errors (like Rust ?)
fn parse_config(path: []u8) -> Result<Config, Error> {
    let data = try fs.read(path)
    let cfg  = try Json.parse(data)
    cfg
}

-- catch inline
let value = parse_config("app.json") catch |e| {
    log.warn("config failed: {}", e)
    Config.default()
}

-- assert (debug-only crash, stripped in release)
@assert(ptr != null, "expected non-null pointer")
```

---

## Comptime (Compile-Time Execution)

```rss
-- Generic functions via comptime
fn max(comptime T: type, a: T, b: T) -> T {
    if a > b { a } else { b }
}

-- Comptime conditional compilation
fn platform_init() -> void {
    comptime if @OS == .Windows {
        win32_init()
    } else {
        posix_init()
    }
}

-- Comptime struct generation
fn Vec(comptime T: type, comptime N: u32) -> type {
    struct {
        data: [T; N],
        fn len(self) -> u32 { N }
    }
}

let v3 = Vec(f32, 3){ .data = .{0, 0, 0} }
```

---

## Concurrency

```rss
-- Async/await (stackless coroutines, no runtime needed)
async fn fetch(url: []u8) -> Result<[]u8, HttpError> {
    let conn = await tcp.connect(url)
    defer conn.close()
    await conn.read_all()
}

-- Channels
let ch = Channel(i32).bounded(16)
spawn fn() { ch.send(42) }
let val = ch.recv()

-- Mutex
let m = Mutex(i32).new(0)
{
    let guard = m.lock()
    guard.* += 1
}
```

---

## FFI (C Interop)

```rss
-- Declare C functions
@extern("C") fn malloc(size: usize) -> *void
@extern("C") fn free(ptr: *void) -> void

-- Export to C
@export("my_function")
fn compute(x: i32) -> i32 { x * 2 }

-- C struct layout compatibility
@repr("C")
struct CPoint { x: i32, y: i32 }
```

---

## Built-in Builtins (@ prefix)

| Builtin         | Description                          |
|-----------------|--------------------------------------|
| `@sizeof(T)`    | Size of type at compile time         |
| `@alignof(T)`   | Alignment of type                    |
| `@cast(T, v)`   | Type cast (explicit)                 |
| `@bitcast(T,v)` | Reinterpret bits                     |
| `@sqrt(x)`      | Intrinsic sqrt                       |
| `@assert(c,m)`  | Debug assertion                      |
| `@panic(m)`     | Unconditional abort with message     |
| `@print(f,...)` | Formatted print (comptime format)    |
| `@OS`           | Target OS enum (comptime)            |
| `@CPU`          | Target CPU arch (comptime)           |
| `@import("x")`  | Import module or C header            |

---

## Module System

```rss
-- math.rss
pub fn add(a: i32, b: i32) -> i32 { a + b }
pub const PI: f64 = 3.14159265358979

-- main.rss
let math = @import("math")
let x = math.add(1, 2)
let c = math.PI * 2.0
```
