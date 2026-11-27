/*-------------------------------------------------------------------------
 *
 * ic_proxy_tls.c
 *	  TLS for ic-proxy peer (daemon-to-daemon) connections.
 *
 * Owns the process-wide SSL_CTXs the ic_proxy bgworker uses to wrap
 * every peer TCP connection in TLS-1.3, the cert / key material that
 * anchors them, and the per-connection filter (ICProxyTlsConn) that
 * ic_proxy_peer.c drives: ciphertext in, plaintext out and back.
 *
 *
 * Copyright (c) 2026-Present.
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef ENABLE_IC_PROXY

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "cdb/cdbvars.h"
#include "common/string.h"		/* pg_clean_ascii */
#include "storage/fd.h"			/* AllocateFile / FreeFile */
#include "utils/guc.h"

#include "ic_proxy.h"
#include "ic_proxy_tls.h"


/*
 * What the handshakes of this process are made from: one SSL_CTX per role
 * and, when no certificate is configured, the ephemeral identity they
 * present.  A connection keeps a reference on the SSL_CTX it was created
 * from, so a set can be replaced while connections made from it live on.
 */
typedef struct ICProxyTlsSettings
{
	SSL_CTX	   *server_ctx;
	SSL_CTX	   *client_ctx;
	EVP_PKEY   *self_key;		/* ephemeral identity, else NULL */
	X509	   *self_cert;
} ICProxyTlsSettings;

/* the set in use, built by ic_proxy_tls_init or ic_proxy_tls_reload */
static ICProxyTlsSettings ic_proxy_tls_cur;

/*
 * The set under construction.  Static, so that what an ERROR leaves behind
 * half-built is found and freed by the next attempt.
 */
static ICProxyTlsSettings ic_proxy_tls_new;

static bool			ic_proxy_tls_enabled_g = false;

/* set by the passphrase callback, see ic_proxy_tls_passwd_cb */
static bool			ic_proxy_tls_passphrase_asked = false;

/*
 * Per-peer TLS state, see the memory-BIO section below for how the peer
 * code drives it.
 */
struct ICProxyTlsConn
{
	SSL		   *ssl;
	BIO		   *rbio;			/* ciphertext in: fed by the peer code, read by SSL */
	BIO		   *wbio;			/* ciphertext out: written by SSL, drained by the peer code */
	const char *name;			/* the peer's name, for logging */
	bool		handshake_done;
	bool		is_server;
	bool		fatal;			/* a fatal SSL error was reported; nothing
								 * may be called on the SSL any more */
	bool		shutdown_sent;	/* ic_proxy_tls_conn_shutdown ran */
	bool		remote_closed;	/* the socket hit EOF or an error */
	bool		got_plaintext;	/* SSL_read returned data at least once */
	char		remote[80];		/* the remote address, the key the failure
								 * reports are rate limited by */
	uint64		tx_records;		/* records encrypted under the current
								 * sending key, see ic_proxy_tls_conn_write */
	char		failed_subject[256];	/* subject of the peer cert that
										 * failed verification, else "" */
};

/*
 * Rotate the traffic keys after this many records were encrypted under one
 * key.  RFC 8446 section 5.5 puts the safe limit for AES-GCM at 2^24.5
 * full-size records per key and OpenSSL does not rotate on its own; a
 * peer connection lives as long as the cluster and carries every motion
 * between two segments, so it does get there.  Stay well below the limit.
 */
#ifndef IC_PROXY_TLS_REKEY_RECORDS
#define IC_PROXY_TLS_REKEY_RECORDS	(UINT64CONST(1) << 23)
#endif

/* the largest plaintext fragment of a TLS record */
#define IC_PROXY_TLS_RECORD_PLAIN_MAX	16384

/* ALPN advertised by the client and matched by the server. */
static const unsigned char ic_proxy_tls_alpn_wire[] = {
	sizeof(IC_PROXY_TLS_ALPN_STRING) - 1,
	'i', 'c', '-', 'p', 'r', 'o', 'x', 'y', '/', '1',
};

/*
 * Format the top OpenSSL error from the error stack as a
 * NUL-terminated string in a static buffer. Subsequent calls
 * overwrite the buffer — callers must use the string immediately
 * (typically inside an errdetail()).
 */
static const char *
ic_proxy_tls_format_openssl_error(void)
{
	static char buf[256];
	unsigned long err = ERR_peek_last_error();

	if (err == 0)
		snprintf(buf, sizeof(buf), "no OpenSSL error queued");
	else
		ERR_error_string_n(err, buf, sizeof(buf));
	ERR_clear_error();
	return buf;
}

/*
 * Like ic_proxy_tls_format_openssl_error, but the oldest error in the
 * queue instead of the newest.  For a failed handshake or read that is
 * the cause ("wrong version number"), where the newest only says which
 * layer gave up ("record layer failure").
 */
static const char *
ic_proxy_tls_format_openssl_cause(void)
{
	static char buf[256];
	unsigned long err = ERR_peek_error();

	if (err == 0)
		snprintf(buf, sizeof(buf), "no OpenSSL error queued");
	else
		ERR_error_string_n(err, buf, sizeof(buf));
	ERR_clear_error();
	return buf;
}

/*
 * Whether an OpenSSL error is what a TLS endpoint reports when the other
 * end does not speak TLS at all, which here means a proxy that runs with
 * gp_interconnect_proxy_tls_enable off.
 */
