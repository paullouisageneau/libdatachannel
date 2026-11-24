/**
 * Copyright (c) 2026 Nemirtingas
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "systemtrust.hpp"
#include "internals.hpp"

#if RTC_ENABLE_WEBSOCKET

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <type_traits>

#if defined(_WIN32)

#include <windows.h>

#include <wincrypt.h> // must be included after windows.h

#elif defined(__APPLE__)

// Prevent AssertMacros.h from defining macros like check() or verify() without underscores
#ifndef __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#endif

namespace rtc::impl::system_trust {

string to_string(Status status) {
	switch (status) {
	case Status::Trusted:
		return "trusted";
	case Status::UntrustedRoot:
		return "untrusted root";
	case Status::HostnameMismatch:
		return "hostname mismatch";
	case Status::Expired:
		return "expired";
	case Status::NotYetValid:
		return "not yet valid";
	case Status::Revoked:
		return "revoked";
	default:
		return "verification error";
	}
}

#if defined(_WIN32)

namespace {

struct CertContextDeleter {
	void operator()(PCCERT_CONTEXT context) const { CertFreeCertificateContext(context); }
};

struct CertStoreDeleter {
	void operator()(HCERTSTORE store) const { CertCloseStore(store, 0); }
};

struct CertChainContextDeleter {
	void operator()(PCCERT_CHAIN_CONTEXT chain) const { CertFreeCertificateChain(chain); }
};

using cert_context_ptr = unique_ptr<const CERT_CONTEXT, CertContextDeleter>;
using cert_store_ptr = unique_ptr<void, CertStoreDeleter>;
using cert_chain_context_ptr = unique_ptr<const CERT_CHAIN_CONTEXT, CertChainContextDeleter>;

const DWORD CertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

string format_error(const string &message, DWORD error) {
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(error));
	return message + " (error " + buffer + ")";
}

std::wstring to_wide_string(const string &str) {
	if (str.empty())
		return {};

	int length = MultiByteToWideChar(CP_UTF8, 0, str.data(), int(str.size()), nullptr, 0);
	if (length <= 0)
		return {};

	std::wstring result(size_t(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, str.data(), int(str.size()), result.data(), length);
	return result;
}

// CERT_E_EXPIRED is reported for any certificate outside of its validity period
bool is_not_yet_valid(PCCERT_CHAIN_CONTEXT chain) {
	for (DWORD i = 0; i < chain->cChain; ++i) {
		PCERT_SIMPLE_CHAIN simple = chain->rgpChain[i];
		for (DWORD j = 0; j < simple->cElement; ++j) {
			PCERT_CHAIN_ELEMENT element = simple->rgpElement[j];
			if ((element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_NOT_TIME_VALID) &&
			    CertVerifyTimeValidity(nullptr, element->pCertContext->pCertInfo) < 0)
				return true;
		}
	}
	return false;
}

} // namespace

Result verify_server_chain(const std::vector<binary> &chain, const string &host) {
	if (chain.empty())
		return {Status::Other, "No server certificate"};

	const auto &leafDer = chain.front();
	cert_context_ptr leaf(CertCreateCertificateContext(
	    CertEncodingType, reinterpret_cast<const BYTE *>(leafDer.data()), DWORD(leafDer.size())));
	if (!leaf)
		return {Status::Other, format_error("Failed to parse server certificate", GetLastError())};

	// In-memory store for the intermediate certificates sent by the server
	cert_store_ptr intermediates(
	    CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr));
	if (!intermediates)
		return {Status::Other, format_error("Failed to create certificate store", GetLastError())};

	for (auto it = chain.begin() + 1; it != chain.end(); ++it) {
		if (!CertAddEncodedCertificateToStore(intermediates.get(), CertEncodingType,
		                                      reinterpret_cast<const BYTE *>(it->data()),
		                                      DWORD(it->size()), CERT_STORE_ADD_ALWAYS, nullptr)) {
			PLOG_DEBUG << "Ignoring an intermediate certificate that failed to parse";
		}
	}

	// Require the chain to be valid for TLS server authentication
	char serverAuthOid[] = szOID_PKIX_KP_SERVER_AUTH;
	LPSTR usages[] = {serverAuthOid};
	CERT_CHAIN_PARA chainPara = {};
	chainPara.cbSize = sizeof(chainPara);
	chainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
	chainPara.RequestedUsage.Usage.cUsageIdentifier = 1;
	chainPara.RequestedUsage.Usage.rgpszUsageIdentifier = usages;

	// The chain engine may block to fetch missing intermediate certificates (AIA)
	PCCERT_CHAIN_CONTEXT chainContext = nullptr;
	if (!CertGetCertificateChain(nullptr, leaf.get(), nullptr, intermediates.get(), &chainPara, 0,
	                             nullptr, &chainContext) ||
	    !chainContext)
		return {Status::Other, format_error("Failed to build certificate chain", GetLastError())};

	cert_chain_context_ptr chainPtr(chainContext);

	// The SSL policy checks the server name against the subjectAltName DNS names and IP addresses
	std::wstring serverName = to_wide_string(host);
	HTTPSPolicyCallbackData httpsPolicy = {};
	httpsPolicy.cbStruct = sizeof(httpsPolicy);
	httpsPolicy.dwAuthType = AUTHTYPE_SERVER;
	httpsPolicy.fdwChecks = 0;
	httpsPolicy.pwszServerName = serverName.data();

	CERT_CHAIN_POLICY_PARA policyPara = {};
	policyPara.cbSize = sizeof(policyPara);
	policyPara.pvExtraPolicyPara = &httpsPolicy;

	CERT_CHAIN_POLICY_STATUS policyStatus = {};
	policyStatus.cbSize = sizeof(policyStatus);

	if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chainPtr.get(), &policyPara,
	                                      &policyStatus))
		return {Status::Other,
		        format_error("Failed to verify certificate chain policy", GetLastError())};

	const DWORD error = policyStatus.dwError;
	switch (static_cast<HRESULT>(error)) {
	case S_OK:
		return {Status::Trusted, "Certificate chain is trusted"};
	case CERT_E_UNTRUSTEDROOT:
	case CERT_E_CHAINING:
	case CERT_E_ISSUERCHAINING:
		return {Status::UntrustedRoot,
		        format_error("Certificate chain is not trusted by the system", error)};
	case CERT_E_CN_NO_MATCH:
		return {Status::HostnameMismatch,
		        format_error("Certificate does not match host \"" + host + "\"", error)};
	case CERT_E_EXPIRED:
		if (is_not_yet_valid(chainPtr.get()))
			return {Status::NotYetValid, format_error("Certificate is not yet valid", error)};
		else
			return {Status::Expired, format_error("Certificate has expired", error)};
	case CRYPT_E_REVOKED:
		return {Status::Revoked, format_error("Certificate has been revoked", error)};
	case CERT_E_WRONG_USAGE:
		return {Status::Other,
		        format_error("Certificate is not valid for server authentication", error)};
	default:
		return {Status::Other, format_error("Certificate chain verification failed", error)};
	}
}

#elif defined(__APPLE__)

namespace {

template <typename T> struct CFReleaser {
	void operator()(T ref) const {
		if (ref)
			CFRelease(ref);
	}
};

template <typename T> using cf_ptr = unique_ptr<std::remove_pointer_t<T>, CFReleaser<T>>;

string to_utf8_string(CFStringRef str) {
	if (!str)
		return {};

	CFIndex length = CFStringGetLength(str);
	CFIndex size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
	string result(size_t(size), '\0');
	if (!CFStringGetCString(str, result.data(), size, kCFStringEncodingUTF8))
		return {};

	result.resize(std::strlen(result.c_str()));
	return result;
}

string describe(CFErrorRef error) {
	if (!error)
		return "Certificate chain verification failed";

	cf_ptr<CFStringRef> description(CFErrorCopyDescription(error));
	string str = to_utf8_string(description.get());
	if (str.empty())
		str = "Certificate chain verification failed";

	return str + " (error " + std::to_string(long(CFErrorGetCode(error))) + ")";
}

} // namespace

Result verify_server_chain(const std::vector<binary> &chain, const string &host) {
	if (chain.empty())
		return {Status::Other, "No server certificate"};

	cf_ptr<CFMutableArrayRef> certificates(
	    CFArrayCreateMutable(kCFAllocatorDefault, CFIndex(chain.size()), &kCFTypeArrayCallBacks));
	if (!certificates)
		return {Status::Other, "Failed to create certificate array"};

	// The server certificate must be the first element, the rest are used as intermediates
	for (size_t i = 0; i < chain.size(); ++i) {
		const auto &der = chain[i];
		cf_ptr<CFDataRef> data(CFDataCreate(
		    kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(der.data()), CFIndex(der.size())));
		cf_ptr<SecCertificateRef> certificate(
		    data ? SecCertificateCreateWithData(kCFAllocatorDefault, data.get()) : nullptr);
		if (!certificate) {
			if (i == 0)
				return {Status::Other, "Failed to parse server certificate"};

			PLOG_DEBUG << "Ignoring an intermediate certificate that failed to parse";
			continue;
		}
		CFArrayAppendValue(certificates.get(), certificate.get());
	}

	cf_ptr<CFStringRef> hostname(
	    CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(host.data()),
		                        CFIndex(host.size()), kCFStringEncodingUTF8, false));
	if (!hostname)
		return {Status::Other, "Invalid host name"};

	// The SSL server policy checks the host name, which may be a DNS name or an IP address
	cf_ptr<SecPolicyRef> policy(SecPolicyCreateSSL(true, hostname.get()));
	if (!policy)
		return {Status::Other, "Failed to create SSL policy"};

	SecTrustRef trustRef = nullptr;
	OSStatus status = SecTrustCreateWithCertificates(certificates.get(), policy.get(), &trustRef);
	cf_ptr<SecTrustRef> trust(trustRef);
	if (status != errSecSuccess || !trust)
		return {Status::Other,
		        "Failed to create trust object (error " + std::to_string(long(status)) + ")"};

	// The evaluation may block to fetch missing intermediate certificates or revocation status
	if (__builtin_available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *)) {
		CFErrorRef errorRef = nullptr;
		bool trusted = SecTrustEvaluateWithError(trust.get(), &errorRef);
		cf_ptr<CFErrorRef> error(errorRef);
		if (trusted)
			return {Status::Trusted, "Certificate chain is trusted"};

		string message = describe(error.get());
		switch (error ? CFErrorGetCode(error.get()) : 0) {
		case errSecNotTrusted:
		case errSecInvalidRoot:
		case errSecCreateChainFailed:
			return {Status::UntrustedRoot, std::move(message)};
		case errSecHostNameMismatch:
			return {Status::HostnameMismatch, std::move(message)};
		case errSecCertificateExpired:
			return {Status::Expired, std::move(message)};
		case errSecCertificateNotValidYet:
			return {Status::NotYetValid, std::move(message)};
		case errSecCertificateRevoked:
			return {Status::Revoked, std::move(message)};
		default:
			return {Status::Other, std::move(message)};
		}
	} else {
		return {Status::Other, "System trust evaluation requires macOS 10.14 or iOS 12"};
	}
}

#else

namespace {

// Append the paths listed in an environment variable
void append_env_paths(std::vector<string> &paths, const char *name, char separator) {
	const char *value = std::getenv(name);
	if (!value)
		return;

	string str(value);
	size_t start = 0;
	while (start <= str.size()) {
		size_t end = str.find(separator, start);
		if (end == string::npos)
			end = str.size();
		if (end > start)
			paths.emplace_back(str.substr(start, end - start));
		start = end + 1;
	}
}

} // namespace

std::vector<string> ca_bundle_files() {
	static const char *const BundleFiles[] = {
	    "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo
	    "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL 6
	    "/etc/ssl/ca-bundle.pem",                            // OpenSUSE
	    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // RHEL 7+
	    "/etc/ssl/cert.pem",                                 // Alpine, OpenBSD
	    "/usr/local/share/certs/ca-root-nss.crt",            // FreeBSD
	};

	std::vector<string> files;
	if (const char *file = std::getenv("SSL_CERT_FILE"); file && *file)
		files.emplace_back(file);

	files.insert(files.end(), std::begin(BundleFiles), std::end(BundleFiles));
	return files;
}

std::vector<string> ca_bundle_directories() {
	static const char *const BundleDirectories[] = {
	    "/system/etc/security/cacerts", // Android
	    "/etc/ssl/certs",
	};

	std::vector<string> directories;
	append_env_paths(directories, "SSL_CERT_DIR", ':');
	directories.insert(directories.end(), std::begin(BundleDirectories),
	                   std::end(BundleDirectories));
	return directories;
}

#endif

} // namespace rtc::impl::system_trust

#endif
