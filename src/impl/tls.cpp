/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "tls.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

#if USE_GNUTLS

namespace rtc::gnutls {

// Return false on non-fatal error
bool check(int ret, const string &message) {
	if (ret < 0) {
		if (!gnutls_error_is_fatal(ret)) {
			return false;
		}
		throw std::runtime_error(message + ": " + gnutls_strerror(ret));
	}
	return true;
}

gnutls_certificate_credentials_t *new_credentials() {
	auto creds = new gnutls_certificate_credentials_t;
	gnutls::check(gnutls_certificate_allocate_credentials(creds));
	return creds;
}

void free_credentials(gnutls_certificate_credentials_t *creds) {
	gnutls_certificate_free_credentials(*creds);
	delete creds;
}

gnutls_x509_crt_t *new_crt() {
	auto crt = new gnutls_x509_crt_t;
	gnutls::check(gnutls_x509_crt_init(crt));
	return crt;
}

void free_crt(gnutls_x509_crt_t *crt) {
	gnutls_x509_crt_deinit(*crt);
	delete crt;
}

gnutls_x509_privkey_t *new_privkey() {
	auto privkey = new gnutls_x509_privkey_t;
	gnutls::check(gnutls_x509_privkey_init(privkey));
	return privkey;
}

void free_privkey(gnutls_x509_privkey_t *privkey) {
	gnutls_x509_privkey_deinit(*privkey);
	delete privkey;
}

gnutls_datum_t make_datum(char *data, size_t size) {
	gnutls_datum_t datum;
	datum.data = reinterpret_cast<unsigned char *>(data);
	datum.size = size;
	return datum;
}

} // namespace rtc::gnutls

#elif USE_MBEDTLS

#include <time.h>

namespace {

// Safe gmtime
int my_gmtime(const time_t *t, struct tm *buf) {
#ifdef _WIN32
	return ::gmtime_s(buf, t) == 0 ? 0 : -1;
#else // POSIX
	return ::gmtime_r(t, buf) != NULL ? 0 : -1;
#endif
}

// Format time_t as UTC
size_t my_strftme(char *buf, size_t size, const char *format, const time_t *t) {
	struct tm g;
	if (my_gmtime(t, &g) != 0)
		return 0;

	return ::strftime(buf, size, format, &g);
}

} // namespace

namespace rtc::mbedtls {

void init() {
	check(psa_crypto_init(), "psa_crypto_init failed.");
}

#if !defined(MBEDTLS_THREADING_C)
std::mutex psa_mutex;

int safe_psa(std::function<int()> func) {
	std::unique_lock<std::mutex> lock(psa_mutex);
	return func();
}

int random_func(void *rng, unsigned char *out, size_t len) {
	std::unique_lock<std::mutex> lock(psa_mutex);
	(void)rng;
	return psa_generate_random(out, len);
}
#else
int safe_psa(std::function<int()> func) {
	return func();
}
int random_func(void *rng, unsigned char *out, size_t len) {
	(void)rng;
	return psa_generate_random(out, len);
}
#endif

// Return false on non-fatal error
bool check(int ret, const string &message) {
	if (ret < 0) {
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
		    ret == MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS || ret == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS ||
		    ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
			return false;

		const size_t bufferSize = 1024;
		char buffer[bufferSize];
		mbedtls_strerror(ret, reinterpret_cast<char *>(buffer), bufferSize);
		throw std::runtime_error(message + ": " + std::string(buffer));
	}
	return true;
}

string format_time(const std::chrono::system_clock::time_point &tp) {
	time_t t = std::chrono::system_clock::to_time_t(tp);
	const size_t bufferSize = 256;
	char buffer[bufferSize];
	if (my_strftme(buffer, bufferSize, "%Y%m%d%H%M%S", &t) == 0)
		throw std::runtime_error("Time conversion failed");

	return {buffer};
};

std::shared_ptr<mbedtls_pk_context> new_pk_context() {
	return std::shared_ptr<mbedtls_pk_context>{[]() {
		                                           auto p = new mbedtls_pk_context;
		                                           mbedtls_pk_init(p);
		                                           return p;
	                                           }(),
	                                           [](mbedtls_pk_context *p) {
		                                           mbedtls_pk_free(p);
		                                           delete p;
	                                           }};
}

std::shared_ptr<mbedtls_x509_crt> new_x509_crt() {
	return std::shared_ptr<mbedtls_x509_crt>{[]() {
		                                         auto p = new mbedtls_x509_crt;
		                                         mbedtls_x509_crt_init(p);
		                                         return p;
	                                         }(),
	                                         [](mbedtls_x509_crt *crt) {
		                                         mbedtls_x509_crt_free(crt);
		                                         delete crt;
	                                         }};
}

} // namespace rtc::mbedtls

