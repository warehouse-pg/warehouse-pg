/*-------------------------------------------------------------------------
 *
 * ic_proxy_tls.c
 *	  TLS for ic-proxy peer (daemon-to-daemon) connections.
 *
 * Owns the process-wide SSL_CTXs the ic_proxy bgworker uses to wrap
 * every peer TCP connection in TLS-1.3. The handshake itself runs out
 * of ic_proxy_peer.c — this module only supplies the contexts and the
 * cert / key material that anchor them.
 *
 *
 * Copyright (c) 2026-Present.
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef ENABLE_IC_PROXY

#include <sys/stat.h>

#include <uv.h>					/* UV_READABLE / UV_WRITABLE for SSL_get_error mapping */

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


/* Process-wide context — built by ic_proxy_tls_init. */
static SSL_CTX	   *ic_proxy_tls_server_ctx_g = NULL;
static SSL_CTX	   *ic_proxy_tls_client_ctx_g = NULL;
static bool			ic_proxy_tls_enabled_g = false;

/* Ephemeral identity, owned by this module and freed on uninit. */
static EVP_PKEY	   *ic_proxy_tls_self_key = NULL;
static X509		   *ic_proxy_tls_self_cert = NULL;

/* ALPN advertised by the client and matched by the server. */
static const unsigned char ic_proxy_tls_alpn_wire[] = {
	sizeof(IC_PROXY_TLS_ALPN_STRING) - 1,
	'i', 'c', '-', 'p', 'r', 'o', 'x', 'y', '/', '1',
};

/*
 * Kernel TLS capability snapshot, captured at proxy startup. Drives
 * the SSL_OP_ENABLE_KTLS hint and the operational log line that
 * tells the operator whether to expect crypto on CPU or in NIC.
 *
 * The actual data-plane switch to kTLS (libuv read/write directly on
 * the fd while the kernel handles AEAD) requires a uv_poll_t-based
 * handshake driver and a socket BIO — see the comment near
 * IC_PROXY_TLS_ENABLE_KTLS_HINT below. This commit captures the
 * capability and sets the OpenSSL option; the data-plane refactor
 * is a follow-up.
 */
static bool			ic_proxy_tls_ktls_kernel_supported = false;

/*
 * Probe whether the running kernel exposes the "tls" ULP — i.e.
 * whether setsockopt(TCP_ULP, "tls") would be accepted on a TCP
 * socket. Reads /proc/sys/net/ipv4/tcp_available_ulp once at
 * proxy startup. False indicates either kernel < 4.13, the
 * tls module not loaded (modprobe tls), or a container without
 * the necessary sysctls exposed.
 */
static bool
ic_proxy_tls_detect_ktls_kernel(void)
{
	FILE	   *fp;
	char		buf[256];
	size_t		n;
	bool		found = false;

	fp = AllocateFile("/proc/sys/net/ipv4/tcp_available_ulp", "r");
	if (fp == NULL)
		return false;

	n = fread(buf, 1, sizeof(buf) - 1, fp);
	FreeFile(fp);
	if (n == 0)
		return false;
	buf[n] = '\0';

	/* The file lists space-separated ULP names: "espintcp mptcp tls\n" */
	if (strstr(buf, "tls") != NULL)
		found = true;

	return found;
}


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
 * Load a PEM-encoded X.509 cert from disk. ereport ERROR on any
 * problem so a misconfigured cluster fails fast at proxy bgworker
 * startup, not at the first peer handshake.
 */
static X509 *
ic_proxy_tls_load_cert_from_file(const char *path)
{
	FILE	   *fp;
	X509	   *cert;

	fp = AllocateFile(path, "r");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("ic-proxy TLS: could not open cert file \"%s\": %m",
						path)));

	cert = PEM_read_X509(fp, NULL, NULL, NULL);
	FreeFile(fp);

	if (cert == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: could not parse cert file \"%s\"",
						path),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	return cert;
}

/*
 * Load a PEM-encoded private key. Mirrors libpq's
 * check_ssl_key_file_permissions: refuse a world- or group-readable
 * key — that's an operator error worth surfacing, not silently
 * accepting.
 */
