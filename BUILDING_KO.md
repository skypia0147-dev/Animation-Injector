# Animation Injector 빌드

## 필요한 것

- Windows, Visual Studio 2026 (MSVC, C++23, C++ 데스크톱 워크로드)
- CMake 3.20 이상
- [vcpkg](https://github.com/microsoft/vcpkg) (매니페스트 모드, 종속성은 `vcpkg.json`)
- Git (CMake가 CommonLibSSE-NG를 받아 옴)
- Python 3 (선택, `ConvertV4` 시험만 사용)

CommonLibSSE-NG는 이 저장소에 들어 있지 않습니다. `cmake/CommonLibSSE.cmake`가
[alandtse 포크](https://github.com/alandtse/CommonLibVR)를 고정 커밋으로 받아 함께 빌드합니다.

## 빌드

```
cmake -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg 루트>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --target AnimationInjector
```

플러그인은 `build/Release/AnimationInjector.dll`입니다. 모드에는 `assets/Interface/AnimationInjectorLoading.swf`를
`Data/Interface/`에, `misc/AnimationInjector.ini`를 `Data/SKSE/Plugins/`에 함께 넣어야 합니다.

빌드할 때마다 모드 폴더로 복사하려면 구성할 때 `-DAI_DEPLOY_DIR=<모드 폴더>/SKSE/Plugins`를 줍니다.

## 시험

```
cmake --build build --config Release --target AnimationInjectorAllTests
ctest --test-dir build -C Release --output-on-failure
```

시험은 게임 없이 돕니다. 공개하지 않는 게임 코드 캡처를 읽는 일부 검사는 건너뜁니다(구성 로그의
`Native game-code fixtures absent`).