#elif USE_SCHANNEL

#include "internals.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace rtc::schannel {

namespace {

// BCrypt does not provide SHA-224, so it is implemented here on top of the SHA-256 compression
// function (FIPS 180-4). It is only used for certificate fingerprints.
class Sha224 {
public:
	binary compute(const uint8_t *data, size_t size) {
		uint32_t h[8] = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
		                 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4};

		const uint64_t bitLength = uint64_t(size) * 8;
		size_t offset = 0;
		while (size - offset >= 64) {
			block(h, data + offset);
			offset += 64;
		}

		uint8_t last[128] = {};
		const size_t remaining = size - offset;
		std::memcpy(last, data + offset, remaining);
		last[remaining] = 0x80;
		const size_t lastSize = remaining < 56 ? 64 : 128;
		for (int i = 0; i < 8; ++i)
			last[lastSize - 1 - i] = uint8_t(bitLength >> (8 * i));

		block(h, last);
		if (lastSize == 128)
			block(h, last + 64);

		binary output(28);
		for (int i = 0; i < 7; ++i)
			for (int j = 0; j < 4; ++j)
				output[i * 4 + j] = byte(h[i] >> (24 - 8 * j));

		return output;
	}

private:
	static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	static void block(uint32_t h[8], const uint8_t *p) {
		static const uint32_t k[64] = {
		    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
		    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
		    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
		    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
		    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
		    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
		    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
		    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
		    0xc67178f2};

		uint32_t w[64];
		for (int i = 0; i < 16; ++i)
			w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 |
			       uint32_t(p[i * 4 + 2]) << 8 | uint32_t(p[i * 4 + 3]);
		for (int i = 16; i < 64; ++i) {
			uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; ++i) {
			uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = hh + s1 + ch + k[i] + w[i];
			uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = s0 + maj;
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}

		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
		h[5] += f;
		h[6] += g;
		h[7] += hh;
	}
};

const wchar_t *const KeyNamePrefix = L"libdatachannel-";

bool is_process_alive(DWORD pid) {
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return GetLastError() != ERROR_INVALID_PARAMETER; // assume alive if access is denied

	DWORD exitCode = 0;
	bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
	CloseHandle(process);
	return alive;
}

// Delete keys left behind by processes which did not exit cleanly
void cleanup_stale_keys() {
	NCRYPT_PROV_HANDLE provider = 0;
	if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
		return;

	const std::wstring prefix(KeyNamePrefix);
	std::vector<std::wstring> stale;
	NCryptKeyName *keyName = nullptr;
	PVOID enumState = nullptr;
	while (NCryptEnumKeys(provider, nullptr, &keyName, &enumState, NCRYPT_SILENT_FLAG) ==
	       ERROR_SUCCESS) {
		std::wstring name(keyName->pszName);
		NCryptFreeBuffer(keyName);
		keyName = nullptr;

		if (name.compare(0, prefix.size(), prefix) != 0)
			continue;

		// Name is prefix + pid + '-' + random
		DWORD pid = wcstoul(name.c_str() + prefix.size(), nullptr, 10);
		if (pid != 0 && pid != GetCurrentProcessId() && !is_process_alive(pid))
			stale.push_back(std::move(name));
	}
	if (enumState)
		NCryptFreeBuffer(enumState);

	for (const auto &name : stale) {
		NCRYPT_KEY_HANDLE key = 0;
		if (NCryptOpenKey(provider, &key, name.c_str(), 0, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS) {
			PLOG_DEBUG << "Deleting stale key from a previous process";
			if (NCryptDeleteKey(key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS)
				NCryptFreeObject(key);
		}
	}

	NCryptFreeObject(provider);
}

shared_ptr<void> open_key_storage_provider() {
	NCRYPT_PROV_HANDLE provider = 0;
	check(NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0),
	      "Unable to open the key storage provider");
	return shared_ptr<void>(reinterpret_cast<void *>(provider), [](void *p) {
		NCryptFreeObject(reinterpret_cast<NCRYPT_PROV_HANDLE>(p));
	});
}

} // namespace