static bool
ic_proxy_tls_error_means_not_tls(unsigned long err)
{
	if (ERR_GET_LIB(err) != ERR_LIB_SSL)
		return false;

	switch (ERR_GET_REASON(err))
	{
		case SSL_R_WRONG_VERSION_NUMBER:
		case SSL_R_UNKNOWN_PROTOCOL:
		case SSL_R_UNSUPPORTED_PROTOCOL:
		case SSL_R_HTTP_REQUEST:
		case SSL_R_HTTPS_PROXY_REQUEST:
		case SSL_R_PACKET_LENGTH_TOO_LONG:
		case SSL_R_RECORD_LENGTH_MISMATCH:
		case SSL_R_BAD_RECORD_TYPE:
			return true;
		default:
			return false;
	}
}

#define IC_PROXY_TLS_NOT_TLS_HINT \
	"; the remote does not seem to speak TLS, is " \
	"gp_interconnect_proxy_tls_enable on there?"

/*
 * Rate limit for the reports of connections that fail before they carry
 * any data.  A proxy retries a lost peer every second, so a peer that
 * cannot be talked to (an expired certificate, another CA, TLS off on one
 * side) would otherwise put a WARNING per second and per pair in the log,
 * for as long as it takes someone to notice.  The first failure with a
 * remote address is a WARNING, the following ones within the interval are
 * DEBUG1 and counted, and the count is reported with the next WARNING.
 *
 * The table is small and fixed: it only has to hold the addresses that
 * are failing right now, the slot that was reported longest ago is reused.
 */
#define IC_PROXY_TLS_FAILURE_LOG_INTERVAL	60	/* seconds */
#define IC_PROXY_TLS_FAILURE_SLOTS			64

static struct
{
	char		remote[80];
	time_t		last_logged;	/* 0 for a free slot */
	uint32		suppressed;
}			ic_proxy_tls_failures[IC_PROXY_TLS_FAILURE_SLOTS];

int
ic_proxy_tls_conn_failure_elevel(ICProxyTlsConn *conn)
{
	time_t		now = time(NULL);
	int			victim = 0;
	int			found = -1;
	int			i;

	for (i = 0; i < IC_PROXY_TLS_FAILURE_SLOTS; i++)
	{
		if (ic_proxy_tls_failures[i].last_logged != 0 &&
			strcmp(ic_proxy_tls_failures[i].remote, conn->remote) == 0)
		{
			found = i;
			break;
		}
		if (ic_proxy_tls_failures[i].last_logged <
			ic_proxy_tls_failures[victim].last_logged)
			victim = i;
	}

	if (found < 0)
	{
		found = victim;
		strlcpy(ic_proxy_tls_failures[found].remote, conn->remote,
				sizeof(ic_proxy_tls_failures[found].remote));
		ic_proxy_tls_failures[found].suppressed = 0;
	}
	else if (now - ic_proxy_tls_failures[found].last_logged <
			 IC_PROXY_TLS_FAILURE_LOG_INTERVAL)
	{
		ic_proxy_tls_failures[found].suppressed++;
		return DEBUG1;
	}

	if (ic_proxy_tls_failures[found].suppressed > 0)
		elog(LOG,
			 "ic-proxy TLS: %s: %u more failed connections with %s were not "
			 "logged since the last report",
			 conn->name, ic_proxy_tls_failures[found].suppressed, conn->remote);

	ic_proxy_tls_failures[found].last_logged = now;
	ic_proxy_tls_failures[found].suppressed = 0;

	return WARNING;
}

/*
 * Generate a fresh P-256 ECDSA key for the proxy's TLS identity.
 * Caller owns the returned EVP_PKEY.
 *
 * OpenSSL 3.0 added EVP_EC_gen() as a one-shot helper. On older OpenSSL
 * (notably the 1.1.1 that ships with EL8) we fall back to the classic
 * EC_KEY API and wrap it in an EVP_PKEY. The 1.1.1 path uses functions
 * that 3.0 deprecates — that's fine here because the path is only
 * compiled when 3.0 isn't available.
 */
static EVP_PKEY *
ic_proxy_tls_generate_ephemeral_key(void)
{
	EVP_PKEY   *pkey;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	pkey = EVP_EC_gen("prime256v1");
#else
	{
		EC_KEY	   *ec_key;

		ec_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
		if (ec_key == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("ic-proxy TLS: could not allocate EC_KEY"),
					 errdetail("OpenSSL: %s",
							   ic_proxy_tls_format_openssl_error())));
		if (EC_KEY_generate_key(ec_key) != 1)
		{
			EC_KEY_free(ec_key);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("ic-proxy TLS: could not generate ephemeral EC key"),
					 errdetail("OpenSSL: %s",
							   ic_proxy_tls_format_openssl_error())));
		}
		pkey = EVP_PKEY_new();
		if (pkey == NULL)
		{
			EC_KEY_free(ec_key);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("ic-proxy TLS: could not allocate EVP_PKEY")));
		}
		/* assign1 transfers ownership of ec_key to pkey on success */
		if (EVP_PKEY_assign_EC_KEY(pkey, ec_key) != 1)
		{
			EC_KEY_free(ec_key);
			EVP_PKEY_free(pkey);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("ic-proxy TLS: EVP_PKEY_assign_EC_KEY failed"),
					 errdetail("OpenSSL: %s",
							   ic_proxy_tls_format_openssl_error())));
		}
	}
