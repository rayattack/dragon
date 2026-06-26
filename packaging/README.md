# Packaging Dragon

Dragon's installers are produced by **CPack**, driven straight from the
`install()` rules in the root `CMakeLists.txt` - those rules are the single
source of truth for the on-disk layout, so packages can never drift from a
plain `cmake --install`.

| Platform | Generator | Artifact                         |
|----------|-----------|----------------------------------|
| Linux    | `DEB`     | `dragon_<ver>_amd64.deb`         |
| Linux    | `RPM`     | `dragon-<ver>-1.x86_64.rpm`      |
| Linux    | `TGZ`     | `dragon-<ver>-Linux.tar.gz`      |
| macOS    | `DragNDrop` | `dragon-<ver>-Darwin.dmg`      |
| macOS    | `TGZ`     | `dragon-<ver>-Darwin.tar.gz`     |
| Windows  | `WIX`     | `dragon-<ver>-win64.msi`         |
| Windows  | `ZIP`     | `dragon-<ver>-win64.zip`         |

## Installed layout (relocatable)

```
<prefix>/bin/dragon              the CLI (and a `dr` symlink on POSIX)
<prefix>/lib/dragon/*.a          statically-linked runtime + bundled libs
<prefix>/share/dragon/stdlib/    the .dr standard library
<prefix>/share/dragon/certs/     bundled CA trust store
```

The binary finds its stdlib by walking up from its own path
(`Driver.cpp::resolveStdlibDir` → `<prefix>/share/dragon/stdlib`), so the tree
works whether it's installed at `/usr` or extracted to `/opt`/anywhere.

## Build packages locally

From a configured build directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build -j

cd build
cpack -G DEB -B dist     # needs dpkg-deb   (Debian/Ubuntu)
cpack -G RPM -B dist     # needs rpmbuild   (`apt install rpm` on Ubuntu)
cpack -G TGZ -B dist     # portable tarball
```

`DragNDrop` requires macOS; `WIX` requires the WiX Toolset on Windows.

## Release pipeline

`.github/workflows/release.yml` builds every target on its native runner,
runs the matching `cpack` generators, **renames** the artifacts to the scheme
the download page expects, and uploads them to a (draft) GitHub Release:

```
dragon-<ver>-linux-x86_64.{deb,rpm,tar.gz}
dragon-<ver>-macos-arm64.{dmg,tar.gz}
dragon-<ver>-macos-x86_64.{dmg,tar.gz}
dragon-<ver>-windows-x86_64.{msi,zip}
```

These exact names are what `dragonlang-org/src/views/www.dr` links to once
`GITHUB_REPO` is set there. Cutting a release:

1. Bump `project(Dragon VERSION ...)` in `CMakeLists.txt` and `VERSION` in
   `include/dragon.h` to the same value.
2. Set `GITHUB_REPO` (and confirm `DRAGON_VERSION`) in `www.dr`.
3. Push a matching tag, e.g. `git tag v0.0.1 && git push origin v0.0.1`.
4. Review the draft release the workflow creates, then publish.

> **LLVM in CI.** `LLVM_VERSION` in the workflow must match the LLVM the
> codebase compiles against (the local dev tree uses a custom build). Pin it
> before tagging - the Windows job in particular depends on a usable prebuilt
> LLVM, and is marked experimental so a failure there won't block the
> Linux/macOS artifacts.