string error_string(long code) {
	char *buffer = nullptr;
	auto format = [&buffer, code](DWORD source, HMODULE module, DWORD language) {
		return FormatMessageA(
		    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | source, module,
		    DWORD(code), language, reinterpret_cast<char *>(&buffer), 0, nullptr);
	};

	const DWORD english = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
	DWORD len = format(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, english);
	if (len == 0) // English might not be available
		len = format(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, 0);

	if (len == 0) { // BCrypt and NCrypt might return NTSTATUS codes
		if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
			len = format(FORMAT_MESSAGE_FROM_HMODULE, ntdll, english);
			if (len == 0)
				len = format(FORMAT_MESSAGE_FROM_HMODULE, ntdll, 0);
		}
	}

	string message;
	if (len > 0 && buffer) {
		message.assign(buffer, len);
		while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
		                            message.back() == ' ' || message.back() == '.'))
			message.pop_back();
	}
	if (buffer)
		LocalFree(buffer);

	std::ostringstream oss;
	oss << (message.empty() ? "Unknown error" : message) << " (0x" << std::hex << std::setfill('0')
	    << std::setw(8) << static_cast<unsigned long>(code) << ")";
	return oss.str();
}

void check(SECURITY_STATUS status, const string &message) {
	if (FAILED(status))
		throw std::runtime_error(message + ": " + error_string(status));
}

std::wstring to_wstring(const string &str) {
	if (str.empty())
		return {};

	int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), int(str.size()), nullptr, 0);
	if (len <= 0)
		throw std::invalid_argument("Invalid UTF-8 string");

	std::wstring result(size_t(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, str.data(), int(str.size()), result.data(), len);
	return result;
}

binary hash(HashAlgorithm algorithm, const void *data, size_t size) {
	if (algorithm == HashAlgorithm::Sha224)
		return Sha224().compute(static_cast<const uint8_t *>(data), size);

	LPCWSTR algId;
	switch (algorithm) {
	case HashAlgorithm::Sha1:
		algId = BCRYPT_SHA1_ALGORITHM;
		break;
	case HashAlgorithm::Sha256:
		algId = BCRYPT_SHA256_ALGORITHM;
		break;
	case HashAlgorithm::Sha384:
		algId = BCRYPT_SHA384_ALGORITHM;
		break;
	case HashAlgorithm::Sha512:
		algId = BCRYPT_SHA512_ALGORITHM;
		break;
	default:
		throw std::invalid_argument("Unknown hash algorithm");
	}

	BCRYPT_ALG_HANDLE alg = nullptr;
	NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, algId, nullptr, 0);
	if (!BCRYPT_SUCCESS(status))
		throw std::runtime_error("BCryptOpenAlgorithmProvider failed: " + error_string(status));

	BCRYPT_HASH_HANDLE h = nullptr;
	binary output;
	try {
		DWORD hashLength = 0, resultSize = 0;
		status = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
		                           sizeof(hashLength), &resultSize, 0);
		if (!BCRYPT_SUCCESS(status))
			throw std::runtime_error("BCryptGetProperty failed: " + error_string(status));

		output.resize(hashLength);
		status = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
		if (!BCRYPT_SUCCESS(status))
			throw std::runtime_error("BCryptCreateHash failed: " + error_string(status));

		status = BCryptHashData(h, static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(size), 0);
		if (!BCRYPT_SUCCESS(status))
			throw std::runtime_error("BCryptHashData failed: " + error_string(status));

		status =
		    BCryptFinishHash(h, reinterpret_cast<PUCHAR>(output.data()), ULONG(output.size()), 0);
		if (!BCRYPT_SUCCESS(status))
			throw std::runtime_error("BCryptFinishHash failed: " + error_string(status));

	} catch (...) {
		if (h)
			BCryptDestroyHash(h);
		BCryptCloseAlgorithmProvider(alg, 0);
		throw;
	}

	BCryptDestroyHash(h);
	BCryptCloseAlgorithmProvider(alg, 0);
	return output;
}

cert_context_ptr make_cert_context_ptr(PCCERT_CONTEXT ctx) {
	if (!ctx)
		return nullptr;

	return cert_context_ptr(ctx, [](PCCERT_CONTEXT c) { CertFreeCertificateContext(c); });
}

