name: Build without WebSocket
on:
  push:
    branches:
    - master
  pull_request:
    branches:
    - master
jobs:
  build-nowebsocket:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    - name: install packages
      run: sudo apt update && sudo apt install libssl-dev libsrtp2-dev
    - name: submodules
      run: git submodule update --init --recursive --depth 1
    - name: cmake
      run: cmake -B build -DNO_WEBSOCKET=1 -DUSE_SYSTEM_SRTP=1 -DWARNINGS_AS_ERRORS=1
    - name: make
      run: (cd build; make -j2)
    - name: test
      run: ./build/tests