static EVP_PKEY *
ic_proxy_tls_load_key_from_file(const char *path)
{
	FILE	   *fp;
	EVP_PKEY   *pkey;
	struct stat st;

	if (stat(path, &st) == 0)
	{
		if (st.st_mode & (S_IRWXG | S_IRWXO))
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("ic-proxy TLS: key file \"%s\" has group or world permissions",
							path),
					 errhint("Run: chmod 0600 \"%s\".", path)));
	}

	fp = AllocateFile(path, "r");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("ic-proxy TLS: could not open key file \"%s\": %m",
						path)));

	pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
	FreeFile(fp);

	if (pkey == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ic-proxy TLS: could not parse key file \"%s\"", path),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	return pkey;
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
					   NULL /* use default cb (X509_verify result) */);

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
 * Create one of the two process-wide SSL_CTXs. Common configuration is
 * applied once here; role-specific knobs (ALPN advertise vs select)
 * are layered on by the caller.
 */
static SSL_CTX *
ic_proxy_tls_make_ctx(bool is_server)
{
	SSL_CTX	   *ctx = SSL_CTX_new(is_server
								  ? TLS_server_method() : TLS_client_method());

	if (ctx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("ic-proxy TLS: could not allocate SSL_CTX"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	/*
	 * TLS-1.3 only. Simpler protocol than TLS-1.2 (one round trip,
	 * smaller cipher matrix), and the kernel TLS upload path
	 * (see follow-up commit) is only well-supported on 1.3 in
	 * recent Linux. There is zero reason to negotiate down for an
	 * intra-cluster handshake.
	 */
	SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
	SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

	/* Bind our cert + key. */
	if (SSL_CTX_use_certificate(ctx, ic_proxy_tls_self_cert) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: SSL_CTX_use_certificate failed"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	if (SSL_CTX_use_PrivateKey(ctx, ic_proxy_tls_self_key) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: SSL_CTX_use_PrivateKey failed"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	if (SSL_CTX_check_private_key(ctx) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: cert and key do not match"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	/*
	 * Disable session tickets and cached resumption. The proxy holds
	 * each peer connection open for the daemon's lifetime, so session
	 * resumption buys nothing while a stored ticket database adds an
	 * attack surface.
	 */
	SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);

	/*
	 * Opt into OpenSSL's automatic kTLS upload when the kernel
	 * supports it. With our socket BIO + uv_poll_t handshake
	 * driver in place (see ic_proxy_peer.c's TLS section), this
	 * causes OpenSSL to setsockopt(TCP_ULP, "tls") + upload the
	 * AEAD keys at handshake-completion time. Subsequent
	 * SSL_read / SSL_write become wrappers over kernel
	 * sendmsg / recvmsg with the AEAD running in kernel (or in
	 * NIC if it has TLS offload).
	 *
	 * Harmless when the kernel can't honour it — OpenSSL silently
	 * falls back to userspace AEAD over the same socket BIO. The
	 * on-wire TLS 1.3 records and the application-visible API are
	 * identical; only the cost of crypto differs. Per-peer
	 * engagement is logged with "kTLS tx=on/off rx=on/off" at
	 * handshake completion.
	 *
	 * SSL_OP_ENABLE_KTLS is OpenSSL 3.0+; on older OpenSSL we just
	 * skip the hint and stay on userspace AEAD regardless of kernel
	 * support.
	 */
#ifdef SSL_OP_ENABLE_KTLS
	if (ic_proxy_tls_ktls_kernel_supported)
		SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
#endif

	return ctx;
}

/*
 * Public entry: build the two SSL_CTXs and (if needed) the ephemeral
 * cert / key. Called once from ic_proxy_server_main() before the libuv
 * loop starts accepting peer connections.
 */
void
ic_proxy_tls_init(void)
{
	bool		have_full_pki;

	if (!gp_interconnect_proxy_tls_enable)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_TERSE, LOG,
			   "ic-proxy TLS: disabled (gp_interconnect_proxy_tls_enable=off)");
		return;
	}

	/*
	 * Probe kernel kTLS support once. The result drives
	 * SSL_OP_ENABLE_KTLS on each SSL_CTX we build below and is
	 * logged for ops visibility. See ic_proxy_tls_detect_ktls_kernel
	 * for what counts as "supported". On OpenSSL < 3.0 the API for
	 * uploading keys to the kernel isn't compiled in at all, so
	 * skip the probe and report it that way.
	 */
#ifdef SSL_OP_ENABLE_KTLS
	ic_proxy_tls_ktls_kernel_supported = ic_proxy_tls_detect_ktls_kernel();
	elog(LOG,
		 "ic-proxy TLS: kernel kTLS %s",
		 ic_proxy_tls_ktls_kernel_supported
		 ? "supported (SSL_OP_ENABLE_KTLS set; OpenSSL will upload "
		   "AEAD keys to the kernel on handshake completion when "
		   "the negotiated cipher matches the kernel's tls module — "
		   "per-peer engagement is logged with \"kTLS tx=on rx=on\")"
		 : "not detected (OpenSSL stays on userspace AEAD over the "
		   "socket BIO; load the tls module and ensure /proc/sys/net/"
		   "ipv4/tcp_available_ulp lists 'tls' to enable in-kernel AEAD)");
#else
	ic_proxy_tls_ktls_kernel_supported = false;
	elog(LOG,
		 "ic-proxy TLS: kTLS unavailable (built against OpenSSL < 3.0, "
		 "no SSL_OP_ENABLE_KTLS); staying on userspace AEAD over the "
		 "socket BIO");
#endif

	have_full_pki = (gp_interconnect_proxy_tls_cert_file != NULL &&
					 gp_interconnect_proxy_tls_cert_file[0] != '\0' &&
					 gp_interconnect_proxy_tls_key_file != NULL &&
					 gp_interconnect_proxy_tls_key_file[0] != '\0');

	if (have_full_pki)
	{
		ic_proxy_tls_self_cert =
			ic_proxy_tls_load_cert_from_file(gp_interconnect_proxy_tls_cert_file);
		ic_proxy_tls_self_key =
			ic_proxy_tls_load_key_from_file(gp_interconnect_proxy_tls_key_file);
		elog(LOG,
			 "ic-proxy TLS: using configured cert \"%s\" + key \"%s\"",
			 gp_interconnect_proxy_tls_cert_file,
			 gp_interconnect_proxy_tls_key_file);
	}
	else
	{
		ic_proxy_tls_self_key = ic_proxy_tls_generate_ephemeral_key();
		ic_proxy_tls_self_cert =
			ic_proxy_tls_generate_self_signed_cert(ic_proxy_tls_self_key);
		elog(LOG,
			 "ic-proxy TLS: using ephemeral self-signed cert "
			 "(encrypted on the wire but not MITM-safe; set "
			 "gp_interconnect_proxy_tls_cert_file/key_file/ca_file "
			 "to anchor the handshake against a cluster CA)");
	}

	/* Build the two contexts after the cert / key are in hand. */
	ic_proxy_tls_server_ctx_g = ic_proxy_tls_make_ctx(true /* is_server */);
	ic_proxy_tls_client_ctx_g = ic_proxy_tls_make_ctx(false /* is_client */);

	/* ALPN: client advertises, server selects. */
	if (SSL_CTX_set_alpn_protos(ic_proxy_tls_client_ctx_g,
								ic_proxy_tls_alpn_wire,
								sizeof(ic_proxy_tls_alpn_wire)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("ic-proxy TLS: SSL_CTX_set_alpn_protos failed"),
				 errdetail("OpenSSL: %s",
						   ic_proxy_tls_format_openssl_error())));

	SSL_CTX_set_alpn_select_cb(ic_proxy_tls_server_ctx_g,
							   ic_proxy_tls_alpn_select_cb, NULL);

	/* Optional mTLS: only when a CA file is configured. */
	if (gp_interconnect_proxy_tls_ca_file != NULL &&
		gp_interconnect_proxy_tls_ca_file[0] != '\0')
	{
		ic_proxy_tls_apply_ca_verify(ic_proxy_tls_server_ctx_g,
									 gp_interconnect_proxy_tls_ca_file);
		ic_proxy_tls_apply_ca_verify(ic_proxy_tls_client_ctx_g,
									 gp_interconnect_proxy_tls_ca_file);
		elog(LOG,
			 "ic-proxy TLS: peer cert verification against CA \"%s\" enabled (mTLS)",
			 gp_interconnect_proxy_tls_ca_file);
	}

	ic_proxy_tls_enabled_g = true;
}

void
ic_proxy_tls_uninit(void)
{
	if (ic_proxy_tls_server_ctx_g != NULL)
	{
		SSL_CTX_free(ic_proxy_tls_server_ctx_g);
		ic_proxy_tls_server_ctx_g = NULL;
	}
	if (ic_proxy_tls_client_ctx_g != NULL)
	{
		SSL_CTX_free(ic_proxy_tls_client_ctx_g);
		ic_proxy_tls_client_ctx_g = NULL;
	}
	if (ic_proxy_tls_self_cert != NULL)
	{
		X509_free(ic_proxy_tls_self_cert);
		ic_proxy_tls_self_cert = NULL;
	}
	if (ic_proxy_tls_self_key != NULL)
	{
		EVP_PKEY_free(ic_proxy_tls_self_key);
		ic_proxy_tls_self_key = NULL;
	}
	ic_proxy_tls_enabled_g = false;
}

bool
ic_proxy_tls_is_enabled(void)
{
	return ic_proxy_tls_enabled_g;
}

SSL_CTX *
ic_proxy_tls_server_ctx(void)
{
	return ic_proxy_tls_server_ctx_g;
}

SSL_CTX *
ic_proxy_tls_client_ctx(void)
{
	return ic_proxy_tls_client_ctx_g;
}


/*
 * ---------------------------------------------------------------
 * Per-peer TLS state — socket BIO + uv_poll_t bridge.
 * ---------------------------------------------------------------
 *
 * Each peer's fd is detached from uv_tcp_t (which would otherwise
 * own the epoll registration) and watched via a uv_poll_t in the
 * peer code (ic_proxy_peer.c). The SSL object here uses a socket
 * BIO that calls recv/send on the same fd directly.
 *
 * SSL_OP_ENABLE_KTLS on the SSL_CTX (set when the kernel supports
 * the tls ULP — see ic_proxy_tls_init) causes OpenSSL to upload the
 * negotiated AEAD keys to the kernel via setsockopt during
 * handshake completion. After that, SSL_read / SSL_write become
 * wrappers over kernel sendmsg / recvmsg and AEAD runs in kernel
 * (or in NIC if the NIC has TLS offload). When kTLS is unavailable
 * (older kernel, unsupported cipher, NIC without offload),
 * OpenSSL silently falls back to userspace AEAD over the same
 * socket BIO — same wire protocol, only the cost of crypto changes.
 *
 * The libuv bridge: poll-readable → SSL_read; poll-writable + caller
 * has bytes → SSL_write. OpenSSL returns WANT_READ / WANT_WRITE on
 * a non-blocking socket; we translate those into the libuv event
 * mask the caller arms on the uv_poll_t.
 */

struct ICProxyTlsConn
{
	SSL		   *ssl;
	BIO		   *sock_bio;		/* socket BIO over peer->tls_fd */
	int			fd;				/* same fd; tracked for debug log only */
	bool		handshake_done;
	bool		is_server;
	bool		ktls_tx;		/* true after handshake if kTLS uploaded TX keys */
	bool		ktls_rx;		/* same for RX */
};

ICProxyTlsConn *
ic_proxy_tls_conn_new(int fd, bool is_server)
{
	ICProxyTlsConn *conn;
	SSL_CTX	   *ctx;

	if (!ic_proxy_tls_enabled_g)
		return NULL;

	ctx = is_server ? ic_proxy_tls_server_ctx_g : ic_proxy_tls_client_ctx_g;
	Assert(ctx != NULL);

	conn = ic_proxy_new(ICProxyTlsConn);
	memset(conn, 0, sizeof(*conn));
	conn->is_server = is_server;
	conn->fd = fd;

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

	/*
	 * BIO_NOCLOSE — when SSL_free tears the BIO down it must NOT close
	 * the underlying fd. The caller (peer code) owns fd lifetime; we
	 * just borrow it for SSL's I/O.
	 */
	conn->sock_bio = BIO_new_socket(fd, BIO_NOCLOSE);
	if (conn->sock_bio == NULL)
	{
		SSL_free(conn->ssl);
		ic_proxy_free(conn);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("ic-proxy TLS: BIO_new_socket failed")));
	}

	/*
	 * Same BIO for read and write — OpenSSL handles socket
	 * read/write directionality internally. SSL_set_bio transfers
	 * ownership; SSL_free will BIO_free it.
	 */
	SSL_set_bio(conn->ssl, conn->sock_bio, conn->sock_bio);

	/*
	 * Allow partial-write semantics from SSL_write. The data-plane
	 * write path (ic_proxy_peer_tls_drain_tx) coalesces multiple
	 * queued packets into one scratch buffer per SSL_write; with
	 * partial writes enabled, OpenSSL is free to consume any
	 * non-zero amount and report it back — we then apportion the
	 * bytes back across queue items, with no obligation to retry
	 * with the same buffer (the moving-write-buffer mode covers
	 * the WANT_WRITE retry case). Without these the API contract
	 * forces us to call SSL_write per-packet, which puts each
	 * motion packet into its own TLS record and pays the 13-byte
	 * header + 16-byte auth-tag overhead per packet.
	 */
	SSL_set_mode(conn->ssl,
				 SSL_MODE_ENABLE_PARTIAL_WRITE |
				 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

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
	{
		/* SSL_free also frees the socket BIO (BIO_NOCLOSE keeps fd open). */
		SSL_free(conn->ssl);
	}
	ic_proxy_free(conn);
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
		snprintf(buf, buflen, "(no peer cert)");
		return;
	}
	if (X509_NAME_oneline(X509_get_subject_name(peer), buf, buflen) == NULL)
		snprintf(buf, buflen, "(unprintable subject)");
	else
		pg_clean_ascii(buf);
	X509_free(peer);
}