std::vector<PemBlock> parse_pem(const string &pem) {
	const string beginTag = "-----BEGIN ";
	const string endTag = "-----END ";
	const string dashes = "-----";

	std::vector<PemBlock> blocks;
	size_t pos = 0;
	while ((pos = pem.find(beginTag, pos)) != string::npos) {
		size_t labelBegin = pos + beginTag.size();
		size_t labelEnd = pem.find(dashes, labelBegin);
		if (labelEnd == string::npos)
			break;

		PemBlock block;
		block.label = pem.substr(labelBegin, labelEnd - labelBegin);
		block.encrypted = false;

		size_t bodyBegin = labelEnd + dashes.size();
		string end = endTag + block.label + dashes;
		size_t bodyEnd = pem.find(end, bodyBegin);
		if (bodyEnd == string::npos)
			throw std::invalid_argument("Invalid PEM: missing end of " + block.label);

		// Remove encapsulated headers (RFC 1421), they contain a colon
		string body;
		std::istringstream lines(pem.substr(bodyBegin, bodyEnd - bodyBegin));
		string line;
		while (std::getline(lines, line)) {
			if (line.find(':') != string::npos) {
				if (line.find("ENCRYPTED") != string::npos)
					block.encrypted = true;
				continue;
			}
			body += line;
		}

		DWORD size = 0;
		if (!CryptStringToBinaryA(body.c_str(), DWORD(body.size()), CRYPT_STRING_BASE64, nullptr,
		                          &size, nullptr, nullptr))
			throw std::invalid_argument("Invalid PEM: malformed base64 in " + block.label);

		block.der.resize(size);
		if (!CryptStringToBinaryA(body.c_str(), DWORD(body.size()), CRYPT_STRING_BASE64,
		                          reinterpret_cast<BYTE *>(block.der.data()), &size, nullptr,
		                          nullptr))
			throw std::invalid_argument("Invalid PEM: malformed base64 in " + block.label);

		block.der.resize(size);
		blocks.push_back(std::move(block));
		pos = bodyEnd + end.size();
	}

	return blocks;
}

string read_file(const string &filename) {
	std::ifstream ifs(filename, std::ifstream::in | std::ifstream::binary);
	if (!ifs.is_open())
		throw std::invalid_argument("Unable to open file: " + filename);

	std::ostringstream oss;
	oss << ifs.rdbuf();
	return oss.str();
}

cert_store_ptr load_pem_certificates(const string &pem, cert_context_ptr *leaf) {
	HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
	if (!store)
		throw std::runtime_error("Unable to create certificate store: " +
		                         error_string(long(GetLastError())));

	cert_store_ptr storePtr(store, [](void *s) { CertCloseStore(s, 0); });

	size_t count = 0;
	for (const auto &block : parse_pem(pem)) {
		if (block.label != "CERTIFICATE" && block.label != "X509 CERTIFICATE" &&
		    block.label != "TRUSTED CERTIFICATE")
			continue;

		PCCERT_CONTEXT ctx = nullptr;
		if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
		                                      reinterpret_cast<const BYTE *>(block.der.data()),
		                                      DWORD(block.der.size()), CERT_STORE_ADD_USE_EXISTING,
		                                      (leaf && count == 0) ? &ctx : nullptr))
			throw std::invalid_argument("Unable to import PEM certificate: " +
			                            error_string(long(GetLastError())));

		if (leaf && count == 0)
			*leaf = make_cert_context_ptr(ctx);

		++count;
	}

	if (count == 0)
		throw std::invalid_argument("No certificate found in PEM data");

	return storePtr;
}

