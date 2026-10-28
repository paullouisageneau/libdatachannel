/**
 * Copyright (c) 2026 Apple Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#if TEST_OPENSSL
#include "impl/tls.hpp"
#endif

using namespace std;

#if TEST_OPENSSL && OPENSSL_VERSION_NUMBER >= 0x30000000

namespace {

// Key exchange groups of each peer, in preference order. A (D)TLS 1.3 client sends a key share for
// its first group only, so the server answers with a HelloRetryRequest if it lacks that group.
struct Peer {
	const char *name;
	const char *groups;
};

const Peer Chrome = {"Chrome (libwebrtc default)", "X25519:P-256:P-384"};
const Peer Libdatachannel = {"libdatachannel", rtc::openssl::KEY_EXCHANGE_GROUPS};
const Peer LibdatachannelP256 = {"libdatachannel before X25519 (P-256 only)", "P-256"};

struct Case {
	const Peer &client;
	const Peer &server;
	int expectedGroup;   // NID of the negotiated group
	bool helloRetryOn13; // whether (D)TLS 1.3 needs a HelloRetryRequest
};

// Servers follow the client's preference, so the client's first group wins when the server has it
const Case Cases[] = {
    {Chrome, Chrome, NID_X25519, false},
    {Chrome, Libdatachannel, NID_X25519, false},
    {Chrome, LibdatachannelP256, NID_X9_62_prime256v1, true},
    {Libdatachannel, Chrome, NID_X9_62_prime256v1, false},
    {Libdatachannel, Libdatachannel, NID_X9_62_prime256v1, false},
    {Libdatachannel, LibdatachannelP256, NID_X9_62_prime256v1, false},
    {LibdatachannelP256, Chrome, NID_X9_62_prime256v1, false},
    {LibdatachannelP256, Libdatachannel, NID_X9_62_prime256v1, false},
    {LibdatachannelP256, LibdatachannelP256, NID_X9_62_prime256v1, false},
};

// In (D)TLS 1.2, the client's groups also restrict the curve of the server's ECDSA certificate
// (RFC 8422 Section 5.3), so a client without P-384 cannot connect to a server with a P-384 one
struct CertificateCase {
	const Peer &client;
	bool failsOn12; // whether (D)TLS 1.2 fails against a server with a P-384 certificate
};

const CertificateCase CertificateCases[] = {
    {Chrome, false},
    {Libdatachannel, false},
    {LibdatachannelP256, true},
};

struct Protocol {
	const char *name;
	bool datagram;
	int version;
	bool is13;
};

const Protocol Protocols[] = {
    {"TLS 1.2", false, TLS1_2_VERSION, false},
#ifndef OPENSSL_NO_TLS1_3
    {"TLS 1.3", false, TLS1_3_VERSION, true},
#endif
    {"DTLS 1.2", true, DTLS1_2_VERSION, false},
#if defined(DTLS1_3_VERSION) && !defined(OPENSSL_NO_DTLS1_3)
    {"DTLS 1.3", true, DTLS1_3_VERSION, true},
#endif
};

struct Credentials {
	EVP_PKEY *pkey = nullptr;
	X509 *cert = nullptr;

	explicit Credentials(const char *curve) {
		pkey = EVP_EC_gen(curve);
		cert = X509_new();
		if (!pkey || !cert)
			throw runtime_error("Failed to create key or certificate");

		X509_set_version(cert, 2);
		ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
		X509_gmtime_adj(X509_getm_notBefore(cert), 0);
		X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
		X509_set_pubkey(cert, pkey);
		// X509_get_subject_name() returns a const name since OpenSSL 4.0, so build one and set it
		X509_NAME *name = X509_NAME_new();
		X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
		                           reinterpret_cast<const unsigned char *>("test"), -1, -1, 0);
		X509_set_subject_name(cert, name);
		X509_set_issuer_name(cert, name);
		X509_NAME_free(name);
		if (!X509_sign(cert, pkey, EVP_sha256()))
			throw runtime_error("Failed to sign certificate");
	}

	~Credentials() {
		X509_free(cert);
		EVP_PKEY_free(pkey);
	}

	Credentials(const Credentials &) = delete;
	Credentials &operator=(const Credentials &) = delete;
};

struct Endpoint {
	SSL_CTX *ctx = nullptr;
	SSL *ssl = nullptr;
	int serverHellosSent = 0;

	~Endpoint() {
		SSL_free(ssl);
		SSL_CTX_free(ctx);
	}
};

void MessageCallback(int writeP, int, int contentType, const void *buf, size_t len, SSL *,
                     void *arg) {
	if (writeP && contentType == SSL3_RT_HANDSHAKE && len > 0 &&
	    static_cast<const unsigned char *>(buf)[0] == SSL3_MT_SERVER_HELLO)
		++static_cast<Endpoint *>(arg)->serverHellosSent;
}

int AcceptAnyCertificate(int, X509_STORE_CTX *) { return 1; }

void setupEndpoint(Endpoint &endpoint, const Peer &peer, bool isClient, const Protocol &protocol,
                   const Credentials &credentials) {
	endpoint.ctx = SSL_CTX_new(protocol.datagram ? DTLS_method() : TLS_method());
	if (!endpoint.ctx)
		throw runtime_error("SSL_CTX_new failed");

	SSL_CTX_set_options(endpoint.ctx, SSL_OP_NO_TICKET);
	SSL_CTX_set_min_proto_version(endpoint.ctx,
	                              protocol.datagram ? DTLS1_2_VERSION : TLS1_2_VERSION);
	SSL_CTX_set_max_proto_version(endpoint.ctx, protocol.version);
	if (protocol.datagram) {
		SSL_CTX_set_options(endpoint.ctx, SSL_OP_NO_QUERY_MTU);
		SSL_CTX_set_read_ahead(endpoint.ctx, 1);
	}
	if (SSL_CTX_set1_groups_list(endpoint.ctx, peer.groups) != 1)
		throw runtime_error(string("Failed to set groups ") + peer.groups);

	SSL_CTX_set_verify(endpoint.ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
	                   AcceptAnyCertificate);
	SSL_CTX_use_certificate(endpoint.ctx, credentials.cert);
	SSL_CTX_use_PrivateKey(endpoint.ctx, credentials.pkey);
	SSL_CTX_set_msg_callback(endpoint.ctx, MessageCallback);
	SSL_CTX_set_msg_callback_arg(endpoint.ctx, &endpoint);

	endpoint.ssl = SSL_new(endpoint.ctx);
	if (!endpoint.ssl)
		throw runtime_error("SSL_new failed");

	BIO *in = BIO_new(BIO_s_mem());
	BIO *out = BIO_new(BIO_s_mem());
	BIO_set_mem_eof_return(in, BIO_EOF);
	SSL_set_bio(endpoint.ssl, in, out);
	if (protocol.datagram)
		SSL_set_mtu(endpoint.ssl, 1200);

	if (isClient)
		SSL_set_connect_state(endpoint.ssl);
	else
		SSL_set_accept_state(endpoint.ssl);
}

// Moves everything pending from one endpoint to the other
void transfer(Endpoint &from, Endpoint &to) {
	char buffer[4096];
	int len;
	while ((len = BIO_read(SSL_get_wbio(from.ssl), buffer, sizeof(buffer))) > 0)
		BIO_write(SSL_get_rbio(to.ssl), buffer, len);
}

// Returns whether the handshake completed on both sides
bool handshake(Endpoint &client, Endpoint &server) {
	bool clientDone = false, serverDone = false;
	for (int i = 0; i < 16 && !(clientDone && serverDone); ++i) {
		if (!clientDone)
			clientDone = SSL_do_handshake(client.ssl) == 1;
		transfer(client, server);
		if (!serverDone)
			serverDone = SSL_do_handshake(server.ssl) == 1;
		transfer(server, client);
	}
	ERR_clear_error();
	return clientDone && serverDone;
}

void runCase(const Case &c, const Protocol &protocol, const Credentials &credentials) {
	const string label =
	    string(protocol.name) + ", client " + c.client.name + ", server " + c.server.name;

	Endpoint client, server;
	setupEndpoint(client, c.client, true, protocol, credentials);
	setupEndpoint(server, c.server, false, protocol, credentials);
	if (!handshake(client, server))
		throw runtime_error(label + ": handshake did not complete");

	if (SSL_version(client.ssl) != protocol.version)
		throw runtime_error(label + ": negotiated " + SSL_get_version(client.ssl));

	const int group = SSL_get_negotiated_group(client.ssl);
	if (group != c.expectedGroup)
		throw runtime_error(label + ": negotiated group " + OBJ_nid2sn(group) + ", expected " +
		                    OBJ_nid2sn(c.expectedGroup));

	const bool helloRetry = server.serverHellosSent > 1;
	const bool expectedHelloRetry = protocol.is13 && c.helloRetryOn13;
	if (helloRetry != expectedHelloRetry)
		throw runtime_error(label + (helloRetry ? ": unexpected HelloRetryRequest"
		                                        : ": expected a HelloRetryRequest"));

	cout << label << ": " << OBJ_nid2sn(group) << (helloRetry ? " after HelloRetryRequest" : "")
	     << endl;
}

void runCertificateCase(const CertificateCase &c, const Protocol &protocol,
                        const Credentials &clientCredentials,
                        const Credentials &serverCredentials) {
	const string label = string(protocol.name) + ", client " + c.client.name +
	                     ", server libdatachannel with a P-384 certificate";

	Endpoint client, server;
	setupEndpoint(client, c.client, true, protocol, clientCredentials);
	setupEndpoint(server, Libdatachannel, false, protocol, serverCredentials);
	const bool completed = handshake(client, server);
	const bool expectedCompleted = protocol.is13 || !c.failsOn12;
	if (completed != expectedCompleted)
		throw runtime_error(label + (completed ? ": handshake unexpectedly completed"
		                                       : ": handshake did not complete"));

	cout << label << ": " << (completed ? "connected" : "failed as expected") << endl;
}

} // namespace

TestResult test_key_exchange_groups() {
	{
		// libdatachannel falls back to P-256 alone when the provider lacks a group
		SSL_CTX *ctx = SSL_CTX_new(TLS_method());
		const bool available =
		    ctx && SSL_CTX_set1_groups_list(ctx, rtc::openssl::KEY_EXCHANGE_GROUPS) == 1;
		SSL_CTX_free(ctx);
		ERR_clear_error();
		if (!available) {
			cout << "Skipped: OpenSSL lacks some of " << rtc::openssl::KEY_EXCHANGE_GROUPS << endl;
			return TestResult(true);
		}
	}

	try {
		const Credentials p256("P-256"), p384("P-384");
		for (const auto &protocol : Protocols) {
			for (const auto &c : Cases)
				runCase(c, protocol, p256);
			for (const auto &c : CertificateCases)
				runCertificateCase(c, protocol, p256, p384);
		}
		return TestResult(true);

	} catch (const exception &e) {
		return TestResult(false, e.what());
	}
}

#else

TestResult test_key_exchange_groups() {
#if TEST_OPENSSL
	cout << "Skipped: key exchange groups are only configured with OpenSSL 3.0 or later" << endl;
#else
	cout << "Skipped: key exchange groups are only configured with OpenSSL" << endl;
#endif
	return TestResult(true);
}

#endif
