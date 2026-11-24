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

Options `USE_GNUTLS` and `USE_MBEDTLS` allow to switch the cryptographic backend to GnuTLS and Mbed TLS respectively, otherwise OpenSSL is selected by default. On Microsoft Windows, the option `USE_SCHANNEL` allows to use the native SChannel and CNG APIs instead, so no third-party TLS library is required (see [below](#microsoft-windows-with-schannel)). The option `USE_NICE` allows to switch between libjuice as submodule (default) and libnice as system library.

The option `PREFER_SYSTEM_LIB` allows to link against the system library rather than building all the submodule. Options `USE_SYSTEM_SRTP`, `USE_SYSTEM_JUICE`, `USE_SYSTEM_USRSCTP`, `USE_SYSTEM_PLOG` and `USE_SYSTEM_JSON` allow to do the same but per submodule, for libsrtp, libjuice, libusrsctp, Plog and Nlohmann JSON respectively.

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

### Microsoft Windows with SChannel

The option `USE_SCHANNEL` builds the library with the native Windows security APIs instead of a third-party TLS library: SChannel through SSPI for TLS and DTLS, CryptoAPI and CNG for certificates, keys, and hashes. It is only available on Windows and can't be combined with `USE_GNUTLS` or `USE_MBEDTLS`. It requires Microsoft Visual C++ with a Windows 10 SDK version 1809 (10.0.17763) or later.

```bash
$ cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DUSE_SCHANNEL=1
$ cd build
$ nmake
```

At runtime, Windows 10 version 1607 or later is required, as DTLS 1.2 and the keying material exporter used to derive SRTP keys are not available in previous versions (it was tested on Windows 11). TLS 1.3 is used for WebSockets when the system supports it (Windows 11 or Windows Server 2022). Protocol versions and cipher suites follow the system SChannel configuration.

Differences with the other backends:
- libSRTP is built with its internal crypto engine, which does not support AES-GCM, so only the `SRTP_AES128_CM_HMAC_SHA1_80` profile is negotiated (like with GnuTLS). AES-GCM profiles are also offered when using a system libSRTP supporting them (`USE_SYSTEM_SRTP`).
- SChannel can't use in-memory private keys, so generated or imported keys are persisted in the key store of the current user (Microsoft Software Key Storage Provider) under a unique `libdatachannel-` name, and deleted when the certificate is released. Keys left behind by a process which did not exit cleanly are deleted the next time a key is created. The process must have a loaded user profile.
- PEM keys may be PKCS#8 (`PRIVATE KEY`), encrypted PKCS#8 (`ENCRYPTED PRIVATE KEY`, with `keyPemPass`), PKCS#1 (`RSA PRIVATE KEY`), or SEC1 (`EC PRIVATE KEY`). Keys encrypted with the legacy OpenSSL format (`Proc-Type: 4,ENCRYPTED` header) are not supported.
- WebSocket server certificates are verified against the system root certificates, and the certificate set in `caCertificatePemFile` if any, unless `disableTlsVerification` is set. Note that verification is disabled on Windows with the other backends.
- SChannel can't send the DTLS close_notify alert, so DTLS connections are closed without alert.
- When retransmitting the DTLS client Finished message, SChannel reuses its record sequence number, so other implementations discard it as a replay. As a consequence, when SChannel is the DTLS client (which is the default for the answerer), if the final flight of a server which is not SChannel is lost, the handshake fails after a 30-second timeout with an explicit error. Losses of other handshake flights are handled normally.

### Microsoft Windows with Microsoft Visual C++

```bash
$ cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
$ cd build
$ nmake
```

## Build directly with Make (Linux only)

Options `USE_GNUTLS` and `USE_MBEDTLS` allow to switch the cryptographic backend to GnuTLS and Mbed TLS respectively, otherwise OpenSSL is selected by default. The option `USE_NICE` allows to switch between libjuice as submodule (default) and libnice as system library.

If you only need Data Channels, the option `NO_MEDIA` removes media support. Similarly, `NO_WEBSOCKET` removes WebSocket support.

```bash
$ make USE_GNUTLS=0 USE_NICE=0
```

