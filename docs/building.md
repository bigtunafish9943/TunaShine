# Building TunaStream

TunaStream is built for Windows with [MSYS2](https://www.msys2.org). The quickest way to get an installer is to
push to this repository and let GitHub Actions build it; the steps below are what that build does.

## Requirements

- Windows 10 or later
- [MSYS2](https://www.msys2.org), using the **UCRT64** shell
- [Node.js](https://nodejs.org) 22 or later, installed for Windows (not the MSYS2 package)

In the UCRT64 shell, install the build tools:

```bash
pacman -Syu
pacman -S git \
  mingw-w64-ucrt-x86_64-boost mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-cppwinrt \
  mingw-w64-ucrt-x86_64-curl-winssl mingw-w64-ucrt-x86_64-MinHook mingw-w64-ucrt-x86_64-miniupnpc \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-nlohmann_json mingw-w64-ucrt-x86_64-nsis \
  mingw-w64-ucrt-x86_64-onevpl mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-opus \
  mingw-w64-ucrt-x86_64-toolchain
```

## Get the source

```bash
git clone https://github.com/bigtunafish9943/TunaShine.git tunastream
cd tunastream
git submodule update --init --recursive --depth 1 \
  third-party/build-deps third-party/libdisplaydevice third-party/moonlight-common-c \
  third-party/nanors third-party/nv-codec-headers third-party/nvapi-open-source-sdk \
  third-party/Simple-Web-Server third-party/tray third-party/ViGEmClient
```

## Build

Make sure `node` is on the PATH of the UCRT64 shell, then:

```bash
cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF
ninja -C build
```

## Package

```bash
cd build
cpack -G NSIS   # installer
cpack -G ZIP    # portable build
```

The results are in `build/cpack_artifacts`.

## Check it

`scripts/ci/smoke-test.sh` starts the portable build and checks that the web interface, login and TunaStream's
own settings and actions work. From Git Bash:

```bash
bash scripts/ci/smoke-test.sh build/cpack_artifacts/TunaStream.zip "$PWD" <version>
```
