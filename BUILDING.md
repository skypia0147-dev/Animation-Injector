# Building Animation Injector

## Requirements

- Windows, Visual Studio 2026 (MSVC, C++23) with the C++ desktop workload
- CMake 3.20 or newer
- [vcpkg](https://github.com/microsoft/vcpkg) (manifest mode; the dependencies are in `vcpkg.json`)
- Git (CMake fetches CommonLibSSE-NG)
- Python 3 (optional; only the `ConvertV4` test uses it)

CommonLibSSE-NG is not part of this repository. `cmake/CommonLibSSE.cmake` fetches the
[alandtse fork](https://github.com/alandtse/CommonLibVR) at a pinned commit and builds it with the project.

## Build

```
cmake -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --target AnimationInjector
```

The plugin is `build/Release/AnimationInjector.dll`. The mod also needs `assets/Interface/AnimationInjectorLoading.swf`
at `Data/Interface/` and `misc/AnimationInjector.ini` at `Data/SKSE/Plugins/`.

To copy the DLL and its files into a mod folder after each build, configure with
`-DAI_DEPLOY_DIR=<mod folder>/SKSE/Plugins`.

## Tests

```
cmake --build build --config Release --target AnimationInjectorAllTests
ctest --test-dir build -C Release --output-on-failure
```

The tests run without the game. A few checks read captured game code that is not published; they are skipped
here (`Native game-code fixtures absent` in the configure log).
