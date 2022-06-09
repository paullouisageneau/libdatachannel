name: Build with libnice
on:
  push:
    branches:
    - master
  pull_request:
jobs:
  build-nice:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    - name: install packages
      run: sudo apt update && sudo apt install libgnutls28-dev libnice-dev
    - name: submodules
      run: git submodule update --init --recursive --depth 1
    - name: cmake
      run: cmake -B build -DUSE_GNUTLS=1 -DUSE_NICE=1 -DWARNINGS_AS_ERRORS=1
    - name: make
      run: (cd build; make -j2)
    - name: test
      run: ./build/tests