#endif

	if (pkey == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("ic-proxy TLS: could not generate ephemeral key pair"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
	return pkey;
}

/*
 * Build a self-signed X.509 cert over the given key. One-year validity;
 * the CN encodes the cluster role for log/dump readability only — nothing
 * verifies it because the ephemeral path is by definition unauthenticated.
 */
static X509 *
ic_proxy_tls_generate_self_signed_cert(EVP_PKEY *pkey)
{
	X509	   *cert = X509_new();
	X509_NAME  *name;
	const char *cn = "whpg-ic-proxy-self-signed";

	if (cert == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("ic-proxy TLS: could not allocate X509")));

	if (X509_set_version(cert, 2 /* v3 */) != 1 ||
		ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) != 1 ||
		X509_gmtime_adj(X509_get_notBefore(cert), 0) == NULL ||
		X509_gmtime_adj(X509_get_notAfter(cert),
						60L * 60 * 24 * 365) == NULL ||
		X509_set_pubkey(cert, pkey) != 1)
	{
		X509_free(cert);
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: could not initialize self-signed cert"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
	}

	name = X509_get_subject_name(cert);
	if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
								   (const unsigned char *) cn,
								   -1, -1, 0) != 1 ||
		X509_set_issuer_name(cert, name) != 1)
	{
		X509_free(cert);
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: could not set cert subject"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
	}

	if (X509_sign(cert, pkey, EVP_sha256()) == 0)
	{
		X509_free(cert);
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: could not sign self-signed cert"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
	}

	return cert;
}

/*
 * Refuse a world- or group-accessible private key, as the backend does
 * for ssl_key_file: that is an operator error worth surfacing, not one
 * to silently accept.  Open / parse errors are left to OpenSSL's loader.
 * This is stricter than the backend's rule, which also takes a root-owned
 * key with group read permission.
 */
static void
ic_proxy_tls_check_key_file_permissions(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("ic-proxy TLS: could not access key file \"%s\": %m",
						path)));

	if (st.st_mode & (S_IRWXG | S_IRWXO))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: key file \"%s\" has group or world permissions",
						path),
				 errhint("Run: chmod 0600 \"%s\".", path)));
}

/*
 * OpenSSL's default passphrase callback prompts on the terminal, which a
 * background worker must never do.  Refuse instead, and remember it for
 * the error message.
 */
static int
ic_proxy_tls_passwd_cb(char *buf pg_attribute_unused(),
					   int size pg_attribute_unused(),
					   int rwflag pg_attribute_unused(),
					   void *userdata pg_attribute_unused())
{
	ic_proxy_tls_passphrase_asked = true;
	return 0;
}

/*
 * Bind the operator-supplied identity to an SSL_CTX.  The cert file is
 * loaded as a chain: the leaf first, followed by any intermediate CA
 * certs, so a two-tier PKI verifies on the remote end.
 */
static void
ic_proxy_tls_use_configured_identity(SSL_CTX *ctx)
{
	const char *cert_path = gp_interconnect_proxy_tls_cert_file;
	const char *key_path = gp_interconnect_proxy_tls_key_file;

	if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: could not load certificate file \"%s\"",
						cert_path),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	ic_proxy_tls_passphrase_asked = false;
	SSL_CTX_set_default_passwd_cb(ctx, ic_proxy_tls_passwd_cb);

	if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1)
	{
		if (ic_proxy_tls_passphrase_asked)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("ic-proxy TLS: private key file \"%s\" is protected by a passphrase",
							key_path),
					 errhint("The ic-proxy process cannot ask for a passphrase, "
							 "use a key file without one.")));
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: could not load private key file \"%s\"",
						key_path),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
	}
}

/*
 * Verification callback: the default behaviour (the chain's verify result
 * decides), plus a copy of the subject of the certificate that failed.
 * OpenSSL only attaches the peer certificate to the session once the
 * chain verified, so after a failed handshake SSL_get_peer_certificate
 * has nothing, and the WARNING would name no one.  The subject comes from
 * the remote end and is scrubbed before it can reach the log.
 */
static int
ic_proxy_tls_verify_cb(int ok, X509_STORE_CTX *store)
{
	if (!ok)
	{
		SSL		   *ssl = X509_STORE_CTX_get_ex_data(store,
												   SSL_get_ex_data_X509_STORE_CTX_idx());
		ICProxyTlsConn *conn = ssl ? SSL_get_app_data(ssl) : NULL;
		X509	   *cert = X509_STORE_CTX_get_current_cert(store);

		if (conn != NULL && cert != NULL && conn->failed_subject[0] == '\0')
		{
			if (X509_NAME_oneline(X509_get_subject_name(cert),
								  conn->failed_subject,
								  sizeof(conn->failed_subject)) == NULL)
				snprintf(conn->failed_subject, sizeof(conn->failed_subject),
						 "(unprintable subject)");
			else
				pg_clean_ascii(conn->failed_subject);
		}
	}
	return ok;
}

/*
 * Switch an SSL_CTX into "require + verify peer's cert chain against
 * this CA bundle" mode. Applied to both server and client roles for
 * symmetric mTLS — either side can refuse an unauthenticated peer.
 */