ICProxyTlsHandshakeResult
ic_proxy_tls_conn_handshake_step(ICProxyTlsConn *conn)
{
	int			rv;
	int			err;

	if (conn->handshake_done)
		return IC_PROXY_TLS_HS_DONE;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy TLS: handshake step (role=%s, fd=%d)",
		   conn->is_server ? "server" : "client", conn->fd);

	rv = SSL_do_handshake(conn->ssl);
	if (rv == 1)
	{
		conn->handshake_done = true;
		/*
		 * BIO_get_ktls_send / BIO_get_ktls_recv are OpenSSL 3.0+. On
		 * older OpenSSL the SSL_OP_ENABLE_KTLS hint above is also
		 * absent, so kTLS is never engaged — report both off.
		 */
#ifdef SSL_OP_ENABLE_KTLS
		conn->ktls_tx = BIO_get_ktls_send(SSL_get_wbio(conn->ssl)) != 0;
		conn->ktls_rx = BIO_get_ktls_recv(SSL_get_rbio(conn->ssl)) != 0;
#else
		conn->ktls_tx = false;
		conn->ktls_rx = false;
#endif

		elog(LOG,
			 "ic-proxy TLS: handshake complete (role=%s, cipher=%s, fd=%d, "
			 "kTLS tx=%s rx=%s)",
			 conn->is_server ? "server" : "client",
			 SSL_get_cipher_name(conn->ssl),
			 conn->fd,
			 conn->ktls_tx ? "on" : "off",
			 conn->ktls_rx ? "on" : "off");

		if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE)
		{
			char		subj[256];

			ic_proxy_tls_describe_peer(conn, subj, sizeof(subj));
			elog(LOG, "ic-proxy TLS: peer cert subject=%s (fd=%d)",
				 subj, conn->fd);
		}
		return IC_PROXY_TLS_HS_DONE;
	}

	err = SSL_get_error(conn->ssl, rv);
	if (err == SSL_ERROR_WANT_READ)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: handshake WANT_READ (role=%s, fd=%d)",
			   conn->is_server ? "server" : "client", conn->fd);
		return IC_PROXY_TLS_HS_WANT_READ;
	}
	if (err == SSL_ERROR_WANT_WRITE)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: handshake WANT_WRITE (role=%s, fd=%d)",
			   conn->is_server ? "server" : "client", conn->fd);
		return IC_PROXY_TLS_HS_WANT_WRITE;
	}

	/*
	 * Fatal. If the peer's certificate failed our CA check, surface that
	 * specifically — it's the most common cause of mTLS deployments
	 * breaking, and "ssl_err=1 followed by an opaque OpenSSL string" is
	 * not actionable. Also include the peer subject so the operator can
	 * tell which segment's cert needs attention.
	 */
	{
		long		vresult = SSL_get_verify_result(conn->ssl);
		char		subj[256];

		ic_proxy_tls_describe_peer(conn, subj, sizeof(subj));

		if (vresult != X509_V_OK)
			elog(WARNING,
				 "ic-proxy TLS: handshake failed: peer certificate "
				 "verification failed: %s (role=%s, fd=%d, peer subject=%s)",
				 X509_verify_cert_error_string(vresult),
				 conn->is_server ? "server" : "client",
				 conn->fd, subj);
		else
			elog(WARNING,
				 "ic-proxy TLS: handshake failed (role=%s, fd=%d, peer "
				 "subject=%s, ssl_err=%d): %s",
				 conn->is_server ? "server" : "client",
				 conn->fd, subj, err,
				 ic_proxy_tls_format_openssl_error());
	}
	return IC_PROXY_TLS_HS_FATAL;
}

