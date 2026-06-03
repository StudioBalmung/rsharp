# R# Package System

## Rsharp.toml Reference

```toml
[package]
name        = "my_project"
version     = "1.0.0"
edition     = "2026"
authors     = ["Neofilisoft <mail@placeholder.com>"]
description = "A brief description"
license     = "MIT"
homepage    = "https://example.com"
repository  = "https://github.com/StudioBalmung/rsharp"
keywords    = ["systems", "game", "tool"]

[dependencies]
# From registry (when live)
serde  = "1.0"
rmath  = "^2.1"

# From local path
mylib  = { path = "./lib/mylib" }

# From git
utils  = { git = "https://github.com/user/utils", tag = "v1.0" }

[dev-dependencies]
test_helpers = "0.2"

[profile.debug]
opt_level    = 0
assertions   = true
overflow_checks = true
debug_info   = true

[profile.release]
opt_level    = 2
assertions   = false
lto          = true
strip        = true

[profile.release-safe]
opt_level    = 2
assertions   = true    # keep assertions in production

[profile.size]
opt_level    = "s"
lto          = true

[targets.wasm32-wasi]
no_std       = false
opt_level    = "s"
```

## rsl Commands

```bash
rsl new <name>           # create project scaffold
rsl build                # debug build
rsl build --release      # release build
rsl build pkg            # build as .rslib library
rsl run                  # build + run
rsl run -- <args>        # pass args to the program
rsl check                # type-check without compiling
rsl test                 # build and run tests
rsl fmt                  # format all .rsl/.rsh/.rss files
rsl doc                  # generate docs to docs/api/
rsl install <pkg>        # install package globally
rsl add <pkg>            # add to current project
rsl add <pkg@version>    # specific version
rsl remove <pkg>         # remove dependency
rsl update               # update all dependencies
rsl clean                # remove build/ and *.o
rsharp --version         # print toolchain version
```

## Manual Library Installation

Until the registry is live:

1. Place `.rslib` in `./lib/`
2. Add to `Rsharp.toml`:
   ```toml
   [dependencies]
   mylib = { path = "./lib/mylib.rslib" }
   ```
3. In source:
   ```rsl
   use mylib::SomeType
   ```

## Writing a Library

```
mylib/
├── Rsharp.toml        (type = "lib")
├── src/
│   ├── lib.rsl        (pub mod declarations)
│   ├── parser.rsl
│   └── types.rsl
└── tests/
    └── test_parser.rsl
```

`Rsharp.toml`:
```toml
[package]
name = "mylib"
type = "lib"        # produces mylib.rslib
```

`src/lib.rsl`:
```rsl
pub use parser::Parser
pub use types::{ Token, TokenKind }
```

Build: `rsl build pkg`  →  `build/libmylib.rslib`