static void
ic_proxy_tls_apply_ca_verify(SSL_CTX *ctx, const char *ca_path)
{
	STACK_OF(X509_NAME) *names;

	if (SSL_CTX_load_verify_locations(ctx, ca_path, NULL) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: could not load CA file \"%s\"",
						ca_path),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	SSL_CTX_set_verify(ctx,
					   SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
					   ic_proxy_tls_verify_cb);

	/*
	 * Tell the SSL_CTX which CAs we'd accept as the issuer of the
	 * peer's cert. The server uses this list to drive its
	 * CertificateRequest; on the client side it is harmless and keeps
	 * both contexts symmetric.
	 */
	names = SSL_load_client_CA_file(ca_path);
	if (names == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: could not load client-CA names from \"%s\"",
						ca_path),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
	SSL_CTX_set_client_CA_list(ctx, names);
}

/*
 * Server-side ALPN selection callback. Accepts the connection only if
 * the peer's advertised list contains IC_PROXY_TLS_ALPN_STRING. Refuses
 * the handshake otherwise — a stray HTTPS / HTTP/3 client that hits
 * the proxy port observes a clean TLS alert and goes away instead of
 * tying up handshake state.
 *
 * OpenSSL only calls this when the ClientHello carries the ALPN
 * extension; a client that sends none is caught after the handshake, by
 * ic_proxy_tls_alpn_negotiated.
 */
static int
ic_proxy_tls_alpn_select_cb(SSL *ssl pg_attribute_unused(),
							const unsigned char **out, unsigned char *outlen,
							const unsigned char *in, unsigned int inlen,
							void *arg pg_attribute_unused())
{
	const unsigned char *want = ic_proxy_tls_alpn_wire + 1;
	size_t			want_len = sizeof(ic_proxy_tls_alpn_wire) - 1;
	const unsigned char *p;
	const unsigned char *end = in + inlen;

	for (p = in; p < end;)
	{
		size_t len = *p++;

		if (p + len > end)
			break;				/* malformed list */
		if (len == want_len && memcmp(p, want, want_len) == 0)
		{
			*out = p;
			*outlen = (unsigned char) len;
			return SSL_TLSEXT_ERR_OK;
		}
		p += len;
	}

	elog(DEBUG1, "ic-proxy TLS: peer's ALPN list does not include \"%s\"",
		 IC_PROXY_TLS_ALPN_STRING);
	return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/*
 * Create the SSL_CTX of one role in *slot.  It is stored there before
 * anything can fail, so that the caller's cleanup finds it.  Common
 * configuration is applied here; the role-specific knobs (ALPN advertise
 * vs select) are layered on by the caller.
 */
static void
ic_proxy_tls_make_ctx(ICProxyTlsSettings *settings, SSL_CTX **slot,
					  bool is_server)
{
	SSL_CTX	   *ctx = SSL_CTX_new(is_server
								  ? TLS_server_method() : TLS_client_method());

	if (ctx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("ic-proxy TLS: could not allocate SSL_CTX"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
	*slot = ctx;

	/*
	 * TLS-1.3 only. Simpler protocol than TLS-1.2 (one round trip,
	 * smaller cipher matrix), and both ends of every connection run the
	 * same code, so there is nothing to negotiate down to.
	 */
	SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
	SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

	/*
	 * Pin the cipher suite so the negotiated AEAD is deterministic across
	 * OpenSSL versions and their default orderings; AES-GCM is hardware
	 * accelerated on every CPU we care about.
	 */
	if (SSL_CTX_set_ciphersuites(ctx, "TLS_AES_256_GCM_SHA384") != 1)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: SSL_CTX_set_ciphersuites failed"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	/*
	 * A peer connection is never resumed: do not send session tickets
	 * after every handshake.
	 */
	if (is_server)
		SSL_CTX_set_num_tickets(ctx, 0);

	/* Bind our identity: the configured chain + key, or the ephemeral pair. */
	if (settings->self_cert == NULL)
		ic_proxy_tls_use_configured_identity(ctx);
	else
	{
		if (SSL_CTX_use_certificate(ctx, settings->self_cert) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("ic-proxy TLS: SSL_CTX_use_certificate failed"),
					 errdetail("OpenSSL: %s",
							   ic_proxy_tls_format_openssl_error())));

		if (SSL_CTX_use_PrivateKey(ctx, settings->self_key) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("ic-proxy TLS: SSL_CTX_use_PrivateKey failed"),
					 errdetail("OpenSSL: %s",
							   ic_proxy_tls_format_openssl_error())));
	}

	if (SSL_CTX_check_private_key(ctx) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: certificate and private key do not match"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
}

static void
ic_proxy_tls_settings_free(ICProxyTlsSettings *settings)
{
	if (settings->server_ctx != NULL)
		SSL_CTX_free(settings->server_ctx);
	if (settings->client_ctx != NULL)
		SSL_CTX_free(settings->client_ctx);
	if (settings->self_cert != NULL)
		X509_free(settings->self_cert);
	if (settings->self_key != NULL)
		EVP_PKEY_free(settings->self_key);
	memset(settings, 0, sizeof(*settings));
}

static bool
ic_proxy_tls_guc_is_set(const char *value)
{
	return value != NULL && value[0] != '\0';
}

/*
 * Build a set of TLS settings from the current values of the cert / key /
 * ca GUCs and the files they name.  Raises an ERROR when they cannot be
 * used; what was built until then stays in *settings for the caller to
 * free.
 */
static void
ic_proxy_tls_settings_build(ICProxyTlsSettings *settings)
{
	bool		have_cert = ic_proxy_tls_guc_is_set(gp_interconnect_proxy_tls_cert_file);
	bool		have_key = ic_proxy_tls_guc_is_set(gp_interconnect_proxy_tls_key_file);
	bool		have_ca = ic_proxy_tls_guc_is_set(gp_interconnect_proxy_tls_ca_file);

	/*
	 * Reject half-configurations up front.  A lone cert or key cannot be
	 * used, and a CA without our own cert would make every handshake
	 * fail: the ephemeral self-signed cert is by definition not issued by
	 * the operator's CA, and each proxy generates a different one.
	 */
	if (have_cert != have_key)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: gp_interconnect_proxy_tls_cert_file and "
						"gp_interconnect_proxy_tls_key_file must be set together"),
				 errhint("Set both to use a configured identity, or neither to "
						 "use an ephemeral self-signed certificate.")));
	if (have_ca && !have_cert)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: gp_interconnect_proxy_tls_ca_file requires "
						"gp_interconnect_proxy_tls_cert_file and "
						"gp_interconnect_proxy_tls_key_file"),
				 errhint("Peer verification needs a certificate issued by that "
						 "CA on every proxy; the ephemeral self-signed "
						 "certificate cannot be verified.")));

	if (have_cert)
	{
		ic_proxy_tls_check_key_file_permissions(gp_interconnect_proxy_tls_key_file);

		/* loaded into each SSL_CTX by ic_proxy_tls_use_configured_identity */
		elog(LOG,
			 "ic-proxy TLS: using configured cert \"%s\" + key \"%s\"",
			 gp_interconnect_proxy_tls_cert_file,
			 gp_interconnect_proxy_tls_key_file);
	}
	else
	{
		settings->self_key = ic_proxy_tls_generate_ephemeral_key();
		settings->self_cert =
			ic_proxy_tls_generate_self_signed_cert(settings->self_key);
		elog(LOG,
			 "ic-proxy TLS: using ephemeral self-signed cert "
			 "(encrypted on the wire but not MITM-safe; set "
			 "gp_interconnect_proxy_tls_cert_file/key_file/ca_file "
			 "to anchor the handshake against a cluster CA)");
	}

	/* Build the two contexts after the cert / key are in hand. */
	ic_proxy_tls_make_ctx(settings, &settings->server_ctx, true /* is_server */);
	ic_proxy_tls_make_ctx(settings, &settings->client_ctx, false /* is_server */);

	/* ALPN: client advertises, server selects. */
	if (SSL_CTX_set_alpn_protos(settings->client_ctx,
								ic_proxy_tls_alpn_wire,
								sizeof(ic_proxy_tls_alpn_wire)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: SSL_CTX_set_alpn_protos failed"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	SSL_CTX_set_alpn_select_cb(settings->server_ctx,
							   ic_proxy_tls_alpn_select_cb, NULL);

	/* Optional mTLS: only when a CA file is configured. */
	if (have_ca)
	{
		ic_proxy_tls_apply_ca_verify(settings->server_ctx,
									 gp_interconnect_proxy_tls_ca_file);
		ic_proxy_tls_apply_ca_verify(settings->client_ctx,
									 gp_interconnect_proxy_tls_ca_file);
		elog(LOG,
			 "ic-proxy TLS: peer cert verification against CA \"%s\" enabled (mTLS)",
			 gp_interconnect_proxy_tls_ca_file);
	}
}

