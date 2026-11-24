/**
 * Copyright (c) 2026 Nemirtingas
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// Tests for certificate generation, PEM import, fingerprint algorithms, and TLS server
// certificate verification, which are implemented separately by each TLS backend.

#include "rtc/rtc.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

using namespace rtc;
using namespace std;

namespace {

// Test certificates were generated with OpenSSL and are valid until 2126

// Self-signed ECDSA P-256 certificate, with key in PKCS#8 and SEC1 formats
const char *const ec_cert_pem = R"(-----BEGIN CERTIFICATE-----
MIIBmDCCAT+gAwIBAgIUSGocVCDpFlJPERk9s0kumB3G+FQwCgYIKoZIzj0EAwIw
ITEfMB0GA1UEAwwWbGliZGF0YWNoYW5uZWwgRUMgdGVzdDAgFw0yNjExMjQxMDA5
MDVaGA8yMTI2MTAzMTEwMDkwNVowITEfMB0GA1UEAwwWbGliZGF0YWNoYW5uZWwg
RUMgdGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABF0dUgLeHBXPcyF/lNku
/vFDAygBdM+ggGu/Dq0HMVe3j5f4wLM4YfzBcOgfCvMjmKBB/4+yTqOuLY+gZPSZ
37KjUzBRMB0GA1UdDgQWBBT0h8W1K9jX9WKO2ytlNxhmMWHh/DAfBgNVHSMEGDAW
gBT0h8W1K9jX9WKO2ytlNxhmMWHh/DAPBgNVHRMBAf8EBTADAQH/MAoGCCqGSM49
BAMCA0cAMEQCIBqnAHOFF/QgvJ5dePn44mWcWA0AyZ7YmyIdsbHnbkisAiB1Ldws
9aXW6fL4mkmk5x6WDb/tJFsX1daRQOrn77Zj9g==
-----END CERTIFICATE-----
)";

const char *const ec_key_pkcs8_pem = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgEG6ve3utRk3ySAYL
dUQHcJBzZ5mk0nPFknWZJcqTF7ChRANCAARdHVIC3hwVz3Mhf5TZLv7xQwMoAXTP
oIBrvw6tBzFXt4+X+MCzOGH8wXDoHwrzI5igQf+Psk6jri2PoGT0md+y
-----END PRIVATE KEY-----
)";

const char *const ec_key_sec1_pem = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIBBur3t7rUZN8kgGC3VEB3CQc2eZpNJzxZJ1mSXKkxewoAoGCCqGSM49
AwEHoUQDQgAEXR1SAt4cFc9zIX+U2S7+8UMDKAF0z6CAa78OrQcxV7ePl/jAszhh
/MFw6B8K8yOYoEH/j7JOo64tj6Bk9Jnfsg==
-----END EC PRIVATE KEY-----
)";

// Self-signed RSA 2048 certificate, with key in PKCS#1 and encrypted PKCS#8 formats
const char *const rsa_cert_pem = R"(-----BEGIN CERTIFICATE-----
MIIDJzCCAg+gAwIBAgIUVwmq7N234KjRyrIqKOtXvA3+20owDQYJKoZIhvcNAQEL
BQAwIjEgMB4GA1UEAwwXbGliZGF0YWNoYW5uZWwgUlNBIHRlc3QwIBcNMjYxMTI0
MTAwOTA1WhgPMjEyNjEwMzExMDA5MDVaMCIxIDAeBgNVBAMMF2xpYmRhdGFjaGFu
bmVsIFJTQSB0ZXN0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAspXf
HQmHYr8DGfBvyillk4yUHNrW+rNcnxCwuMMy18RLHaxCiatpDc7bkeXMQ5Pikfaz
cFltHnhY1pX3hU1zPltt/AdtpcRrwiaIs/cDT2PSMaUXLumXR5Yn0+lczyaMjltQ
FPYvjYokMHQqepfyjOWaoqDppoartPlwuCevgDQcJ2x4JhBLe24oEnXpfFumgXoL
cgAST5TQJXlR/FDadf4AKqukOFkXUTzgoXm9zSNHJXWf9/t5XvPiZFEpHPcao8Z+
0H8WD6BNb0dI7clAXOovEdM5931rWoa8HcrCEht68L62Cb3SG14+MhWVI5xTxDeQ
gHEEVp67zK2fwZ+74QIDAQABo1MwUTAdBgNVHQ4EFgQUBoO8DjNsvRP8e/lE5N6/
vh8IWKgwHwYDVR0jBBgwFoAUBoO8DjNsvRP8e/lE5N6/vh8IWKgwDwYDVR0TAQH/
BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEACg4EIWAlgoGKM8yVwt8iFYb5Qwe7
54HRI9fkcEwXCsPwoJ98gXlmE6t1+8iAUWCUZjddnpG5POJSXGDO1DUkqe459QCi
uBx1LQrFqZtD8cuIdnfPYGmMBd5VUlhF03Y59XN6an+tHmilPljXo1CscVZWzHrV
R5eoAtOZk97nlU23LNhqnn81E6b1cLM2Co9PI+63FwGl5iFj5bPyj5ZoIAYrnaTn
L7Q5dRh02hXsyEflB7issUMg4Q4f8rgHnbMS9tnEJhBv0PeDEB7JJet0AX72ey+Z
Ovs4KcI1kJlkhjtojw5Cj2A6gthpayQDHOKw7LODvY7v7I8CJvaQwwFwwQ==
-----END CERTIFICATE-----
)";

const char *const rsa_key_pkcs1_pem = R"(-----BEGIN RSA PRIVATE KEY-----
MIIEowIBAAKCAQEAspXfHQmHYr8DGfBvyillk4yUHNrW+rNcnxCwuMMy18RLHaxC
iatpDc7bkeXMQ5PikfazcFltHnhY1pX3hU1zPltt/AdtpcRrwiaIs/cDT2PSMaUX
LumXR5Yn0+lczyaMjltQFPYvjYokMHQqepfyjOWaoqDppoartPlwuCevgDQcJ2x4
JhBLe24oEnXpfFumgXoLcgAST5TQJXlR/FDadf4AKqukOFkXUTzgoXm9zSNHJXWf
9/t5XvPiZFEpHPcao8Z+0H8WD6BNb0dI7clAXOovEdM5931rWoa8HcrCEht68L62
Cb3SG14+MhWVI5xTxDeQgHEEVp67zK2fwZ+74QIDAQABAoIBABGdNqRmjDPxIx0r
2drWum4TL4ipDQstJbWKr+OJXTpq4f3LP8TUY0g5Xu7E+6CutWowKwKpz1lyBVEk
+YTsXrGH0xFIqkfvV7GOm20kr+7kqZa78c1uvL0S0lrUaZME+lgQKZXnskkDlw4w
ZwAwfHvpKZdgx6rlHYp8D+IaB/VdBCOIIjibrNn/yLkV9uz+STIB1Dk3sJoLYRSN
WqPMrZ89JxRFHZIdUEoXZV3StC3ezXhsSek80hmx8VF93grau8Wk7e0zLO6T+Iuc
Pg4lJJp7GECgVZfRlIhO4xa2q703uu/cXlJ1+D0sMs3a6510iVaKdKyEeD98fenZ
wrJuJQECgYEA5Lvwt2MwXjwaXF+mWDQc3gRrJlr+BGsOC4I+FdCEt1YFL2gH/JvK
KhpO2rInb6AVmnd1W9XUIAW7BRbcTBp5pCmeE82dWgzsw5FfFniG8apNsOPAiAvT
oCe6UuwRU+qiT50unHWlZNIg17KtNJ8MiL97D7z4QFYliBsBwq7yc/cCgYEAx9+X
aATb6JZFv06+dUisKW1p5Uk6sfFb30ETWKPnVBQjp4QOq8LH3HIwlE1q8F7+prDI
aUz56O6bR587N+lt/hDxJduNDlE9ZSGE9nt4D1ikn0zIT2E9lPIHhh72KjK96mRh
6hmg39rOgp1Hr2TfXZb2Wxnr51SQHeX2I495qOcCgYEAjoGlPKxYky2BAvI40hVN
sukv1Mq2HgyQyIzRn4e1WAAI5Ps8lp59OvP/EvQR69LiAsXQYXH8YiMP3ODcSbHo
/WVMbmwZT+Rjkoi8pGRdNq1zD/Ulm906cTqvNECNnym9DDsSsjn93K2p9pkEnpdF
GOGriyehx3ukkIK6+w9CUQsCgYBfXdn6McdTtM/WthW+FEzA/4m1czlkSgtR/NTk
SGBKB8nzVdlCLw/5qSpwuHe79TeABX26cJYVhDPm+pzg0qo+vYqyS42ENVMb++Bb
UxI4EE2MDLDO31Dozoq0ffVqsQdY3Uiy0EOoS9iZDb50e6bwGuW1FisvF3QkL+M/
mGeuJQKBgFqwCJdSmSnFiVW8ogpZv0MY63wWkiIA4g++t/D5OK8NKN2wQlNrEqNo
C4JMfgHSIKOrhLIp5p0NMj0opi901eENCbLmFpJ+fnrwI52j5SO+pvU9Kcvd6JG6
eED/07Sxb9qHXH5ndZfk+VMn9j6qm4eEh/3NoDAzEI7u1A3JfWQw
-----END RSA PRIVATE KEY-----
)";

const char *const rsa_key_encrypted_pem = R"(-----BEGIN ENCRYPTED PRIVATE KEY-----
MIIFNTBfBgkqhkiG9w0BBQ0wUjAxBgkqhkiG9w0BBQwwJAQQvfgoZya2QemC8juz
KF2oNwICCAAwDAYIKoZIhvcNAgkFADAdBglghkgBZQMEASoEEHDfwNH+TMfxRg41
B2r2EGAEggTQg+mVuzGyQdblA3WI/6y+efD2Hd++jSIsxQxO2YDJRrShXU5WP4WY
R+Sl4GS5DtYeChAY0N2a3wbcmIV/wDg5onFzDkzPr8odrvFMz06DE6VZoFgUY2r/
sfe3tS98WK1rS076hLg2HMvaxwoLgvYGsQhyrD49pol7QRWbY0ljB/yubuD5LP9O
NV6ccKPLjA0DBKThlg1ExbPyUkRA5UjedjERy72M/Ep2+ql7meALir2vG4CWLP7L
ghyYkcd+JoqIzdiV0c8CFBkZ4I1raE9G3TrWClPEA3fwSq1wfVN9Arkj560Hz3Y0
vK/qMu7jNUYrvumNhQRoTnMa6kQGldrqaU6HnH5iixrSzsInEEcaVhnSKwdwm7g7
9lERVxRSZxnFPSlYd/Qo8d0ofFzsT54S+t9u+E3p28cX3Mhm8aLzQIQrnzWD1RLl
XlkmxvhN6BCXjVTyvmhlJfUFfU+6FragdZa6KPSYHB+Ka9V8+pRr1IqGDT/nAOsR
XW9PQpX273uNbwsB/NL6GmC68RT4fwi1W0effrh/w+hpHcFXdIUHOo92HGXNB8bL
H2ylxs2VQW8RygSbLDWCKyh8VGsdKfJNvMbphSmQaglbsTc0YYqp63PUlB4KhhtV
ZcJsHmEfb7+u+FXnhLwzxWVvwK7ivJe0iVckhwz832QnzLFivsSffS3z+qdhXQ4n
xybFdeGGtSEu+uMkmAPswL0v75L2kWCvg36tRrwQ1EE0Dxdm4aeNPdvhATQiqnsW
GFGtw2+NE1Yat5mUNy3l5ENfnbltxBqZJgsWCsxDJXcMS28DrnfWGym3UcwmiTNB
QL5N3QDnjk3QvkT+qs1IUyxPl2UJ8eVSjEekhCc4rFQkRIJyt6GiJqEKNfP+hObZ
Mp3m83JC3JHxBMy5v3K7uiQIbHjSIPr0Qj53pazTAY4A6nyrknmGPbRMR5lla8+X
gOyMVloFPA0WYd9reivTeh2uwlusN46+6F1aqqy+PiyuFBrL0HdZvGOj5gQnIbh7
h8/CKRuuCLILEikv6KfusLJi5OQGPgStw59AbLy0aI7+I0/4Hy3up3y9jOcPyIzV
AUEwnsnwP6VJh26AljPmCBnYq1Kid+36NA0cUXQCU1VcpS7IyrUROmng28veUAJo
SAgWX2BBzLoCTOBgMC+/+1TynCsFC6atSEvCk5z5dAZaStQMw/GioDDxzqHA81le
UA61KpQ2XEewA+TlY/a8oaH7VAY3nT/M6tqukQUb5Zw3NBOW84Xy96TFnNOwuWFO
uoVYJtHH2U0ts6oBANoWIvMxQt7MSyQkMQEtzjvZnHCyX8J5TyBefSk+qzIGEFY9
+1TAkadmQVzWpC3coYWBUWW7Taw7eAoZFwEeSxxU14+kaq1buO66oT5nmGd0n4sh
LXEHZ8euiRZndr7YkGf3ItWz1925Um3h7lpZL67g3kM12Z4wJ9OR3oprWorSS3JW
0Bb3l7ENqvg5DR4YIbuRNf2GmAB9Ym9+8tqpeAcq/jweKBWM9uLrpyQZlHH8GJuH
rho7yFWHpofHJgqDHbKpf9CUREOPYL6LSl9F6hxt1s88dFyR+NXrY4XeC4cscrZG
+W4CRcwbbtWwA9id6alLJUS4/6KuazxLNVkir0NMqAmKz69wFHAHxiA=
-----END ENCRYPTED PRIVATE KEY-----
)";

const char *const rsa_key_password = "libdatachannel";

// Test CA, and certificate for localhost signed by the test CA
const char *const ca_cert_pem = R"(-----BEGIN CERTIFICATE-----
MIIBqDCCAU+gAwIBAgIUXI2rJdeKCqq6/EB7XupIr4WZyzcwCgYIKoZIzj0EAwIw
ITEfMB0GA1UEAwwWbGliZGF0YWNoYW5uZWwgdGVzdCBDQTAgFw0yNjExMjQxMDA5
MDVaGA8yMTI2MTAzMTEwMDkwNVowITEfMB0GA1UEAwwWbGliZGF0YWNoYW5uZWwg
dGVzdCBDQTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABPTMjW4uY9ZyZapri7uK
42Ih9KuIRXcuemtYcYzH1qUZaD5ZrQV/mUlXgH0q+Nf8QXByXLidSA19bnOzx2qP
ZTajYzBhMB0GA1UdDgQWBBQfwn6l0nJrI+bLnxqlHun438jlmTAfBgNVHSMEGDAW
gBQfwn6l0nJrI+bLnxqlHun438jlmTAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB
/wQEAwIBBjAKBggqhkjOPQQDAgNHADBEAiA+P6Nsczg0xOLTJeTFchb+4XmMaFwn
rWm/xeXex7sP/gIgKCPMOWzkA3tR1GpgEiey8MdK6rUEWsP1VnzB415D8Tw=
-----END CERTIFICATE-----
)";

const char *const server_cert_pem = R"(-----BEGIN CERTIFICATE-----
MIIBzTCCAXSgAwIBAgIUFH+OYfcnx5ISWCZFBnKEeY9XDHQwCgYIKoZIzj0EAwIw
ITEfMB0GA1UEAwwWbGliZGF0YWNoYW5uZWwgdGVzdCBDQTAgFw0yNjExMjQxMDA5
MDVaGA8yMTI2MTAzMTEwMDkwNVowFDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYH
KoZIzj0CAQYIKoZIzj0DAQcDQgAEgJZE7r+NdltfhavvHjpmu7ndvcg0gAzxYw/d
fsFifoC/1+LyUurOA9LALNv5zolmObe1Yf7LT8y1kvjJSmsBcqOBlDCBkTAsBgNV
HREEJTAjgglsb2NhbGhvc3SHBH8AAAGHEAAAAAAAAAAAAAAAAAAAAAEwEwYDVR0l
BAwwCgYIKwYBBQUHAwEwDAYDVR0TAQH/BAIwADAdBgNVHQ4EFgQUci4Ex9x4ITLU
5tYuRQtsp35i9ZwwHwYDVR0jBBgwFoAUH8J+pdJyayPmy58apR7p+N/I5ZkwCgYI
KoZIzj0EAwIDRwAwRAIgY9E2hsyYJtYvlGk0YEaCKKDLhzpChvWDSAeFmmv8/aEC
IHOqpR2UQZvvT0hwWJtZ5FDQSpjP5oKJe/2i6CvljIa5
-----END CERTIFICATE-----
)";

const char *const server_key_pem = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgAhhnecNITmGaa/21
UJzS//WuK3ocFSNj9jvGjZupnEShRANCAASAlkTuv412W1+Fq+8eOma7ud29yDSA
DPFjD91+wWJ+gL/X4vJS6s4D0sAs2/nOiWY5t7Vh/stPzLWS+MlKawFy
-----END PRIVATE KEY-----
)";

// Another CA which did not sign the server certificate
const char *const other_ca_cert_pem = R"(-----BEGIN CERTIFICATE-----
MIIBpDCCAUugAwIBAgIUXBAuNGyxFlfGIw1CguPBB7CsxcowCgYIKoZIzj0EAwIw
JzElMCMGA1UEAwwcbGliZGF0YWNoYW5uZWwgb3RoZXIgdGVzdCBDQTAgFw0yNjEx
MjQxMDA5MDVaGA8yMTI2MTAzMTEwMDkwNVowJzElMCMGA1UEAwwcbGliZGF0YWNo
YW5uZWwgb3RoZXIgdGVzdCBDQTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABFoN
5UdJkV6suYhLOQ8N2ttdK89pwj5WuajE+ZJZdU9/E+/PuLfHhYsIay0OtYTZnRPF
bZWRegRTCEUNpqinFiujUzBRMB0GA1UdDgQWBBTyyP3yb1qiOH5kMQANZ0dmfEiE
bjAfBgNVHSMEGDAWgBTyyP3yb1qiOH5kMQANZ0dmfEiEbjAPBgNVHRMBAf8EBTAD
AQH/MAoGCCqGSM49BAMCA0cAMEQCIAPph75EYi0ypTaeH6R94mrciYERt4Fyp4z8
oTrdHTOJAiAm9BLX1bxLTCB4CqyD69I06Y5PkLend8izZkSitmrdzg==
-----END CERTIFICATE-----
)";

struct Fingerprint {
	CertificateFingerprint::Algorithm algorithm;
	const char *name;
	const char *value;
};

const Fingerprint ec_fingerprints[] = {
    {CertificateFingerprint::Algorithm::Sha1, "sha-1",
     "BE:5A:B6:2A:E2:72:25:36:9F:F2:77:6B:40:F3:66:AA:2B:36:0B:22"},
    {CertificateFingerprint::Algorithm::Sha224, "sha-224",
     "B8:1C:6A:98:F6:55:00:1F:68:18:50:12:B5:43:77:3E:9B:E5:93:10:FB:71:88:C3:7F:20:74:44"},
    {CertificateFingerprint::Algorithm::Sha256, "sha-256",
     "B9:20:CC:38:52:F3:D5:E4:8E:0E:F8:8C:42:CD:9E:21:49:B7:E5:91:F7:AC:31:58:37:D9:0E:A0:61:A9:9A:"
     "87"},
    {CertificateFingerprint::Algorithm::Sha384, "sha-384",
     "3A:07:34:66:92:D8:7E:27:2F:85:CD:AF:26:2A:D6:3F:C6:60:D5:5D:26:E6:85:CE:6A:78:36:69:C6:D6:83:"
     "0F:AA:43:AF:9A:40:15:92:87:E8:97:6E:82:5F:42:C0:2F"},
    {CertificateFingerprint::Algorithm::Sha512, "sha-512",
     "4A:9A:4D:00:92:0B:8C:A7:99:20:D1:E8:AC:6F:1D:1A:DC:2D:F3:6C:5D:93:1D:A9:F9:2A:E8:A7:CC:C6:2C:"
     "52:E3:C5:32:0E:A3:25:F4:62:81:A4:70:A6:A0:AE:E5:9E:02:A5:CA:D1:63:ED:A7:1A:75:EE:CC:FD:D2:1F:"
     "8C:84"},
};

const Fingerprint rsa_fingerprints[] = {
    {CertificateFingerprint::Algorithm::Sha1, "sha-1",
     "10:DC:EC:F5:38:BB:28:87:9E:0F:2B:D4:57:1D:A1:65:80:3A:E2:D7"},
    {CertificateFingerprint::Algorithm::Sha224, "sha-224",
     "6F:72:98:E6:4F:45:F1:26:86:40:51:0E:57:3E:4B:A8:9A:03:58:DF:59:DD:2F:1C:B7:38:F2:2D"},
    {CertificateFingerprint::Algorithm::Sha256, "sha-256",
     "60:62:1E:CF:E5:BA:95:9C:87:70:5D:D3:6A:32:E7:C4:53:2D:9F:5F:9C:24:7C:90:23:0E:EE:DC:E7:A4:A0:"
     "B1"},
};

const char *const ec_sha256_fingerprint = "B9:20:CC:38:52:F3:D5:E4:8E:0E:F8:8C:42:CD:9E:21:49:B7:"
                                          "E5:91:F7:AC:31:58:37:D9:0E:A0:61:A9:9A:87";
const char *const rsa_sha256_fingerprint = "60:62:1E:CF:E5:BA:95:9C:87:70:5D:D3:6A:32:E7:C4:53:2D:"
                                           "9F:5F:9C:24:7C:90:23:0E:EE:DC:E7:A4:A0:B1";

bool isValidFingerprint(const CertificateFingerprint &f) {
	if (f.algorithm != CertificateFingerprint::Algorithm::Sha256)
		return false;

	const string &value = f.value;
	if (value.size() != 32 * 3 - 1)
		return false;

	for (size_t i = 0; i < value.size(); ++i) {
		char c = value[i];
		if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
			return false;
	}
	return true;
}

// Connect two peers and send a message over a DataChannel, mangle allows to modify the
// description of the first peer before it is signaled
bool connectPeers(const Configuration &config1, const Configuration &config2,
                  function<void(Description &)> mangle = nullptr,
                  optional<CertificateFingerprint> *localFingerprint1 = nullptr) {
	atomic<bool> received = false;
	optional<CertificateFingerprint> fingerprint1;
	mutex fingerprintMutex;

	{
		PeerConnection pc1(config1);
		PeerConnection pc2(config2);

		pc1.onLocalDescription([&](Description sdp) {
			{
				lock_guard lock(fingerprintMutex);
				fingerprint1 = sdp.fingerprint();
			}
			if (mangle)
				mangle(sdp);
			pc2.setRemoteDescription(string(sdp));
		});
		pc1.onLocalCandidate(
		    [&pc2](Candidate candidate) { pc2.addRemoteCandidate(string(candidate)); });
		pc2.onLocalDescription([&pc1](Description sdp) { pc1.setRemoteDescription(string(sdp)); });
		pc2.onLocalCandidate(
		    [&pc1](Candidate candidate) { pc1.addRemoteCandidate(string(candidate)); });

		shared_ptr<DataChannel> dc2;
		pc2.onDataChannel([&](shared_ptr<DataChannel> dc) {
			dc->onMessage([&received](message_variant message) {
				if (holds_alternative<string>(message) && get<string>(message) == "Hello")
					received = true;
			});
			std::atomic_store(&dc2, dc);
		});

		auto dc1 = pc1.createDataChannel("test");
		dc1->onOpen([wdc1 = weak_ptr<DataChannel>(dc1)]() {
			if (auto dc = wdc1.lock())
				dc->send("Hello");
		});

		int attempts = 100;
		while (!received && attempts--)
			this_thread::sleep_for(100ms);

		pc1.close();
		pc2.close();
		this_thread::sleep_for(100ms);
	}

	if (localFingerprint1) {
		lock_guard lock(fingerprintMutex);
		*localFingerprint1 = fingerprint1;
	}
	return received;
}

string writeTempFile(const string &name, const string &content) {
	auto path = filesystem::temp_directory_path() / name;
	ofstream ofs(path, ios::binary | ios::trunc);
	ofs << content;
	return path.string();
}

} // namespace

TestResult test_certificate_types() {
	InitLogger(LogLevel::Warning);

	const CertificateType types[] = {CertificateType::Default, CertificateType::Ecdsa,
	                                 CertificateType::Rsa};
	for (auto type : types) {
		Configuration config;
		config.certificateType = type;
		optional<CertificateFingerprint> fingerprint;
		if (!connectPeers(config, config, nullptr, &fingerprint))
			return TestResult(false, "Connection failed with certificate type " +
			                             to_string(static_cast<int>(type)));

		if (!fingerprint || !isValidFingerprint(*fingerprint))
			return TestResult(false, "Invalid fingerprint with certificate type " +
			                             to_string(static_cast<int>(type)));

		cout << "Certificate type " << static_cast<int>(type) << ": " << fingerprint->value << endl;
	}

	// Mixed certificate types
	Configuration rsaConfig;
	rsaConfig.certificateType = CertificateType::Rsa;
	Configuration ecdsaConfig;
	ecdsaConfig.certificateType = CertificateType::Ecdsa;
	if (!connectPeers(rsaConfig, ecdsaConfig) || !connectPeers(ecdsaConfig, rsaConfig))
		return TestResult(false, "Connection failed with mixed certificate types");

	return TestResult(true);
}

TestResult test_pem_formats() {
	InitLogger(LogLevel::Warning);

	struct PemCase {
		const char *name;
		string certificate;
		string key;
		optional<string> pass;
		const char *fingerprint;
	};

	const string rsaCertFile = writeTempFile("libdatachannel-test-rsa.pem", rsa_cert_pem);
	const string rsaKeyFile =
	    writeTempFile("libdatachannel-test-rsa-encrypted.key", rsa_key_encrypted_pem);
	const string ecCertFile = writeTempFile("libdatachannel-test-ec.pem", ec_cert_pem);
	const string ecKeyFile = writeTempFile("libdatachannel-test-ec-sec1.key", ec_key_sec1_pem);

	const PemCase cases[] = {
	    {"ECDSA with PKCS#8 key", ec_cert_pem, ec_key_pkcs8_pem, nullopt, ec_sha256_fingerprint},
	    {"ECDSA with SEC1 key", ec_cert_pem, ec_key_sec1_pem, nullopt, ec_sha256_fingerprint},
	    {"ECDSA with SEC1 key from files", ecCertFile, ecKeyFile, nullopt, ec_sha256_fingerprint},
	    {"RSA with PKCS#1 key", rsa_cert_pem, rsa_key_pkcs1_pem, nullopt, rsa_sha256_fingerprint},
	    {"RSA with encrypted PKCS#8 key from files", rsaCertFile, rsaKeyFile,
	     string(rsa_key_password), rsa_sha256_fingerprint},
	};

	optional<TestResult> failure;
	for (const auto &c : cases) {
		Configuration config1;
		config1.certificatePemFile = c.certificate;
		config1.keyPemFile = c.key;
		config1.keyPemPass = c.pass;

		optional<CertificateFingerprint> fingerprint;
		bool connected = false;
		try {
			connected = connectPeers(config1, Configuration(), nullptr, &fingerprint);
		} catch (const exception &e) {
			failure = TestResult(false, string(c.name) + ": " + e.what());
			break;
		}

		if (!fingerprint || fingerprint->value != c.fingerprint) {
			failure = TestResult(false, string(c.name) + ": unexpected fingerprint");
			break;
		}

		if (!connected) {
			failure = TestResult(false, string(c.name) + ": connection failed");
			break;
		}

		cout << c.name << ": OK" << endl;
	}

	for (const auto &file : {rsaCertFile, rsaKeyFile, ecCertFile, ecKeyFile})
		std::remove(file.c_str());

	return failure.value_or(TestResult(true));
}

TestResult test_fingerprint_algorithms() {
	InitLogger(LogLevel::Warning);

	struct CertCase {
		const char *certificate;
		const char *key;
		const Fingerprint *begin;
		const Fingerprint *end;
	};

	const CertCase cases[] = {
	    {ec_cert_pem, ec_key_pkcs8_pem, std::begin(ec_fingerprints), std::end(ec_fingerprints)},
	    {rsa_cert_pem, rsa_key_pkcs1_pem, std::begin(rsa_fingerprints), std::end(rsa_fingerprints)},
	};

	for (const auto &c : cases) {
		Configuration config1;
		config1.certificatePemFile = c.certificate;
		config1.keyPemFile = c.key;

		for (auto f = c.begin; f != c.end; ++f) {
			// Signal the fingerprint with another algorithm, the remote peer must verify it
			auto mangle = [f](Description &sdp) {
				sdp.setFingerprint(CertificateFingerprint{f->algorithm, f->value});
			};

			const string name = f->name;
			if (!connectPeers(config1, Configuration(), mangle))
				return TestResult(false, "Connection failed with fingerprint algorithm " + name);

			cout << "Fingerprint algorithm " << name << ": OK" << endl;
		}
	}

	return TestResult(true);
}

#if RTC_ENABLE_WEBSOCKET

TestResult test_websocketserver_tls_verification() {
	InitLogger(LogLevel::Warning);

	const uint16_t port = 48083;

	WebSocketServer::Configuration serverConfig;
	serverConfig.port = port;
	serverConfig.enableTls = true;
	serverConfig.certificatePemFile = server_cert_pem;
	serverConfig.keyPemFile = server_key_pem;
	WebSocketServer server(std::move(serverConfig));

	vector<shared_ptr<WebSocket>> clients;
	mutex clientsMutex;
	server.onClient([&](shared_ptr<WebSocket> incoming) {
		weak_ptr<WebSocket> wincoming = incoming;
		incoming->onMessage([wincoming](message_variant message) {
			if (auto ws = wincoming.lock())
				ws->send(std::move(message)); // Echo
		});
		lock_guard lock(clientsMutex);
		clients.push_back(std::move(incoming));
	});

	// Return true if the connection opens and the message is echoed
	[[maybe_unused]] auto tryConnect = [port](const string &caCertificate,
	                                          const string &host = "localhost") {
		WebSocket::Configuration config;
		config.caCertificatePemFile = caCertificate;
		WebSocket ws(config);
		atomic<bool> echoed = false, failed = false;
		ws.onOpen([&ws]() { ws.send("Hello"); });
		ws.onMessage([&echoed](message_variant message) {
			if (holds_alternative<string>(message) && get<string>(message) == "Hello")
				echoed = true;
		});
		ws.onError([&failed](string error) {
			cout << "WebSocket error: " << error << endl;
			failed = true;
		});
		ws.onClosed([&failed]() { failed = true; });
		ws.open("wss://" + host + ":" + to_string(port) + "/");

		int attempts = 100;
		while (!echoed && !failed && attempts--)
			this_thread::sleep_for(100ms);

		bool result = echoed;
		ws.close();
		this_thread::sleep_for(200ms);
		return result;
	};

#if USE_GNUTLS
	// The GnuTLS backend does not support specifying the CA certificate
	cout << "Skipping connection with the test CA certificate" << endl;
#else
	if (!tryConnect(ca_cert_pem))
		return TestResult(false, "Connection with the test CA certificate failed");

	cout << "Connection with the test CA certificate: OK" << endl;

#if !defined(_WIN32) || USE_SCHANNEL // On Windows, verification is only supported by SChannel
	if (tryConnect(other_ca_cert_pem))
		return TestResult(false, "Connection succeeded with the wrong CA certificate");

	cout << "Connection with the wrong CA certificate failed as expected" << endl;

	// The certificate is valid for localhost, 127.0.0.1, and ::1
	if (tryConnect(ca_cert_pem, "127.0.0.2"))
		return TestResult(false, "Connection succeeded with a host not matching the certificate");

	cout << "Connection with a host not matching the certificate failed as expected" << endl;
#endif
#endif

	server.stop();
	return TestResult(true);
}

#endif
