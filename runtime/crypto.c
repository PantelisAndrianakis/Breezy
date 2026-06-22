/* runtime/crypto.c -- the few libcrypto primitives the database drivers need for
   authentication (SCRAM-SHA-256 and MD5). Own TU: only a program that authenticates
   to a database references these symbols, and libcrypto is dlopen'd lazily at first
   use, so a trust-auth connection loads nothing. Distinct from tls.c's binding (TLS
   resolves its own libssl+libcrypto symbol set); the OS loader dedups the shared
   library when both are present. */
#include "breezy.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		HMODULE h = LoadLibraryA(names[i]);
		if (h)
		{
			return (void*)h;
		}
	}
	return NULL;
}
static void *dl_sym(void *h, const char *n)
{
	return (void*)GetProcAddress((HMODULE)h, n);
}
#else
#include <dlfcn.h>
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
		if (h)
		{
			return h;
		}
	}
	return NULL;
}
static void *dl_sym(void *h, const char *n)
{
	return dlsym(h, n);
}
#endif

static struct
{
	int loaded;   /* 0 unknown, 1 ok, -1 unavailable. */
	unsigned char *(*SHA1)(const unsigned char*, size_t, unsigned char*);
	unsigned char *(*SHA256)(const unsigned char*, size_t, unsigned char*);
	unsigned char *(*MD5)(const unsigned char*, size_t, unsigned char*);
	const void    *(*EVP_sha256)(void);
	unsigned char *(*HMAC)(const void*, const void*, int, const unsigned char*, size_t, unsigned char*, unsigned int*);
	int            (*PKCS5_PBKDF2_HMAC)(const char*, int, const unsigned char*, int, int, const void*, int, unsigned char*);
	int            (*RAND_bytes)(unsigned char*, int);
} cc;

/* Resolve libcrypto once. 1 on success, 0 if OpenSSL is unavailable. */
int bzy_crypto_load(void)
{
	if (cc.loaded == 1)
	{
		return 1;
	}
	if (cc.loaded == -1)
	{
		return 0;
	}

#ifdef _WIN32
	const char *names[] = { "libcrypto-3-x64.dll", "libcrypto-3.dll", "libcrypto.dll", NULL };
#else
	const char *names[] = { "libcrypto.so.3", "libcrypto.so", "libcrypto.so.1.1", NULL };
#endif
	void *h = dl_open_first(names);
	if (!h)
	{
		cc.loaded = -1;
		return 0;
	}

#define CSYM(field, name) do { \
		*(void**)(&cc.field) = dl_sym(h, name); \
		if (!cc.field) { cc.loaded = -1; return 0; } \
	} while (0)
	CSYM(SHA1, "SHA1");
	CSYM(SHA256, "SHA256");
	CSYM(MD5, "MD5");
	CSYM(EVP_sha256, "EVP_sha256");
	CSYM(HMAC, "HMAC");
	CSYM(PKCS5_PBKDF2_HMAC, "PKCS5_PBKDF2_HMAC");
	CSYM(RAND_bytes, "RAND_bytes");
#undef CSYM

	cc.loaded = 1;
	return 1;
}

void bzy_crypto_sha1(const unsigned char *in, size_t len, unsigned char *out20)
{
	cc.SHA1(in, len, out20);
}

void bzy_crypto_sha256(const unsigned char *in, size_t len, unsigned char *out32)
{
	cc.SHA256(in, len, out32);
}

void bzy_crypto_md5(const unsigned char *in, size_t len, unsigned char *out16)
{
	cc.MD5(in, len, out16);
}

void bzy_crypto_hmac_sha256(const unsigned char *key, int klen, const unsigned char *data, size_t dlen, unsigned char *out32)
{
	unsigned int ml = 32;
	cc.HMAC(cc.EVP_sha256(), key, klen, data, dlen, out32, &ml);
}

int bzy_crypto_pbkdf2_sha256(const char *pass, int plen, const unsigned char *salt, int slen, int iters, unsigned char *out, int outlen)
{
	return cc.PKCS5_PBKDF2_HMAC(pass, plen, salt, slen, iters, cc.EVP_sha256(), outlen, out);
}

int bzy_crypto_rand(unsigned char *out, int len)
{
	return cc.RAND_bytes(out, len) == 1;
}