/*
 * Public entry: build the TLS settings.  Called from
 * ic_proxy_server_main() before the libuv loop starts accepting peer
 * connections.
 */
void
ic_proxy_tls_init(void)
{
	if (!gp_interconnect_proxy_tls_enable)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
			   "ic-proxy TLS: disabled (gp_interconnect_proxy_tls_enable=off)");
		return;
	}

	ic_proxy_tls_settings_free(&ic_proxy_tls_new);
	ic_proxy_tls_settings_build(&ic_proxy_tls_new);

	ic_proxy_tls_settings_free(&ic_proxy_tls_cur);
	ic_proxy_tls_cur = ic_proxy_tls_new;
	memset(&ic_proxy_tls_new, 0, sizeof(ic_proxy_tls_new));

	ic_proxy_tls_enabled_g = true;
}

/*
 * The configuration was reloaded: read the certificate, key and CA again,
 * the files may have been replaced even when the settings still name the
 * same paths.  New connections use the new set, established ones are not
 * touched.  When the new set cannot be used the error is reported and the
 * proxy keeps the one it has: unlike at startup there is something that
 * works, and it is better than nothing.
 */
void
ic_proxy_tls_reload(void)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	volatile bool ok = false;

	/* gp_interconnect_proxy_tls_enable itself needs a restart */
	if (!ic_proxy_tls_enabled_g)
		return;

	/* an ephemeral identity and still no files: nothing to read again */
	if (ic_proxy_tls_cur.self_cert != NULL &&
		!ic_proxy_tls_guc_is_set(gp_interconnect_proxy_tls_cert_file) &&
		!ic_proxy_tls_guc_is_set(gp_interconnect_proxy_tls_key_file) &&
		!ic_proxy_tls_guc_is_set(gp_interconnect_proxy_tls_ca_file))
		return;

	PG_TRY();
	{
		ic_proxy_tls_settings_free(&ic_proxy_tls_new);
		ic_proxy_tls_settings_build(&ic_proxy_tls_new);
		ok = true;
	}
	PG_CATCH();
	{
		/* the ERROR left us in ErrorContext, which FlushErrorState resets */
		MemoryContextSwitchTo(oldcontext);
		EmitErrorReport();
		FlushErrorState();
	}
	PG_END_TRY();

	if (!ok)
	{
		ic_proxy_tls_settings_free(&ic_proxy_tls_new);
		elog(LOG,
			 "ic-proxy TLS: the reloaded TLS settings cannot be used, the "
			 "ic-proxy process keeps the previous ones; fix the "
			 "gp_interconnect_proxy_tls_* settings and reload again");
		return;
	}

	ic_proxy_tls_settings_free(&ic_proxy_tls_cur);
	ic_proxy_tls_cur = ic_proxy_tls_new;
	memset(&ic_proxy_tls_new, 0, sizeof(ic_proxy_tls_new));

	elog(LOG, "ic-proxy TLS: TLS settings reloaded, new peer connections use them");
}

