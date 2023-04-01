name: Build with Mbed TLS
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
    - name: Set up Homebrew
      uses: Homebrew/actions/setup-homebrew@master
    - name: Install Mbed TLS
      run: brew update && brew install mbedtls
    - name: submodules
      run: git submodule update --init --recursive --depth 1
    - name: cmake
      run: cmake -B build -DUSE_MBEDTLS=1 -DWARNINGS_AS_ERRORS=1  -DCMAKE_PREFIX_PATH=$(brew --prefix mbedtls)
    - name: make
      run: (cd build; make -j2)
    - name: test
      run: ./build/tests
  build-macos:
    runs-on: macos-latest
    steps:
    - uses: actions/checkout@v2
    - name: Install Mbed TLS
      run: brew update && brew install mbedtls
    - name: submodules
      run: git submodule update --init --recursive --depth 1
    - name: cmake
      run: cmake -B build -DUSE_MBEDTLS=1 -DWARNINGS_AS_ERRORS=1 -DENABLE_LOCAL_ADDRESS_TRANSLATION=1  -DCMAKE_PREFIX_PATH=$(brew --prefix mbedtls)
    - name: make
      run: (cd build; make -j2)
    - name: test
      run: ./build/tests
