#include "breezy.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#else
#include <pthread.h>
#include <curl/curl.h>
#endif

/* readUrl context: the worker thread fills body/err; the breeze wraps body in a
   managed string after it resumes (no cross-thread ARC allocation). */
typedef struct
{
	char  url[2048];   /* In: the request URL (UTF-8/ASCII). */
	char *body;        /* Out: malloc'd response body (NULL on error). */
	int   body_len;    /* Out: body length in bytes. */
	char  err[256];    /* Out: error message ("" = success). */
} UrlCtx;

static void url_fail(UrlCtx *c, const char *msg)
{
	snprintf(c->err, sizeof(c->err), "%s", msg);
}

#ifdef _WIN32
/* Worker-side: the blocking WinHTTP fetch. Runs on an offload thread. */
static void url_fetch(void *vp)
{
	UrlCtx *c = (UrlCtx*)vp;
	c->body = NULL;
	c->body_len = 0;
	c->err[0] = '\0';

	wchar_t wurl[2048];
	if (MultiByteToWideChar(CP_UTF8, 0, c->url, -1, wurl, 2048) == 0)
	{
		url_fail(c, "readUrl: URL too long or invalid.");
		return;
	}

	URL_COMPONENTS uc;
	wchar_t host[256], path[1600];
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 1600;
	if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
	{
		url_fail(c, "readUrl: cannot parse URL (use http:// or https://).");
		return;
	}

	host[uc.dwHostNameLength] = L'\0';
	path[uc.dwUrlPathLength] = L'\0';
	if (path[0] == L'\0')
	{
		wcscpy(path, L"/");
	}

	HINTERNET ses = WinHttpOpen(L"breezy/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
								WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses)
	{
		url_fail(c, "readUrl: WinHttpOpen failed.");
		return;
	}

	HINTERNET conn = WinHttpConnect(ses, host, uc.nPort, 0);
	if (!conn)
	{
		WinHttpCloseHandle(ses);
		url_fail(c, "readUrl: connect failed.");
		return;
	}

	DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
									   WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
	if (!req)
	{
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(ses);
		url_fail(c, "readUrl: open request failed.");
		return;
	}

	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
							WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
			|| !WinHttpReceiveResponse(req, NULL))
	{
		WinHttpCloseHandle(req);
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(ses);
		url_fail(c, "readUrl: request failed (host unreachable or TLS error).");
		return;
	}

	char *buf = NULL;
	int len = 0, cap = 0;
	const int CAP_MAX = 64 * 1024 * 1024;
	for (;;)
	{
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(req, &avail))
		{
			free(buf);
			buf = NULL;
			len = 0;
			url_fail(c, "readUrl: read failed.");
			break;
		}

		if (avail == 0)
		{
			break;   /* End of response. */
		}

		if (len + (int)avail > CAP_MAX)
		{
			free(buf);
			buf = NULL;
			len = 0;
			url_fail(c, "readUrl: response exceeds 64 MiB cap.");
			break;
		}

		if (len + (int)avail > cap)
		{
			cap = (cap == 0) ? (int)avail + 4096 : cap * 2;
			if (cap < len + (int)avail)
			{
				cap = len + (int)avail;
			}

			buf = realloc(buf, (size_t)cap);
		}

		DWORD got = 0;
		if (!WinHttpReadData(req, buf + len, avail, &got) || got == 0)
		{
			break;
		}

		len += (int)got;
	}

	WinHttpCloseHandle(req);
	WinHttpCloseHandle(conn);
	WinHttpCloseHandle(ses);
	c->body = buf;
	c->body_len = len;
}

#else
/* ===== POSIX: libcurl fetch (http + https; TLS, redirects, chunked handled by
   libcurl). Runs on an offload thread; curl_easy_perform blocks. ===== */

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void curl_global_setup(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);   /* Once, before any worker uses curl. */
}

/* A growing body buffer with a 64 MiB cap (matches the WinHTTP path). */
typedef struct
{
	char  *buf;
	size_t len;
	size_t cap;
	int    over;   /* Exceeded the cap (or OOM). */
} BodyAcc;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	BodyAcc *a = (BodyAcc*)userdata;
	size_t n = size * nmemb;
	if (a->over)
	{
		return 0;
	}

	if (a->len + n > (size_t)(64 * 1024 * 1024))
	{
		a->over = 1;
		return 0;
	}

	if (a->len + n > a->cap)
	{
		size_t nc = (a->cap == 0) ? (n + 4096) : (a->cap * 2);
		if (nc < a->len + n)
		{
			nc = a->len + n;
		}

		char *nb = realloc(a->buf, nc);
		if (!nb)
		{
			a->over = 1;
			return 0;
		}

		a->buf = nb;
		a->cap = nc;
	}

	memcpy(a->buf + a->len, ptr, n);
	a->len += n;
	return n;
}

static void url_fetch(void *vp)
{
	UrlCtx *c = (UrlCtx*)vp;
	c->body = NULL;
	c->body_len = 0;
	c->err[0] = '\0';

	pthread_once(&g_curl_once, curl_global_setup);

	CURL *h = curl_easy_init();
	if (!h)
	{
		url_fail(c, "readUrl: curl init failed.");
		return;
	}

	BodyAcc a;
	a.buf = NULL;
	a.len = 0;
	a.cap = 0;
	a.over = 0;

	curl_easy_setopt(h, CURLOPT_URL, c->url);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);   /* Follow redirects (matches WinHTTP). */
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &a);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "breezy/1.0");
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);          /* Thread-safe: no SIGALRM-based timeouts. */
	CURLcode rc = curl_easy_perform(h);
	curl_easy_cleanup(h);

	if (a.over)
	{
		free(a.buf);
		url_fail(c, "readUrl: response exceeds 64 MiB cap.");
		return;
	}

	if (rc != CURLE_OK)
	{
		free(a.buf);
		url_fail(c, "readUrl: request failed (host unreachable or TLS error).");
		return;
	}

	c->body = a.buf;
	c->body_len = (int)a.len;
}
#endif

/* Breeze-side: park on the offload pool while the worker fetches, then wrap the
   body in a managed string. On failure, io_fail sets the thread-local error that
   the codegen-emitted bzy_io_check turns into a catchable IOException. */
void *bzy_net_read_url(void *url)
{
	UrlCtx c;
	snprintf(c.url, sizeof(c.url), "%.*s", (int)bzy_str_len(url), bzy_str_data(url));
	c.body = NULL;
	c.body_len = 0;
	c.err[0] = '\0';

	if (bzy_sched_current())
	{
		bzy_offload_run(url_fetch, &c);   /* Parks the breeze. */
	}
	else
	{
		url_fetch(&c);                    /* No breeze (unit/pre-scheduler): run inline. */
	}

	if (c.err[0] != '\0')
	{
		free(c.body);
		bzy_io_fail(c.err);
		return bzy_str_new("", 0);
	}

	void *s = bzy_str_new(c.body ? c.body : "", c.body_len);
	free(c.body);
	return s;
}