void
ic_proxy_tls_uninit(void)
{
	ic_proxy_tls_settings_free(&ic_proxy_tls_new);
	ic_proxy_tls_settings_free(&ic_proxy_tls_cur);
	ic_proxy_tls_enabled_g = false;
}

bool
ic_proxy_tls_is_enabled(void)
{
	return ic_proxy_tls_enabled_g;
}


/*
 * ---------------------------------------------------------------
 * Per-peer TLS state — memory BIO filter.
 * ---------------------------------------------------------------
 *
 * The SSL object reads ciphertext from rbio and writes ciphertext to
 * wbio, both memory BIOs.  The peer code (ic_proxy_peer.c) moves bytes
 * between those BIOs and the socket with libuv's ordinary stream API, so
 * OpenSSL never sees a file descriptor and libuv never sees TLS.  An
 * empty rbio reads as "retry" (WANT_READ), never as EOF: EOF on the
 * socket is handled by the peer code, which simply stops feeding us.
 */


/*
 * The key the failure reports of a connection are rate limited by: the
 * remote address, with the port when we connected to it.  The port of a
 * proxy tells the segments of one host apart; the port an incoming
 * connection comes from is different every time and says nothing.
 */
static void
ic_proxy_tls_format_remote(char *buf, size_t buflen,
						   const struct sockaddr *remote, bool is_server)
{
	char		ip[INET6_ADDRSTRLEN] = "unknown";
	int			port = 0;

	if (remote != NULL && remote->sa_family == AF_INET)
	{
		const struct sockaddr_in *in4 = (const struct sockaddr_in *) remote;

		inet_ntop(AF_INET, &in4->sin_addr, ip, sizeof(ip));
		port = ntohs(in4->sin_port);
	}
	else if (remote != NULL && remote->sa_family == AF_INET6)
	{
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) remote;

		inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
		port = ntohs(in6->sin6_port);
	}

	if (is_server)
		snprintf(buf, buflen, "%s", ip);
	else
		snprintf(buf, buflen, "%s:%d", ip, port);
}

ICProxyTlsConn *
ic_proxy_tls_conn_new(bool is_server, const char *name,
					  const struct sockaddr *remote)
{
	ICProxyTlsConn *conn;
	SSL_CTX	   *ctx;

	if (!ic_proxy_tls_enabled_g)
		return NULL;

	ctx = is_server ? ic_proxy_tls_cur.server_ctx : ic_proxy_tls_cur.client_ctx;
	Assert(ctx != NULL);

	conn = ic_proxy_new(ICProxyTlsConn);
	memset(conn, 0, sizeof(*conn));
	conn->is_server = is_server;
	conn->name = name;
	ic_proxy_tls_format_remote(conn->remote, sizeof(conn->remote),
							   remote, is_server);

	conn->ssl = SSL_new(ctx);
	if (conn->ssl == NULL)
	{
		ic_proxy_free(conn);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("ic-proxy TLS: SSL_new failed"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));
	}

	conn->rbio = BIO_new(BIO_s_mem());
	conn->wbio = BIO_new(BIO_s_mem());
	if (conn->rbio == NULL || conn->wbio == NULL)
	{
		BIO_free(conn->rbio);
		BIO_free(conn->wbio);
		SSL_free(conn->ssl);
		ic_proxy_free(conn);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("ic-proxy TLS: BIO_new failed")));
	}

	/*
	 * An empty input BIO must mean "nothing yet", not EOF, or SSL_read
	 * would report an unexpected EOF between two records.  Socket EOF is
	 * handled by the peer code, which simply stops feeding us.
	 */
	BIO_set_mem_eof_return(conn->rbio, -1);

	/* SSL_set_bio transfers ownership; SSL_free will BIO_free both. */
	SSL_set_bio(conn->ssl, conn->rbio, conn->wbio);

	/* for ic_proxy_tls_verify_cb */
	SSL_set_app_data(conn->ssl, conn);

	if (is_server)
		SSL_set_accept_state(conn->ssl);
	else
		SSL_set_connect_state(conn->ssl);

	return conn;
}

void
ic_proxy_tls_conn_free(ICProxyTlsConn *conn)
{
	if (conn == NULL)
		return;

	if (conn->ssl != NULL)
		SSL_free(conn->ssl);	/* frees rbio and wbio too */
	ic_proxy_free(conn);
}

void
ic_proxy_tls_conn_feed(ICProxyTlsConn *conn, const void *data, int len)
{
	/* a memory BIO takes everything or fails to grow: out of memory */
	if (BIO_write(conn->rbio, data, len) != len)
	{
		conn->fatal = true;
		elog(WARNING, "ic-proxy TLS: %s: could not buffer %d bytes of input",
			 conn->name, len);
	}
}

void
ic_proxy_tls_conn_remote_closed(ICProxyTlsConn *conn)
{
	if (conn != NULL)
		conn->remote_closed = true;
}

