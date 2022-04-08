name: Build with GnuTLS
on:
  push:
    branches:
    - master
  pull_request:
jobs:
  build-linux:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    - name: install packages
      run: sudo apt update && sudo apt install libgnutls28-dev nettle-dev libsrtp2-dev
    - name: submodules
      run: git submodule update --init --recursive --depth 1
    - name: cmake
      run: cmake -B build -DUSE_GNUTLS=1 -DUSE_SYSTEM_SRTP=1 -DWARNINGS_AS_ERRORS=1
    - name: make
      run: (cd build; make -j2)
    - name: test
      run: ./build/tests
  build-macos:
    runs-on: macos-latest
    steps:
    - uses: actions/checkout@v2
    - name: install packages
      run: brew install gnutls nettle
    - name: submodules
      run: git submodule update --init --recursive --depth 1
    - name: cmake
      run: cmake -B build -DUSE_GNUTLS=1 -DWARNINGS_AS_ERRORS=1 -DENABLE_LOCAL_ADDRESS_TRANSLATION=1
    - name: make
      run: (cd build; make -j2)
    - name: test
      run: ./build/tests
