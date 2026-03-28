# Forge-Point

**Forge-Point** is a local-first C++ terminal cockpit for discovering GGUF models, detecting cached models, downloading selected files from Hugging Face, and starting or stopping `llama-server` from a single TUI.

## Features

- **FTXUI-powered terminal UI** with a welcome splash screen and colored ASCII art.
- **Embedded Hugging Face integration** using HTTP inside the app.
- **GGUF detection** from both:
  - the project's `models/` directory
  - the user's Hugging Face cache directory
- **Bundled `llama.cpp` runtime layout** under `runtime/llama.cpp/`
- **Start / stop / health check** for `llama-server`
- **Slash command palette** opened with `/`
- **Autocomplete for slash commands** with `Tab`
- **YOLO mode** toggled with `Shift+Tab`
  - when YOLO is **off**, guarded actions ask for confirmation
  - when YOLO is **on**, they execute immediately

## Layout

```text
forge-point/
├── .gitignore
├── CMakeLists.txt
├── README.md
├── models/
│   └── .gitkeep
├── runtime/
│   └── llama.cpp/
│       └── .gitkeep
└── src/
    └── main.cpp
```

## Runtime folder

Place a prebuilt `llama.cpp` release in:

```text
runtime/llama.cpp/
```

Forge-Point will try to find:

- `runtime/llama.cpp/llama-server`
- `runtime/llama.cpp/bin/llama-server`
- any matching `llama-server` under that tree
- `llama-server` on `PATH`

## Build

Requirements:

- CMake 3.21+
- a C++20 compiler
- libcurl development package available to CMake

Build:

```bash
cmake -S . -B build
cmake --build build --config Release
```

Run:

```bash
./build/forge-point
```

On Windows, depending on the generator:

```powershell
.\build\Release\forge-point.exe
```

## Key bindings

- `Enter`: context-sensitive action (search → search repos, repo list → list files, file list → download, command palette → run command)
- `/`: focus the slash command palette
- `Tab`: cycle between panels (Local → Hub → Server → Command), or autocomplete in command palette
- `Shift+Tab`: cycle panels in reverse
- `Ctrl+Y`: toggle YOLO mode
- `Esc`: return focus to command palette
- `Arrow Up / Arrow Down`: move through slash command suggestions
- `q` or `Ctrl+C`: quit

## Slash commands

- `/help`
- `/search <query>`
- `/files`
- `/download`
- `/start`
- `/stop`
- `/health`
- `/rescan`
- `/refresh-binary`
- `/yolo [on|off|toggle]`
- `/welcome`
- `/focus <search|models|server|command>`

## Notes

- Hugging Face authentication is optional. If `HF_TOKEN` is present in the environment, Forge-Point sends it automatically.
- Downloaded models go into `models/<repo-id-normalized>/`.
- The project is intentionally compact so you can extend it into a richer local inference cockpit.