void
ic_proxy_tls_conn_shutdown(ICProxyTlsConn *conn)
{
	int			rv;

	if (conn == NULL || conn->shutdown_sent)
		return;
	conn->shutdown_sent = true;

	/*
	 * SSL_shutdown is only defined after a completed handshake and must
	 * not follow a fatal error; and there is nobody to notify once the
	 * socket is gone.  One call queues our close_notify in wbio; we do
	 * not wait for the peer's.
	 */
	if (!conn->handshake_done || conn->fatal || conn->remote_closed)
		return;

	ERR_clear_error();
	rv = SSL_shutdown(conn->ssl);
	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy TLS: %s: SSL_shutdown returned %d", conn->name, rv);
	ERR_clear_error();
}

/*
 * Render the peer cert's subject DN into buf, or "(no peer cert)" if
 * the handshake didn't reach the cert-exchange stage. Used in WARNING
 * messages to point the operator at which side's cert was rejected.
 *
 * The subject bytes come from the remote end's certificate — untrusted
 * input when the handshake failed verification, which is exactly when
 * we log it — so scrub non-printable / non-ASCII bytes before they can
 * reach the server log.
 */
static void
ic_proxy_tls_describe_peer(ICProxyTlsConn *conn, char *buf, size_t buflen)
{
	X509	   *peer = SSL_get_peer_certificate(conn->ssl);

	if (peer == NULL)
	{
		/* not attached to the session: verification failed, or none sent */
		if (conn->failed_subject[0] != '\0')
			snprintf(buf, buflen, "%s", conn->failed_subject);
		else
			snprintf(buf, buflen, "(no peer cert)");
		return;
	}
	if (X509_NAME_oneline(X509_get_subject_name(peer), buf, buflen) == NULL)
		snprintf(buf, buflen, "(unprintable subject)");
	else
		pg_clean_ascii(buf);
	X509_free(peer);
}

/*
 * Whether the completed handshake negotiated our ALPN.  The server's
 * select callback only runs for a client that offers ALPN, and a client
 * is not told when the server ignored its offer, so both ends check the
 * outcome.
 */
static bool
ic_proxy_tls_alpn_negotiated(ICProxyTlsConn *conn)
{
	const unsigned char *selected = NULL;
	unsigned int len = 0;

	SSL_get0_alpn_selected(conn->ssl, &selected, &len);

	return len == sizeof(ic_proxy_tls_alpn_wire) - 1 &&
		memcmp(selected, ic_proxy_tls_alpn_wire + 1, len) == 0;
}

/*
 * A note on the OpenSSL error queue.  It belongs to the thread, not to the
 * connection, and SSL_get_error consults it: a leftover entry makes a
 * plain "need more input" look like a fatal error.  Every SSL I/O call
 * below is therefore preceded by ERR_clear_error, so that what one
 * connection left behind (a failed certificate verification, say) cannot
 * take down the next one that is read.
 */

ICProxyTlsHandshakeResult
ic_proxy_tls_conn_handshake_step(ICProxyTlsConn *conn)
{
	int			rv;
	int			err;

	if (conn->handshake_done)
		return IC_PROXY_TLS_HS_DONE;
	if (conn->fatal)
		return IC_PROXY_TLS_HS_FATAL;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy TLS: %s: handshake step (role=%s)",
		   conn->name, conn->is_server ? "server" : "client");

	ERR_clear_error();
	rv = SSL_do_handshake(conn->ssl);
	if (rv == 1)
	{
		if (!ic_proxy_tls_alpn_negotiated(conn))
		{
			conn->fatal = true;
			elog(ic_proxy_tls_conn_failure_elevel(conn),
				 "ic-proxy TLS: %s: handshake failed: the remote did not "
				 "negotiate ALPN \"%s\", it is not an ic-proxy peer (role=%s)",
				 conn->name, IC_PROXY_TLS_ALPN_STRING,
				 conn->is_server ? "server" : "client");
			return IC_PROXY_TLS_HS_FATAL;
		}

		conn->handshake_done = true;

		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
			   "ic-proxy TLS: %s: handshake complete (role=%s, cipher=%s)",
			   conn->name, conn->is_server ? "server" : "client",
			   SSL_get_cipher_name(conn->ssl));

		if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE)
		{
			char		subj[256];

			ic_proxy_tls_describe_peer(conn, subj, sizeof(subj));
			elog(LOG, "ic-proxy TLS: %s: peer cert subject=%s",
				 conn->name, subj);
		}
		return IC_PROXY_TLS_HS_DONE;
	}

	err = SSL_get_error(conn->ssl, rv);
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
	{
		/*
		 * WANT_WRITE cannot happen with a memory output BIO; either way
		 * the next step comes with the peer's next bytes.
		 */
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: %s: handshake wants more input (role=%s)",
			   conn->name, conn->is_server ? "server" : "client");
		return IC_PROXY_TLS_HS_WANT_READ;
	}

	/*
	 * Fatal. If the peer's certificate failed our CA check, surface that
	 * specifically — it's the most common cause of mTLS deployments
	 * breaking, and "ssl_err=1 followed by an opaque OpenSSL string" is
	 * not actionable. Also include the peer subject so the operator can
	 * tell which segment's cert needs attention.
	 */
	conn->fatal = true;
	{
		long		vresult = SSL_get_verify_result(conn->ssl);
		bool		not_tls = ic_proxy_tls_error_means_not_tls(ERR_peek_error());
		int			elevel = ic_proxy_tls_conn_failure_elevel(conn);
		char		subj[256];

		ic_proxy_tls_describe_peer(conn, subj, sizeof(subj));

		if (vresult != X509_V_OK)
			elog(elevel,
				 "ic-proxy TLS: %s: handshake failed: peer certificate "
				 "verification failed: %s (role=%s, peer subject=%s)",
				 conn->name, X509_verify_cert_error_string(vresult),
				 conn->is_server ? "server" : "client", subj);
		else
			elog(elevel,
				 "ic-proxy TLS: %s: handshake failed (role=%s, peer "
				 "subject=%s, ssl_err=%d): %s%s",
				 conn->name, conn->is_server ? "server" : "client",
				 subj, err, ic_proxy_tls_format_openssl_cause(),
				 not_tls ? IC_PROXY_TLS_NOT_TLS_HINT : "");
		ERR_clear_error();
	}
	return IC_PROXY_TLS_HS_FATAL;
}

