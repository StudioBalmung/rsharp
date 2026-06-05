# Install R# On Windows

This makes R# usable like `rustc.exe`, `python.exe`, `g++.exe`, and `java.exe`.

## Build

```powershell
cmake -S . -B build -G Ninja
cmake --build build
```

Expected executables:

```text
build\rsharp.exe
build\rsl.exe
build\rsc.exe
build\rsrun.exe
build\rsfmt.exe
build\rsdoc.exe
```

## Add To PATH Temporarily

Current PowerShell window only:

```powershell
$env:Path = "C:\Users\BEST\Desktop\RSharp\build;$env:Path"
```

Check:

```powershell
rsharp --version
rsl --version
rsc --version
```

## Add To PATH Permanently

```powershell
[Environment]::SetEnvironmentVariable(
  "Path",
  "C:\Users\BEST\Desktop\RSharp\build;" + [Environment]::GetEnvironmentVariable("Path", "User"),
  "User"
)
```

Open a new terminal:

```powershell
where.exe rsharp
rsharp --version
```

## Use

```powershell
rsc --check examples\hello_world.rsl
rsl build
rsl run
rsl check
```

## Lua Note

`C:\Users\BEST\Desktop\Repo\Lua` is Vanilla Lua, not LuaJIT. It reports:

```text
Lua 5.5.0  Copyright (C) 1994-2025 Lua.org, PUC-Rio
```