std::wstring PrivateKey::NewName() {
	static std::once_flag cleanupFlag;
	std::call_once(cleanupFlag, []() {
		try {
			cleanup_stale_keys();
		} catch (const std::exception &e) {
			PLOG_WARNING << "Stale key cleanup failed: " << e.what();
		}
	});

	uint8_t random[16];
	if (!BCRYPT_SUCCESS(
	        BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
		throw std::runtime_error("Unable to generate random bytes");

	std::wostringstream oss;
	oss << KeyNamePrefix << GetCurrentProcessId() << L'-' << std::hex << std::setfill(L'0');
	for (auto b : random)
		oss << std::setw(2) << unsigned(b);

	return oss.str();
}

shared_ptr<PrivateKey> PrivateKey::Generate(bool rsa) {
	auto provider = open_key_storage_provider();
	std::wstring name = NewName();

	NCRYPT_KEY_HANDLE key = 0;
	check(NCryptCreatePersistedKey(reinterpret_cast<NCRYPT_PROV_HANDLE>(provider.get()), &key,
	                               rsa ? BCRYPT_RSA_ALGORITHM : BCRYPT_ECDSA_P256_ALGORITHM,
	                               name.c_str(), 0, NCRYPT_OVERWRITE_KEY_FLAG),
	      "Unable to create key");

	try {
		if (rsa) {
			DWORD length = 2048;
			check(NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&length),
			                        sizeof(length), 0),
			      "Unable to set key length");
		}

		check(NCryptFinalizeKey(key, NCRYPT_SILENT_FLAG), "Unable to generate key pair");

	} catch (...) {
		if (NCryptDeleteKey(key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS)
			NCryptFreeObject(key);
		throw;
	}

	return std::make_shared<PrivateKey>(key, std::move(name));
}

shared_ptr<PrivateKey> PrivateKey::ImportPkcs8(const binary &der, const string &pass) {
	auto provider = open_key_storage_provider();
	std::wstring name = NewName();
	std::wstring wpass = to_wstring(pass);

	NCryptBuffer params[2];
	params[0].BufferType = NCRYPTBUFFER_PKCS_KEY_NAME;
	params[0].cbBuffer = ULONG((name.size() + 1) * sizeof(wchar_t));
	params[0].pvBuffer = const_cast<wchar_t *>(name.c_str());
	params[1].BufferType = NCRYPTBUFFER_PKCS_SECRET;
	params[1].cbBuffer = ULONG((wpass.size() + 1) * sizeof(wchar_t));
	params[1].pvBuffer = const_cast<wchar_t *>(wpass.c_str());

	NCryptBufferDesc paramsDesc;
	paramsDesc.ulVersion = NCRYPTBUFFER_VERSION;
	paramsDesc.cBuffers = pass.empty() ? 1 : 2;
	paramsDesc.pBuffers = params;

	NCRYPT_KEY_HANDLE key = 0;
	check(NCryptImportKey(reinterpret_cast<NCRYPT_PROV_HANDLE>(provider.get()), 0,
	                      NCRYPT_PKCS8_PRIVATE_KEY_BLOB, &paramsDesc, &key,
	                      reinterpret_cast<PBYTE>(const_cast<byte *>(der.data())),
	                      DWORD(der.size()), NCRYPT_OVERWRITE_KEY_FLAG | NCRYPT_SILENT_FLAG),
	      pass.empty() ? "Unable to import private key"
	                   : "Unable to import private key (wrong password?)");

	SecureZeroMemory(wpass.data(), wpass.size() * sizeof(wchar_t));
	return std::make_shared<PrivateKey>(key, std::move(name));
}

PrivateKey::PrivateKey(NCRYPT_KEY_HANDLE handle, std::wstring name)
    : mHandle(handle), mName(std::move(name)) {}

PrivateKey::~PrivateKey() {
	// Delete the persisted key, this also frees the handle
	if (NCryptDeleteKey(mHandle, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) {
		PLOG_WARNING << "Unable to delete persisted private key";
		NCryptFreeObject(mHandle);
	}
}

Credentials::~Credentials() {
	if (SecIsValidHandle(&mHandle))
		FreeCredentialsHandle(&mHandle);
}

Context::~Context() {
	if (SecIsValidHandle(&mHandle))
		DeleteSecurityContext(&mHandle);
}

ContextBuffers::ContextBuffers(std::initializer_list<unsigned long> types) {
	for (auto type : types) {
		SecBuffer buffer = {};
		buffer.BufferType = type;
		mBuffers.push_back(buffer);
	}
	mDesc.ulVersion = SECBUFFER_VERSION;
	mDesc.cBuffers = ULONG(mBuffers.size());
	mDesc.pBuffers = mBuffers.data();
}

ContextBuffers::~ContextBuffers() {
	for (auto &buffer : mBuffers)
		if (buffer.pvBuffer)
			FreeContextBuffer(buffer.pvBuffer);
}

const SecBuffer *ContextBuffers::find(unsigned long type) const {
	for (const auto &buffer : mBuffers)
		if (buffer.BufferType == type && buffer.pvBuffer && buffer.cbBuffer > 0)
			return &buffer;

	return nullptr;
}

} // namespace rtc::schannel

#else // OPENSSL

namespace rtc::openssl {

void init() {
	static std::mutex mutex;
	static bool done = false;

	std::lock_guard lock(mutex);
	if (!std::exchange(done, true)) {
		uint64_t ssl_opts = OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS;
#ifdef OPENSSL_INIT_NO_ATEXIT
		ssl_opts |= OPENSSL_INIT_NO_ATEXIT;
#endif
		OPENSSL_init_ssl(ssl_opts, nullptr);
		OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
	}
}

string error_string(unsigned long error) {
	const size_t bufferSize = 256;
	char buffer[bufferSize];
	ERR_error_string_n(error, buffer, bufferSize);
	return {buffer};
}

bool check(int success, const string &message) {
	unsigned long last_error = ERR_peek_last_error();
	ERR_clear_error();

	if (success > 0)
		return true;

	throw std::runtime_error(message + (last_error != 0 ? ": " + error_string(last_error) : ""));
}

namespace {

string want_string(int want) {
	switch (want) {
	case SSL_NOTHING:
		return "nothing";
	case SSL_WRITING:
		return "writing";
	case SSL_READING:
		return "reading";
	case SSL_X509_LOOKUP:
		return "x509_lookup";
#ifdef SSL_RETRY_VERIFY
	case SSL_RETRY_VERIFY:
		return "retry_verify";
#endif
	default:
		return std::to_string(want);
	}
}

} // namespace

// Return false on recoverable error
bool check_error(int err, const string &message, const SSL *ssl) {
	unsigned long last_error = ERR_peek_last_error();
	ERR_clear_error();

	if (err == SSL_ERROR_NONE)
		return true;

	if (err == SSL_ERROR_ZERO_RETURN)
		throw std::runtime_error(message + ": peer closed connection");

	if (err == SSL_ERROR_SYSCALL) {
		// SSL_get_error() returns SSL_ERROR_SYSCALL both for a real I/O failure and as its
		// fall-through: when the SSL object wants I/O but the corresponding BIO's retry flags
		// do not say so, it drops past every branch and lands here with an empty error queue.
		// The transports below feed OpenSSL from memory BIOs, so that fall-through is a likely
		// cause, and a bare "fatal I/O error" does not distinguish it. SSL_want() is the SSL
		// object's own state, so it is meaningful regardless of where any syscall happened.
		string detail = ": fatal I/O error";
		if (ssl)
			detail += " (want=" + want_string(SSL_want(ssl)) + ")";
		throw std::runtime_error(message + detail);
	}

	if (err == SSL_ERROR_SSL)
		throw std::runtime_error(message +
		                         (last_error != 0 ? ": " + error_string(last_error) : ""));

	// SSL_ERROR_WANT_READ and SSL_ERROR_WANT_WRITE end up here
	return false;
}

BIO *BIO_new_from_file(const string &filename) {
	BIO *bio = nullptr;
	try {
		std::ifstream ifs(filename, std::ifstream::in | std::ifstream::binary);
		if (!ifs.is_open())
			return nullptr;

		bio = BIO_new(BIO_s_mem());

		const size_t bufferSize = 4096;
		char buffer[bufferSize];
		while (ifs.good()) {
			ifs.read(buffer, bufferSize);
			BIO_write(bio, buffer, int(ifs.gcount()));
		}
		ifs.close();
		return bio;

	} catch (const std::exception &) {
		BIO_free(bio);
		return nullptr;
	}
}

void SSL_CTX_add_cert_to_store_from_pem(SSL_CTX *ctx, const string &pem) {
	X509_STORE *store = SSL_CTX_get_cert_store(ctx);
	if (!store) {
		throw std::runtime_error("Failed to get certificate store");
	}

	BIO *bio = BIO_new(BIO_s_mem());
	BIO_write(bio, pem.data(), int(pem.size()));
	STACK_OF(X509_INFO) *certs = PEM_X509_INFO_read_bio(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	if (!certs) {
		throw std::runtime_error("Failed to load PEM certificate");
	}

	for (int i = 0; i < sk_X509_INFO_num(certs); i++) {
		X509_INFO *value = sk_X509_INFO_value(certs, i);
		if (value->x509) {
			if (!X509_STORE_add_cert(store, value->x509)) {
				unsigned long error = ERR_get_error();
				if (ERR_GET_REASON(error) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
					sk_X509_INFO_pop_free(certs, X509_INFO_free);
					throw std::runtime_error("Failed to add certificate to store: " +
					                         openssl::error_string(error));
				}
			}
		}
		if (value->crl) {
			if (!X509_STORE_add_crl(store, value->crl)) {
				unsigned long error = ERR_get_error();
				sk_X509_INFO_pop_free(certs, X509_INFO_free);
				throw std::runtime_error("Failed to add CRL to store: " +
				                         openssl::error_string(error));
			}
		}
	}

	sk_X509_INFO_pop_free(certs, X509_INFO_free);
}

} // namespace rtc::openssl

#endif