bool
ic_proxy_tls_conn_handshake_done(const ICProxyTlsConn *conn)
{
	return conn != NULL && conn->handshake_done;
}

bool
ic_proxy_tls_conn_ktls_tx(const ICProxyTlsConn *conn)
{
	return conn != NULL && conn->ktls_tx;
}

bool
ic_proxy_tls_conn_ktls_rx(const ICProxyTlsConn *conn)
{
	return conn != NULL && conn->ktls_rx;
}

int
ic_proxy_tls_conn_pending(const ICProxyTlsConn *conn)
{
	return SSL_pending(conn->ssl);
}

int
ic_proxy_tls_conn_read(ICProxyTlsConn *conn, void *buf, int len, int *out_event)
{
	int			rv;
	int			err;

	Assert(conn->handshake_done);
	*out_event = 0;

	rv = SSL_read(conn->ssl, buf, len);
	if (rv > 0)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: SSL_read got %d bytes (fd=%d)", rv, conn->fd);
		return rv;
	}

	err = SSL_get_error(conn->ssl, rv);
	if (err == SSL_ERROR_WANT_READ)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: SSL_read WANT_READ (fd=%d)", conn->fd);
		*out_event = UV_READABLE;
		return 0;
	}
	if (err == SSL_ERROR_WANT_WRITE)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: SSL_read WANT_WRITE (fd=%d)", conn->fd);
		*out_event = UV_WRITABLE;
		return 0;
	}
	if (err == SSL_ERROR_ZERO_RETURN)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
			   "ic-proxy TLS: SSL_read clean shutdown (fd=%d)", conn->fd);
		return -1;					/* clean shutdown */
	}
	elog(WARNING, "ic-proxy TLS: SSL_read failed (fd=%d, ssl_err=%d): %s",
		 conn->fd, err, ic_proxy_tls_format_openssl_error());
	return -1;
}

int
ic_proxy_tls_conn_write(ICProxyTlsConn *conn, const void *buf, int len,
						int *out_event)
{
	int			rv;
	int			err;

	Assert(conn->handshake_done);
	*out_event = 0;

	if (len <= 0)
		return 0;

	rv = SSL_write(conn->ssl, buf, len);
	if (rv > 0)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: SSL_write wrote %d/%d bytes (fd=%d)",
			   rv, len, conn->fd);
		return rv;
	}

	err = SSL_get_error(conn->ssl, rv);
	if (err == SSL_ERROR_WANT_READ)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: SSL_write WANT_READ (fd=%d)", conn->fd);
		*out_event = UV_READABLE;
		return 0;
	}
	if (err == SSL_ERROR_WANT_WRITE)
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy TLS: SSL_write WANT_WRITE (fd=%d)", conn->fd);
		*out_event = UV_WRITABLE;
		return 0;
	}
	elog(WARNING, "ic-proxy TLS: SSL_write failed (fd=%d, ssl_err=%d): %s",
		 conn->fd, err, ic_proxy_tls_format_openssl_error());
	return -1;
}

#endif /* ENABLE_IC_PROXY */
