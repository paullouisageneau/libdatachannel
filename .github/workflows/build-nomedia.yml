name: Build without media
on:
  push:
    branches:
    - master
  pull_request:
jobs:
  build-nomedia:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    - name: install packages
      run: sudo apt update && sudo apt install libssl-dev
    - name: submodules
      run: git submodule update --init --recursive --depth 1
    - name: cmake
      run: cmake -B build -DUSE_GNUTLS=0 -DNO_MEDIA=1 -DWARNINGS_AS_ERRORS=1
    - name: make
      run: (cd build; make -j2)
    - name: test
      run: ./build/tests
  build-nomedia-windows:
    runs-on: windows-latest
    steps:
    - uses: actions/checkout@v2
    - uses: ilammy/msvc-dev-cmd@v1
    - name: install packages
      run: choco install openssl
    - name: submodules
      run: git submodule update --init --recursive --depth 1
    - name: cmake
      run: cmake -B build -G "NMake Makefiles" -DUSE_GNUTLS=0 -DNO_MEDIA=1 -DWARNINGS_AS_ERRORS=1
    - name: nmake
      run: |
        cd build
        set CL=/MP
        nmake
    - name: test
      run: build/tests.exe