bool
ic_proxy_tls_conn_handshake_done(const ICProxyTlsConn *conn)
{
	return conn != NULL && conn->handshake_done;
}

bool
ic_proxy_tls_conn_has_pending(const ICProxyTlsConn *conn)
{
	if (conn == NULL || conn->fatal)
		return false;

	/*
	 * SSL_has_pending only knows about bytes OpenSSL already pulled out of
	 * the BIO; with read-ahead off that is one record at a time, so records
	 * fed while no reader was armed are still sitting in rbio.
	 */
	return SSL_has_pending(conn->ssl) != 0 || BIO_ctrl_pending(conn->rbio) > 0;
}

int
ic_proxy_tls_conn_read(ICProxyTlsConn *conn, void *buf, int len)
{
	int			rv;
	int			err;

	Assert(conn->handshake_done);
	if (conn->fatal || conn->shutdown_sent)
		return -1;

	ERR_clear_error();
	rv = SSL_read(conn->ssl, buf, len);
	if (rv > 0)
	{
		conn->got_plaintext = true;
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: %s: SSL_read got %d bytes", conn->name, rv);
		return rv;
	}

	err = SSL_get_error(conn->ssl, rv);
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
		return 0;
	if (err == SSL_ERROR_ZERO_RETURN)
	{
		/* the peer sent close_notify; our own may still follow */
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
			   "ic-proxy TLS: %s: SSL_read clean shutdown", conn->name);
		return -1;
	}

	/*
	 * A connection that fails before it delivered anything was most likely
	 * refused by the remote: in TLS 1.3 the alert for a certificate the
	 * server does not accept reaches the client after its own handshake
	 * completed.  The proxy retries such a peer every second, hence the
	 * rate limit; a connection that worked and then broke always warns.
	 */
	conn->fatal = true;
	elog(conn->got_plaintext ? WARNING : ic_proxy_tls_conn_failure_elevel(conn),
		 "ic-proxy TLS: %s: SSL_read failed (ssl_err=%d): %s",
		 conn->name, err, ic_proxy_tls_format_openssl_cause());
	return -1;
}

bool
ic_proxy_tls_conn_write(ICProxyTlsConn *conn, const void *buf, int len)
{
	int			rv;

	Assert(conn->handshake_done);
	if (conn->fatal || conn->shutdown_sent)
		return false;

	/*
	 * Time for new traffic keys?  SSL_key_update only schedules the
	 * KeyUpdate, the SSL_write below emits it ahead of the data and
	 * switches our sending key; the remote answers with a KeyUpdate of
	 * its own, which rotates the other direction.  A refusal (a KeyUpdate
	 * is already under way) is retried on the next write.
	 */
	if (conn->tx_records >= IC_PROXY_TLS_REKEY_RECORDS)
	{
		ERR_clear_error();
		if (SSL_key_update(conn->ssl, SSL_KEY_UPDATE_REQUESTED) == 1)
		{
			elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
				   "ic-proxy TLS: %s: rotating the traffic keys after "
				   UINT64_FORMAT " records", conn->name, conn->tx_records);
			conn->tx_records = 0;
		}
		else
			ERR_clear_error();
	}

	/*
	 * Without SSL_MODE_ENABLE_PARTIAL_WRITE this returns only once all of
	 * buf is encrypted, as one or more records; a memory BIO never makes
	 * it stop short.
	 */
	ERR_clear_error();
	rv = SSL_write(conn->ssl, buf, len);
	if (rv != len)
	{
		conn->fatal = true;
		elog(WARNING, "ic-proxy TLS: %s: SSL_write failed (ssl_err=%d): %s",
			 conn->name, SSL_get_error(conn->ssl, rv),
			 ic_proxy_tls_format_openssl_error());
		return false;
	}

	conn->tx_records += (len + IC_PROXY_TLS_RECORD_PLAIN_MAX - 1) /
		IC_PROXY_TLS_RECORD_PLAIN_MAX;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy TLS: %s: SSL_write encrypted %d bytes", conn->name, len);
	return true;
}

int
ic_proxy_tls_conn_output_pending(const ICProxyTlsConn *conn)
{
	return (int) BIO_ctrl_pending(conn->wbio);
}

int
ic_proxy_tls_conn_output_take(ICProxyTlsConn *conn, void *buf, int len)
{
	int			n = BIO_read(conn->wbio, buf, len);

	return n < 0 ? 0 : n;
}

#endif /* ENABLE_IC_PROXY */
