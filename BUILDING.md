# libdatachannel - Building instructions

## Clone repository

```bash
$ git clone https://github.com/paullouisageneau/libdatachannel.git
```

## Init submodules

This step is optional if `PREFER_SYSTEM_LIB` CMake option will be enabled.

```bash
$ cd libdatachannel
$ git submodule update --init --recursive --depth 1
```

## Build with CMake

The CMake library targets `libdatachannel` and `libdatachannel-static` respectively correspond to the shared and static libraries. The default target will build tests and examples.

The option `USE_GNUTLS` allows to switch between OpenSSL (default) and GnuTLS, and the option `USE_NICE` allows to switch between libjuice as submodule (default) and libnice.

The option `PREFER_SYSTEM_LIB` allow to link against the system library rather than building all the submodule.
Options `USE_SYSTEM_SRTP`, `USE_SYSTEM_JUICE`, `USE_SYSTEM_USRSCTP`, `USE_SYSTEM_PLOG` and `USE_SYSTEM_JSON` allow to do the same but per submodule, for libsrtp, libjuice, libusrsctp, Plog and Nlohmann JSON respectively.

If you only need Data Channels, the option `NO_MEDIA` allows to make the library lighter by removing media support. Similarly, `NO_WEBSOCKET` removes WebSocket support.

For the sake of performance, the library should be compiled in `Release` mode if you don't plan to debug it.

The CMake build exports the targets with namespace `LibDataChannel::LibDataChannel` and `LibDataChannel::LibDataChannelStatic` to link the library from another CMake project.

### POSIX-compliant operating systems (including Linux and Apple macOS)

```bash
$ cmake -B build -DUSE_GNUTLS=0 -DUSE_NICE=0 -DCMAKE_BUILD_TYPE=Release
$ cd build
$ make -j2
```

### Apple macOS with Xcode project

To generate an Xcode project in the `build` directory:

```bash
$ cmake -B build -G Xcode -DUSE_GNUTLS=0 -DUSE_NICE=0
```

#### Solving "Could NOT find OpenSSL" error

You need to add OpenSSL root directory if the build fails with the following message:

```
Could NOT find OpenSSL, try to set the path to OpenSSL root folder in the system variable OPENSSL_ROOT_DIR (missing: OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR)
```

For example:

```bash
$ cmake -B build -G Xcode -DUSE_GNUTLS=0 -DUSE_NICE=0 -DOPENSSL_ROOT_DIR=/usr/local/Cellar/openssl\@1.1/1.1.1h/
```

### Microsoft Windows with MinGW cross-compilation

```bash
$ cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/usr/share/mingw/toolchain-x86_64-w64-mingw32.cmake # replace with your toolchain file
$ cd build
$ make -j2
```

### Microsoft Windows with Microsoft Visual C++

```bash
$ cmake -B build -G "NMake Makefiles"
$ cd build
$ nmake
```

## Build directly with Make (Linux only)

The option `USE_GNUTLS` allows to switch between OpenSSL (default) and GnuTLS, and the option `USE_NICE` allows to switch between libjuice as submodule (default) and libnice.

If you only need Data Channels, the option `NO_MEDIA` removes media support. Similarly, `NO_WEBSOCKET` removes WebSocket support.

```bash
$ make USE_GNUTLS=0 USE_NICE=0
```

