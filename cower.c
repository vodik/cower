/* Copyright (c) 2010-2011 Dave Reisner
 *
 * cower.c
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/* glibc */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <pthread.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <wchar.h>
#include <wordexp.h>

/* external libs */
#include <alpm.h>
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <yajl/yajl_parse.h>

/* macros {{{ */
#define ALLOC_FAIL(s) do { cwr_fprintf(stderr, LOG_ERROR, "could not allocate %zd bytes\n", s); } while(0)
#define MALLOC(p, s, action) do { p = calloc(1, s); if(!p) { ALLOC_FAIL(s); action; } } while(0)
#define CALLOC(p, l, s, action) do { p = calloc(l, s); if(!p) { ALLOC_FAIL(s); action; } } while(0)
#define FREE(x)               do { free((void*)x); x = NULL; } while(0)
#define UNUSED                __attribute__((unused))
#define STREQ(x,y)            (strcmp((x),(y)) == 0)
#define STR_STARTS_WITH(x,y)  (strncmp((x),(y), strlen(y)) == 0)
#define NCFLAG(val, flag)     (!cfg.color && (val)) ? (flag) : ""
#define KEY_IS(k)             (strncmp(p->key, (k), p->keysz) == 0)

#ifndef PACMAN_ROOT
	#define PACMAN_ROOT         "/"
#endif
#ifndef PACMAN_DBPATH
	#define PACMAN_DBPATH       "/var/lib/pacman"
#endif
#ifndef PACMAN_CONFIG
	#define PACMAN_CONFIG       "/etc/pacman.conf"
#endif

#define COWER_USERAGENT       "cower/3.x"

#define AUR_BASE_URL          "%s://aur.archlinux.org%s"
#define AUR_PKG_URL_FORMAT    "%s://aur.archlinux.org/packages.php?ID="
#define AUR_RPC_URL           "%s://aur.archlinux.org/rpc.php?type=%s&arg=%s"
#define THREAD_DEFAULT        10
#define TIMEOUT_DEFAULT       10L
#define UNSET                 -1

#define AUR_QUERY_TYPE        "type"
#define AUR_QUERY_TYPE_INFO   "info"
#define AUR_QUERY_TYPE_SEARCH "search"
#define AUR_QUERY_TYPE_MSRCH  "msearch"
#define AUR_QUERY_ERROR       "error"
#define AUR_QUERY_RESULTCOUNT "resultcount"

#define NAME                  "Name"
#define VERSION               "Version"
#define URL                   "URL"
#define URLPATH               "URLPath"

#define AUR_ID                "ID"
#define AUR_CAT               "CategoryID"
#define AUR_DESC              "Description"
#define AUR_LICENSE           "License"
#define AUR_VOTES             "NumVotes"
#define AUR_OOD               "OutOfDate"
#define AUR_FIRSTSUB          "FirstSubmitted"
#define AUR_LASTMOD           "LastModified"

#define PKGBUILD_DEPENDS      "depends=("
#define PKGBUILD_MAKEDEPENDS  "makedepends=("
#define PKGBUILD_OPTDEPENDS   "optdepends=("
#define PKGBUILD_PROVIDES     "provides=("
#define PKGBUILD_CONFLICTS    "conflicts=("
#define PKGBUILD_REPLACES     "replaces=("

#define PKG_REPO              "Repository"
#define PKG_AURPAGE           "AUR Page"
#define PKG_PROVIDES          "Provides"
#define PKG_DEPENDS           "Depends On"
#define PKG_MAKEDEPENDS       "Makedepends"
#define PKG_OPTDEPENDS        "Optional Deps"
#define PKG_CONFLICTS         "Conflicts With"
#define PKG_REPLACES          "Replaces"
#define PKG_CAT               "Category"
#define PKG_NUMVOTES          "Votes"
#define PKG_LICENSE           "License"
#define PKG_OOD               "Out of Date"
#define PKG_DESC              "Description"
#define PKG_MAINT             "Maintainer"
#define PKG_FIRSTSUB          "Submitted"
#define PKG_LASTMOD           "Last Modified"
#define PKG_TIMEFMT           "%c"

#define INFO_INDENT           17
#define SRCH_INDENT           4
#define LIST_DELIM            "  "

#define NC                    "\033[0m"
#define BOLD                  "\033[1m"

#define BLACK                 "\033[0;30m"
#define RED                   "\033[0;31m"
#define GREEN                 "\033[0;32m"
#define YELLOW                "\033[0;33m"
#define BLUE                  "\033[0;34m"
#define MAGENTA               "\033[0;35m"
#define CYAN                  "\033[0;36m"
#define WHITE                 "\033[0;37m"
#define BOLDBLACK             "\033[1;30m"
#define BOLDRED               "\033[1;31m"
#define BOLDGREEN             "\033[1;32m"
#define BOLDYELLOW            "\033[1;33m"
#define BOLDBLUE              "\033[1;34m"
#define BOLDMAGENTA           "\033[1;35m"
#define BOLDCYAN              "\033[1;36m"
#define BOLDWHITE             "\033[1;37m"

#define REGEX_OPTS            REG_ICASE|REG_EXTENDED|REG_NOSUB|REG_NEWLINE
#define REGEX_CHARS           "^.+*?$[](){}|\\"

#define BRIEF_ERR             "E"
#define BRIEF_WARN            "W"
#define BRIEF_OK              "S"
/* }}} */

/* typedefs and objects {{{ */
typedef enum __loglevel_t {
	LOG_INFO    = 1,
	LOG_ERROR   = (1 << 1),
	LOG_WARN    = (1 << 2),
	LOG_DEBUG   = (1 << 3),
	LOG_VERBOSE = (1 << 4),
	LOG_BRIEF   = (1 << 5)
} loglevel_t;

typedef enum __operation_t {
	OP_SEARCH   = 1,
	OP_INFO     = (1 << 1),
	OP_DOWNLOAD = (1 << 2),
	OP_UPDATE   = (1 << 3),
	OP_MSEARCH  = (1 << 4)
} operation_t;

enum {
	OP_DEBUG = 1000,
	OP_FORMAT,
	OP_IGNOREPKG,
	OP_IGNOREREPO,
	OP_LISTDELIM,
	OP_NOSSL,
	OP_THREADS,
	OP_TIMEOUT,
	OP_VERSION,
	OP_NOIGNOREOOD
};

typedef enum __pkgdetail_t {
	PKGDETAIL_DEPENDS = 0,
	PKGDETAIL_MAKEDEPENDS,
	PKGDETAIL_OPTDEPENDS,
	PKGDETAIL_PROVIDES,
	PKGDETAIL_CONFLICTS,
	PKGDETAIL_REPLACES,
	PKGDETAIL_MAX
} pkgdetail_t;

struct strings_t {
	const char *error;
	const char *warn;
	const char *info;
	const char *pkg;
	const char *repo;
	const char *url;
	const char *ood;
	const char *utd;
	const char *nc;
};

struct aurpkg_t {
	char *desc;
	char *lic;
	char *maint;
	char *name;
	char *url;
	char *urlpath;
	char *ver;
	int cat;
	int id;
	int ood;
	int votes;
	time_t firstsub;
	time_t lastmod;
	alpm_list_t *conflicts;
	alpm_list_t *depends;
	alpm_list_t *makedepends;
	alpm_list_t *optdepends;
	alpm_list_t *provides;
	alpm_list_t *replaces;
};

struct yajl_parser_t {
	alpm_list_t *pkglist;
	int resultcount;
	struct aurpkg_t *aurpkg;
	char key[32];
	size_t keysz;
	int json_depth;
};

struct response_t {
	char *data;
	size_t size;
};

struct task_t {
	void *(*threadfn)(CURL*, void*);
	void (*printfn)(struct aurpkg_t*);
};

struct openssl_mutex_t {
	pthread_mutex_t *lock;
	long *lock_count;
};
/* }}} */

/* function prototypes {{{ */
static alpm_list_t *alpm_find_foreign_pkgs(void);
static alpm_handle_t *alpm_init(void);
static int alpm_pkg_is_foreign(alpm_pkg_t*);
static const char *alpm_provides_pkg(const char*);
static int archive_extract_file(const struct response_t*, char**);
static int aurpkg_cmp(const void*, const void*);
static struct aurpkg_t *aurpkg_dup(const struct aurpkg_t*);
static void aurpkg_free(void*);
static void aurpkg_free_inner(struct aurpkg_t*);
static CURL *curl_init_easy_handle(CURL*);
static char *curl_get_url_as_buffer(CURL*, const char*);
static size_t curl_write_response(void*, size_t, size_t, void*);
static int cwr_asprintf(char**, const char*, ...) __attribute__((format(printf,2,3)));
static int cwr_fprintf(FILE*, loglevel_t, const char*, ...) __attribute__((format(printf,3,4)));
static int cwr_printf(loglevel_t, const char*, ...) __attribute__((format(printf,2,3)));
static int cwr_vfprintf(FILE*, loglevel_t, const char*, va_list) __attribute__((format(printf,3,0)));
static void *download(CURL *curl, void*);
static alpm_list_t *filter_results(alpm_list_t*);
static char *get_file_as_buffer(const char*);
static int getcols(void);
static void indentprint(const char*, int);
static int json_end_map(void*);
static int json_integer(void *ctx, long long);
static int json_map_key(void*, const unsigned char*, size_t);
static int json_start_map(void*);
static int json_string(void*, const unsigned char*, size_t);
static void openssl_crypto_cleanup(void);
static void openssl_crypto_init(void);
static unsigned long openssl_thread_id(void) __attribute__ ((const));
static void openssl_thread_cb(int, int, const char*, int);
static alpm_list_t *parse_bash_array(alpm_list_t*, char*, pkgdetail_t);
static int parse_configfile(void);
static int parse_options(int, char*[]);
static int pkg_is_binary(const char *pkg);
static void pkgbuild_get_extinfo(char*, alpm_list_t**[]);
static int print_escaped(const char*);
static void print_extinfo_list(alpm_list_t*, const char*, const char*, int);
static void print_pkg_formatted(struct aurpkg_t*);
static void print_pkg_info(struct aurpkg_t*);
static void print_pkg_search(struct aurpkg_t*);
static void print_results(alpm_list_t*, void (*)(struct aurpkg_t*));
static int resolve_dependencies(CURL*, const char*, const char*);
static int set_working_dir(void);
static int strings_init(void);
static size_t strtrim(char*);
static void *task_download(CURL*, void*);
static void *task_query(CURL*, void*);
static void *task_update(CURL*, void*);
static void *thread_pool(void*);
static char *url_escape(char*, int, const char*);
static void usage(void);
static void version(void);
static size_t yajl_parse_stream(void*, size_t, size_t, void*);
/* }}} */

/* runtime configuration {{{ */
static struct {
	char *dlpath;
	const char *delim;
	const char *format;
	const char *proto;

	operation_t opmask;
	loglevel_t logmask;

	short color;
	short ignoreood;
	int extinfo:1;
	int force:1;
	int getdeps:1;
	int quiet:1;
	int skiprepos:1;
	int secure:1;
	int maxthreads;
	long timeout;

	alpm_list_t *targets;
	struct {
	  alpm_list_t *pkgs;
	  alpm_list_t *repos;
	} ignore;
} cfg; /* }}} */

/* globals {{{ */
struct strings_t *colstr;
alpm_handle_t *pmhandle;
alpm_db_t *db_local;
alpm_list_t *workq;
struct openssl_mutex_t openssl_lock;
static pthread_mutex_t listlock = PTHREAD_MUTEX_INITIALIZER;

static yajl_callbacks callbacks = {
	NULL,             /* null */
	NULL,             /* boolean */
	json_integer,     /* integer */
	NULL,             /* double */
	NULL,             /* number */
	json_string,      /* string */
	json_start_map,   /* start_map */
	json_map_key,     /* map_key */
	json_end_map,     /* end_map */
	NULL,             /* start_array */
	NULL              /* end_array */
};

static char const *digits = "0123456789";
static char const *printf_flags = "'-+ #0I";

static const char *aur_cat[] = { NULL, "None", "daemons", "devel", "editors",
                                "emulators", "games", "gnome", "i18n", "kde", "lib",
                                "modules", "multimedia", "network", "office",
                                "science", "system", "x11", "xfce", "kernels" };
/* }}} */

alpm_handle_t *alpm_init(void) /* {{{ */
{
	FILE *fp;
	char line[PATH_MAX];
	char *ptr, *section = NULL;
	enum _alpm_errno_t err;

	cwr_printf(LOG_DEBUG, "initializing alpm\n");
	pmhandle = alpm_initialize(PACMAN_ROOT, PACMAN_DBPATH, &err);
	if(!pmhandle) {
		fprintf(stderr, "failed to initialize alpm: %s\n", alpm_strerror(err));
		return NULL;
	}

	fp = fopen(PACMAN_CONFIG, "r");
	if(!fp) {
		return pmhandle;
	}

	while(fgets(line, PATH_MAX, fp)) {
		size_t linelen;

		if((ptr = strchr(line, '#'))) {
			*ptr = '\0';
		}
		if(!(linelen = strtrim(line))) {
			continue;
		}

		if(line[0] == '[' && line[linelen - 1] == ']') {
			free(section);
			section = strndup(&line[1], linelen - 2);

			if(strcmp(section, "options") != 0) {
				if(!cfg.skiprepos && !alpm_list_find_str(cfg.ignore.repos, section)) {
					alpm_register_syncdb(pmhandle, section, 0);
					cwr_printf(LOG_DEBUG, "registering alpm db: %s\n", section);
				}
			}
		} else {
			char *key, *token;

			key = ptr = line;
			strsep(&ptr, "=");
			strtrim(key);
			strtrim(ptr);
			if(STREQ(key, "IgnorePkg")) {
				for(token = strtok(ptr, "\t\n "); token; token = strtok(NULL, "\t\n ")) {
					cwr_printf(LOG_DEBUG, "ignoring package: %s\n", token);
					cfg.ignore.pkgs = alpm_list_add(cfg.ignore.pkgs, strdup(token));
				}
			}
		}
	}

	db_local = alpm_get_localdb(pmhandle);

	free(section);
	fclose(fp);

	return pmhandle;
} /* }}} */

alpm_list_t *alpm_find_foreign_pkgs(void) /* {{{ */
{
	const alpm_list_t *i;
	alpm_list_t *ret = NULL;

	for(i = alpm_db_get_pkgcache(db_local); i; i = alpm_list_next(i)) {
		alpm_pkg_t *pkg = i->data;

		if(alpm_pkg_is_foreign(pkg)) {
			ret = alpm_list_add(ret, strdup(alpm_pkg_get_name(pkg)));
		}
	}

	return ret;
} /* }}} */

int alpm_pkg_is_foreign(alpm_pkg_t *pkg) /* {{{ */
{
	const alpm_list_t *i;
	const char *pkgname;

	pkgname = alpm_pkg_get_name(pkg);

	for(i = alpm_get_syncdbs(pmhandle); i; i = alpm_list_next(i)) {
		if(alpm_db_get_pkg(i->data, pkgname)) {
			return 0;
		}
	}

	return 1;
} /* }}} */

const char *alpm_provides_pkg(const char *pkgname) /* {{{ */
{
	const alpm_list_t *i;
	const char *dbname = NULL;
	static pthread_mutex_t alpmlock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&alpmlock);
	for(i = alpm_get_syncdbs(pmhandle); i; i = alpm_list_next(i)) {
		alpm_db_t *db = i->data;
		if(alpm_find_satisfier(alpm_db_get_pkgcache(db), pkgname)) {
			dbname = alpm_db_get_name(db);
			break;
		}
	}
	pthread_mutex_unlock(&alpmlock);

	return dbname;
} /* }}} */

int archive_extract_file(const struct response_t *file, char **subdir) /* {{{ */
{
	struct archive *archive;
	struct archive_entry *entry;
	const int archive_flags = ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME;
	int want_subdir = subdir != NULL, ok, ret = 0;

	archive = archive_read_new();
	archive_read_support_compression_all(archive);
	archive_read_support_format_all(archive);

	want_subdir = (subdir != NULL);

	ret = archive_read_open_memory(archive, file->data, file->size);
	if(ret == ARCHIVE_OK) {
		while(archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
			const char *entryname = archive_entry_pathname(entry);

			if(want_subdir) {
				if(entryname) {
					const struct stat *st = archive_entry_stat(entry);
					if (S_ISDIR(st->st_mode)) {
						size_t len = strlen(entryname);
						*subdir = strndup(entryname, entryname[len - 1] == '/' ? len - 1 : len);
						want_subdir = 0;
					}
				}
			}

			cwr_printf(LOG_DEBUG, "extracting file: %s\n", entryname);

			ok = archive_read_extract(archive, entry, archive_flags);
			/* NOOP ON ARCHIVE_{OK,WARN,RETRY} */
			if(ok == ARCHIVE_FATAL || ok == ARCHIVE_WARN) {
				ret = archive_errno(archive);
				break;
			} else if (ok == ARCHIVE_EOF) {
				ret = 0;
				break;
			}
		}
		archive_read_close(archive);
	}
	archive_read_free(archive);

	if(want_subdir && *subdir == NULL) {
		/* massively broken PKGBUILD without a subdir... */
		*subdir = strdup("");
	}

	return ret;
} /* }}} */

int aurpkg_cmp(const void *p1, const void *p2) /* {{{ */
{
	const struct aurpkg_t *pkg1 = p1;
	const struct aurpkg_t *pkg2 = p2;

	return strcmp(pkg1->name, pkg2->name);
} /* }}} */

struct aurpkg_t *aurpkg_dup(const struct aurpkg_t *pkg) /* {{{ */
{
	struct aurpkg_t *newpkg;

	MALLOC(newpkg, sizeof(struct aurpkg_t), return NULL);
	memcpy(newpkg, pkg, sizeof(struct aurpkg_t));

	return newpkg;
} /* }}} */

void aurpkg_free(void *pkg) /* {{{ */
{
	aurpkg_free_inner(pkg);
	FREE(pkg);
} /* }}} */

void aurpkg_free_inner(struct aurpkg_t *pkg) /* {{{ */
{
	if(!pkg) {
		return;
	}

	/* free allocated string fields */
	FREE(pkg->name);
	FREE(pkg->maint);
	FREE(pkg->ver);
	FREE(pkg->urlpath);
	FREE(pkg->desc);
	FREE(pkg->url);
	FREE(pkg->lic);

	/* free extended list info */
	FREELIST(pkg->depends);
	FREELIST(pkg->makedepends);
	FREELIST(pkg->optdepends);
	FREELIST(pkg->provides);
	FREELIST(pkg->conflicts);
	FREELIST(pkg->replaces);
} /* }}} */

int cwr_asprintf(char **string, const char *format, ...) /* {{{ */
{
	int ret = 0;
	va_list args;

	va_start(args, format);
	ret = vasprintf(string, format, args);
	va_end(args);

	if(ret == -1) {
		cwr_fprintf(stderr, LOG_ERROR, "failed to allocate string\n");
	}

	return ret;
} /* }}} */

int cwr_fprintf(FILE *stream, loglevel_t level, const char *format, ...) /* {{{ */
{
	int ret;
	va_list args;

	va_start(args, format);
	ret = cwr_vfprintf(stream, level, format, args);
	va_end(args);

	return ret;
} /* }}} */

int cwr_printf(loglevel_t level, const char *format, ...) /* {{{ */
{
	int ret;
	va_list args;

	va_start(args, format);
	ret = cwr_vfprintf(stdout, level, format, args);
	va_end(args);

	return ret;
} /* }}} */

int cwr_vfprintf(FILE *stream, loglevel_t level, const char *format, va_list args) /* {{{ */
{
	const char *prefix;
	char bufout[128];

	if(!(cfg.logmask & level)) {
		return 0;
	}

	switch(level) {
		case LOG_VERBOSE:
		case LOG_INFO:
			prefix = colstr->info;
			break;
		case LOG_ERROR:
			prefix = colstr->error;
			break;
		case LOG_WARN:
			prefix = colstr->warn;
			break;
		case LOG_DEBUG:
			prefix = "debug:";
			break;
		default:
			prefix = "";
			break;
	}

	/* f.l.w.: 128 should be big enough... */
	snprintf(bufout, 128, "%s %s", prefix, format);

	return vfprintf(stream, bufout, args);
} /* }}} */

CURL *curl_init_easy_handle(CURL *handle) /* {{{ */
{
	if(!handle) {
		return NULL;
	}

	curl_easy_reset(handle);
	curl_easy_setopt(handle, CURLOPT_USERAGENT, COWER_USERAGENT);
	curl_easy_setopt(handle, CURLOPT_ENCODING, "deflate, gzip");
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, cfg.timeout);
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

	/* This is required of multi-threaded apps using timeouts. See
	 * curl_easy_setopt(3) */
	if(cfg.timeout > 0L) {
		curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
	}

	return handle;
} /* }}} */

char *curl_get_url_as_buffer(CURL *curl, const char *url) /* {{{ */
{
	long httpcode;
	struct response_t response = { NULL, 0 };
	CURLcode curlstat;

	curl = curl_init_easy_handle(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	cwr_printf(LOG_DEBUG, "get_url_as_buffer: curl_easy_perform %s\n", url);
	curlstat = curl_easy_perform(curl);
	if(curlstat != CURLE_OK) {
		cwr_fprintf(stderr, LOG_ERROR, "%s: %s\n", url, curl_easy_strerror(curlstat));
		goto finish;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpcode);
	cwr_printf(LOG_DEBUG, "get_url_as_buffer: %s: server responded with %ld\n", url, httpcode);
	if(httpcode >= 400) {
		cwr_fprintf(stderr, LOG_ERROR, "%s: server responded with HTTP %ld\n",
				url, httpcode);
	}

finish:
	return response.data;
} /* }}} */

size_t curl_write_response(void *ptr, size_t size, size_t nmemb, void *stream) /* {{{ */
{
	void *newdata;
	size_t realsize = size * nmemb;
	struct response_t *mem = (struct response_t*)stream;

	newdata = realloc(mem->data, mem->size + realsize + 1);
	if(newdata) {
		mem->data = newdata;
		memcpy(&(mem->data[mem->size]), ptr, realsize);
		mem->size += realsize;
		mem->data[mem->size] = '\0';
	} else {
		cwr_fprintf(stderr, LOG_ERROR, "failed to reallocate %zd bytes\n",
				mem->size + realsize + 1);
		return 0;
	}

	return realsize;
} /* }}} */

void *download(CURL *curl, void *arg) /* {{{ */
{
	alpm_list_t *queryresult = NULL;
	struct aurpkg_t *result;
	CURLcode curlstat;
	char *url, *escaped, *subdir = NULL;
	int ret;
	long httpcode;
	struct response_t response = { 0, 0 };

	curl = curl_init_easy_handle(curl);

	queryresult = task_query(curl, arg);
	if(!queryresult) {
		cwr_fprintf(stderr, LOG_BRIEF, BRIEF_ERR "\t%s\t", (const char*)arg);
		cwr_fprintf(stderr, LOG_ERROR, "no results found for %s\n", (const char*)arg);
		return NULL;
	}

	if(access(arg, F_OK) == 0 && !cfg.force) {
		cwr_fprintf(stderr, LOG_BRIEF, BRIEF_ERR "\t%s\t", (const char*)arg);
		cwr_fprintf(stderr, LOG_ERROR, "`%s/%s' already exists. Use -f to overwrite.\n",
				cfg.dlpath, (const char*)arg);
		alpm_list_free_inner(queryresult, aurpkg_free);
		alpm_list_free(queryresult);
		return NULL;
	}

	curl_easy_setopt(curl, CURLOPT_ENCODING, "identity"); /* disable compression */
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);

	result = queryresult->data;
	escaped = url_escape(result->urlpath, 0, "/");
	cwr_asprintf(&url, AUR_BASE_URL, cfg.proto, escaped);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	free(escaped);

	cwr_printf(LOG_DEBUG, "[%s]: curl_easy_perform %s\n", (const char*)arg, url);
	curlstat = curl_easy_perform(curl);

	if(curlstat != CURLE_OK) {
		cwr_fprintf(stderr, LOG_BRIEF, BRIEF_ERR "\t%s\t", (const char*)arg);
		cwr_fprintf(stderr, LOG_ERROR, "[%s]: %s\n", (const char*)arg, curl_easy_strerror(curlstat));
		goto finish;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpcode);
	cwr_printf(LOG_DEBUG, "[%s]: server responded with %ld\n", (const char *)arg, httpcode);

	switch(httpcode) {
		case 200:
			break;
		default:
			cwr_fprintf(stderr, LOG_BRIEF, BRIEF_ERR "\t%s\t", (const char*)arg);
			cwr_fprintf(stderr, LOG_ERROR, "[%s]: server responded with HTTP %ld\n",
					(const char*)arg, httpcode);
			goto finish;
	}

	ret = archive_extract_file(&response, &subdir);
	if(ret != 0) {
		cwr_fprintf(stderr, LOG_BRIEF, BRIEF_ERR "\t%s\t", (const char*)arg);
		cwr_fprintf(stderr, LOG_ERROR, "[%s]: failed to extract tarball: %s\n",
				(const char*)arg, strerror(ret));
		goto finish;
	}

	cwr_printf(LOG_BRIEF, BRIEF_OK "\t%s\t", result->name);
	cwr_printf(LOG_INFO, "%s%s%s downloaded to %s\n",
			colstr->pkg, result->name, colstr->nc, cfg.dlpath);

	if(cfg.getdeps) {
		resolve_dependencies(curl, arg, subdir);
	}

finish:
	FREE(url);
	FREE(response.data);
	FREE(subdir);

	return queryresult;
} /* }}} */

alpm_list_t *filter_results(alpm_list_t *list) /* {{{ */
{
	const alpm_list_t *i, *j;
	alpm_list_t *filterlist = NULL;

	if(!(cfg.opmask & OP_SEARCH)) {
		return list;
	}

	for(i = cfg.targets; i; i = alpm_list_next(i)) {
		regex_t regex;
		const char *targ = i->data;
		filterlist = NULL;

		if(regcomp(&regex, targ, REGEX_OPTS) == 0) {
			for(j = list; j; j = alpm_list_next(j)) {
				struct aurpkg_t *pkg = j->data;
				const char *name = pkg->name;
				const char *desc = pkg->desc;

				if(regexec(&regex, name, 0, 0, 0) != REG_NOMATCH ||
						regexec(&regex, desc, 0, 0, 0) != REG_NOMATCH) {
					filterlist = alpm_list_add(filterlist, pkg);
				} else {
					aurpkg_free(pkg);
				}
			}
			regfree(&regex);
		}

		/* switcheroo */
		alpm_list_free(list);
		list = filterlist;
	}

	return alpm_list_msort(filterlist, alpm_list_count(filterlist), aurpkg_cmp);
} /* }}} */

int getcols(void) /* {{{ */
{
	int termwidth = -1;
	const int default_tty = 80;
	const int default_notty = 0;

	if(!isatty(fileno(stdout))) {
		return default_notty;
	}

#ifdef TIOCGSIZE
	struct ttysize win;
	if(ioctl(1, TIOCGSIZE, &win) == 0) {
		termwidth = win.ts_cols;
	}
#elif defined(TIOCGWINSZ)
	struct winsize win;
	if(ioctl(1, TIOCGWINSZ, &win) == 0) {
		termwidth = win.ws_col;
	}
#endif
	return termwidth <= 0 ? default_tty : termwidth;
} /* }}} */

char *get_file_as_buffer(const char *path) /* {{{ */
{
	FILE *fp;
	char *buf;
	long fsize, nread;

	fp = fopen(path, "r");
	if(!fp) {
		cwr_fprintf(stderr, LOG_ERROR, "fopen: %s\n", strerror(errno));
		return NULL;
	}

	fseek(fp, 0L, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0L, SEEK_SET);

	CALLOC(buf, 1, (ssize_t)fsize + 1, return NULL);

	nread = fread(buf, 1, fsize, fp);
	fclose(fp);

	if(nread < fsize) {
		cwr_fprintf(stderr, LOG_ERROR, "Failed to read full PKGBUILD\n");
		return NULL;
	}

	return buf;
} /* }}} */

void indentprint(const char *str, int indent) /* {{{ */
{
	wchar_t *wcstr;
	const wchar_t *p;
	int len, cidx, cols;

	if(!str) {
		return;
	}

	cols = getcols();

	/* if we're not a tty, print without indenting */
	if(cols == 0) {
		printf("%s", str);
		return;
	}

	len = strlen(str) + 1;
	CALLOC(wcstr, len, sizeof(wchar_t), return);
	len = mbstowcs(wcstr, str, len);
	p = wcstr;
	cidx = indent;

	if(!p || !len) {
		return;
	}

	while(*p) {
		if(*p == L' ') {
			const wchar_t *q, *next;
			p++;
			if(!p || *p == L' ') {
				continue;
			}
			next = wcschr(p, L' ');
			if(!next) {
				next = p + wcslen(p);
			}

			/* len captures # cols */
			len = 0;
			q = p;

			while(q < next) {
				len += wcwidth(*q++);
			}

			if(len > (cols - cidx - 1)) {
				/* wrap to a newline and reindent */
				printf("\n%-*s", indent, "");
				cidx = indent;
			} else {
				printf(" ");
				cidx++;
			}
			continue;
		}
#ifdef __clang__
		printf("%lc", *p);
#else /* assume GCC */
		printf("%lc", (wint_t)*p);
#endif
		cidx += wcwidth(*p);
		p++;
	}
	free(wcstr);
} /* }}} */

int json_end_map(void *ctx) /* {{{ */
{
	struct yajl_parser_t *p = (struct yajl_parser_t*)ctx;

	p->json_depth--;
	if(p->json_depth > 0) {
		if(!(p->aurpkg->ood && cfg.ignoreood)) {
			p->pkglist = alpm_list_add_sorted(p->pkglist, aurpkg_dup(p->aurpkg), aurpkg_cmp);
		} else {
			aurpkg_free_inner(p->aurpkg);
		}
	}

	return 1;
} /* }}} */

int json_integer(void *ctx, long long val) /* {{{ */
{
	struct yajl_parser_t *p = (struct yajl_parser_t*)ctx;

	if(KEY_IS(AUR_ID)) {
		p->aurpkg->id = (int)val;
	} else if(KEY_IS(AUR_CAT)) {
		p->aurpkg->cat = (int)val;
	} else if(KEY_IS(AUR_VOTES)) {
		p->aurpkg->votes = (int)val;
	} else if(KEY_IS(AUR_OOD)) {
		p->aurpkg->ood = (int)val;
	} else if(KEY_IS(AUR_FIRSTSUB)) {
		p->aurpkg->firstsub = (time_t)val;
	} else if(KEY_IS(AUR_LASTMOD)) {
		p->aurpkg->lastmod = (time_t)val;
	} else if(KEY_IS(AUR_QUERY_RESULTCOUNT)) {
		p->resultcount = (int)val;
	}

	return 1;
} /* }}} */

int json_map_key(void *ctx, const unsigned char *data, size_t size) /* {{{ */
{
	struct yajl_parser_t *p = (struct yajl_parser_t*)ctx;

	p->keysz = size;
	memcpy(p->key, (const char*)data, size);
	p->key[size] = '\0';

	return 1;
} /* }}} */

int json_start_map(void *ctx) /* {{{ */
{
	struct yajl_parser_t *p = (struct yajl_parser_t*)ctx;

	p->json_depth++;
	if(p->json_depth > 1) {
		memset(p->aurpkg, 0, sizeof(struct aurpkg_t));
	}

	return 1;
} /* }}} */

int json_string(void *ctx, const unsigned char *data, size_t size) /* {{{ */
{
	struct yajl_parser_t *p = (struct yajl_parser_t*)ctx;
	char buffer[32];

	if(KEY_IS(AUR_QUERY_TYPE) &&
			STR_STARTS_WITH((const char*)data, AUR_QUERY_ERROR)) {
		return 1;
	}

#define VALDUPE(dest, src, n) \
	dest = malloc(n + 1); \
	memcpy(dest, src, n); \
	dest[n] = '\0';

#define NUMCOPY(dest, src, n) \
	memcpy(buffer, src, n); \
	buffer[n] = '\0'; \
	dest = strtol(buffer, NULL, 10);

	if(KEY_IS(AUR_ID)) {
		NUMCOPY(p->aurpkg->id, data, size);
	} else if(KEY_IS(NAME)) {
		VALDUPE(p->aurpkg->name, data, size);
	} else if(KEY_IS(PKG_MAINT)) {
		VALDUPE(p->aurpkg->maint, data, size);
	} else if(KEY_IS(VERSION)) {
		VALDUPE(p->aurpkg->ver, data, size);
	} else if(KEY_IS(AUR_CAT)) {
		NUMCOPY(p->aurpkg->cat, data, size);
	} else if(KEY_IS(AUR_DESC)) {
		VALDUPE(p->aurpkg->desc, data, size);
	} else if(KEY_IS(URL)) {
		VALDUPE(p->aurpkg->url, data, size);
	} else if(KEY_IS(URLPATH)) {
		VALDUPE(p->aurpkg->urlpath, data, size);
	} else if(KEY_IS(AUR_LICENSE)) {
		VALDUPE(p->aurpkg->lic, data, size);
	} else if(KEY_IS(AUR_VOTES)) {
		NUMCOPY(p->aurpkg->votes, data, size);
	} else if(KEY_IS(AUR_OOD)) {
		p->aurpkg->ood = *data - 48;
	} else if(KEY_IS(AUR_FIRSTSUB)) {
		NUMCOPY(p->aurpkg->firstsub, data, size);
	} else if(KEY_IS(AUR_LASTMOD)) {
		NUMCOPY(p->aurpkg->lastmod, data, size);
	}

	return 1;
} /* }}} */

void openssl_crypto_cleanup(void) /* {{{ */
{
	int i;

	if(!cfg.secure) {
		return;
	}

	CRYPTO_set_locking_callback(NULL);

	for(i = 0; i < CRYPTO_num_locks(); i++) {
		pthread_mutex_destroy(&(openssl_lock.lock[i]));
	}

	OPENSSL_free(openssl_lock.lock);
	OPENSSL_free(openssl_lock.lock_count);
} /* }}} */

void openssl_crypto_init(void) /* {{{ */
{
	int i;

	openssl_lock.lock = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));
	openssl_lock.lock_count = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(long));
	for(i = 0; i < CRYPTO_num_locks(); i++) {
		openssl_lock.lock_count[i] = 0;
		pthread_mutex_init(&(openssl_lock.lock[i]), NULL);
	}

	CRYPTO_set_id_callback(openssl_thread_id);
	CRYPTO_set_locking_callback(openssl_thread_cb);
} /* }}} */

void openssl_thread_cb(int mode, int type, const char UNUSED *file, /* {{{ */
		int UNUSED line)
{
	if(mode & CRYPTO_LOCK) {
		pthread_mutex_lock(&(openssl_lock.lock[type]));
		openssl_lock.lock_count[type]++;
	} else {
		pthread_mutex_unlock(&(openssl_lock.lock[type]));
		openssl_lock.lock_count[type]--;
	}
} /* }}} */

unsigned long openssl_thread_id(void) /* {{{ */
{
	return pthread_self();
} /* }}} */

alpm_list_t *parse_bash_array(alpm_list_t *deplist, char *array, pkgdetail_t type) /* {{{ */
{
	char *ptr, *token, *saveptr;

	if(!array) {
		return NULL;
	}

	if(type == PKGDETAIL_OPTDEPENDS) {
		const char *arrayend = rawmemchr(array, '\0');
		for(token = array; token <= arrayend; token++) {
			if(*token == '\'' || *token == '\"') {
				token++;
				ptr = memchr(token, *(token - 1), arrayend - token);
				*ptr = '\0';
			} else if(isalpha(*token)) {
				ptr = token;
				while(!isspace(*++ptr));
				*(ptr - 1) = '\0';
			} else {
				continue;
			}

			strtrim(token);
			cwr_printf(LOG_DEBUG, "adding depend: %s\n", token);
			deplist = alpm_list_add(deplist, strdup(token));

			token = ptr;
		}
		return deplist;
	}

	for(token = strtok_r(array, " \t\n", &saveptr); token;
			token = strtok_r(NULL, " \t\n", &saveptr)) {
		/* found an embedded comment. skip to the next line */
		if(*token == '#') {
			strtok_r(NULL, "\n", &saveptr);
			continue;
		}

		/* unquote the element */
		if(*token == '\'' || *token == '\"') {
			ptr = strrchr(token + 1, *token);
			if(ptr) {
				token++;
				*ptr = '\0';
			}
		}

		/* some people feel compelled to do insane things in PKGBUILDs. these people suck */
		if(!*token || strlen(token) < 2 || *token == '$') {
			continue;
		}

		cwr_printf(LOG_DEBUG, "adding depend: %s\n", token);
		if(!alpm_list_find_str(deplist, token)) {
			deplist = alpm_list_add(deplist, strdup(token));
		}
	}

	return deplist;
} /* }}} */

int parse_configfile(void) /* {{{ */
{
	char *xdg_config_home, *home, *config_path;
	char line[PATH_MAX];
	int ret = 0;
	FILE *fp;

	xdg_config_home = getenv("XDG_CONFIG_HOME");
	if(xdg_config_home) {
		cwr_asprintf(&config_path, "%s/cower/config", xdg_config_home);
	} else {
		home = getenv("HOME");
		if(!home) {
			cwr_fprintf(stderr, LOG_ERROR, "Unable to find path to config file.\n");
			return 1;
		}
		cwr_asprintf(&config_path, "%s/.config/cower/config", getenv("HOME"));
	}

	fp = fopen(config_path, "r");
	if(!fp) {
		cwr_printf(LOG_DEBUG, "config file not found. skipping parsing\n");
		return 0; /* not an error, just nothing to do here */
	}

	/* don't need this anymore, get rid of it ASAP */
	free(config_path);

	while(fgets(line, PATH_MAX, fp)) {
		char *key, *val;
		size_t linelen;

		linelen = strtrim(line);
		if(!linelen || line[0] == '#') {
			continue;
		}

		if((val = strchr(line, '#'))) {
			*val = '\0';
		}

		key = val = line;
		strsep(&val, "=");
		strtrim(key);
		strtrim(val);

		if(val && !*val) {
			val = NULL;
		}

		cwr_printf(LOG_DEBUG, "found config option: %s => %s\n", key, val);

		/* colors are not initialized in this section, so usage of cwr_printf
		 * functions is verboten unless we're using loglevel_t LOG_DEBUG */
		if(STREQ(key, "NoSSL")) {
			cfg.secure &= 0;
			cfg.proto = "http";
		} else if(STREQ(key, "IgnoreRepo")) {
			for(key = strtok(val, " "); key; key = strtok(NULL, " ")) {
				cwr_printf(LOG_DEBUG, "ignoring repo: %s\n", key);
				cfg.ignore.repos = alpm_list_add(cfg.ignore.repos, strdup(key));
			}
		} else if(STREQ(key, "IgnorePkg")) {
			for(key = strtok(val, " "); key; key = strtok(NULL, " ")) {
				cwr_printf(LOG_DEBUG, "ignoring package: %s\n", key);
				cfg.ignore.pkgs = alpm_list_add(cfg.ignore.pkgs, strdup(key));
			}
		} else if(STREQ(key, "IgnoreOOD")) {
			if(cfg.ignoreood == UNSET) {
				cfg.ignoreood = 1;
			}
		} else if(STREQ(key, "TargetDir")) {
			if(val && !cfg.dlpath) {
				wordexp_t p;
				if(wordexp(val, &p, 0) == 0) {
					if(p.we_wordc == 1) {
						cfg.dlpath = strdup(p.we_wordv[0]);
					}
					wordfree(&p);
					/* error on relative paths */
					if(*cfg.dlpath != '/') {
						fprintf(stderr, "error: TargetDir cannot be a relative path\n");
						ret = 1;
					}
				} else {
					fprintf(stderr, "error: failed to resolve option to TargetDir\n");
					ret = 1;
				}
			}
		} else if(STREQ(key, "MaxThreads")) {
			if(val && cfg.maxthreads == UNSET) {
				cfg.maxthreads = strtol(val, &key, 10);
				if(*key != '\0' || cfg.maxthreads <= 0) {
					fprintf(stderr, "error: invalid option to MaxThreads: %s\n", val);
					ret = 1;
				}
			}
		} else if(STREQ(key, "ConnectTimeout")) {
			if(val && cfg.timeout == UNSET) {
				cfg.timeout = strtol(val, &key, 10);
				if(*key != '\0' || cfg.timeout < 0) {
					fprintf(stderr, "error: invalid option to ConnectTimeout: %s\n", val);
					ret = 1;
				}
			}
		} else if(STREQ(key, "Color")) {
			if(cfg.color == UNSET) {
				if(!val || STREQ(val, "auto")) {
					if(isatty(fileno(stdout))) {
						cfg.color = 1;
					} else {
						cfg.color = 0;
					}
				} else if(STREQ(val, "always")) {
					cfg.color = 1;
				} else if(STREQ(val, "never")) {
					cfg.color = 0;
				} else {
					fprintf(stderr, "error: invalid option to Color\n");
					return 1;
				}
			}
		} else {
			fprintf(stderr, "ignoring unknown option: %s\n", key);
		}
		if(ret > 0) {
			goto finish;
		}
	}

finish:
	fclose(fp);
	return ret;
} /* }}} */

int parse_options(int argc, char *argv[]) /* {{{ */
{
	int opt, option_index = 0;

	static const struct option opts[] = {
		/* operations */
		{"download",      no_argument,        0, 'd'},
		{"info",          no_argument,        0, 'i'},
		{"msearch",       no_argument,        0, 'm'},
		{"search",        no_argument,        0, 's'},
		{"update",        no_argument,        0, 'u'},

		/* options */
		{"brief",         no_argument,        0, 'b'},
		{"color",         optional_argument,  0, 'c'},
		{"debug",         no_argument,        0, OP_DEBUG},
		{"force",         no_argument,        0, 'f'},
		{"format",        required_argument,  0, OP_FORMAT},
		{"help",          no_argument,        0, 'h'},
		{"ignore",        required_argument,  0, OP_IGNOREPKG},
		{"ignore-ood",    no_argument,        0, 'o'},
		{"no-ignore-ood", no_argument,        0, OP_NOIGNOREOOD},
		{"ignorerepo",    optional_argument,  0, OP_IGNOREREPO},
		{"listdelim",     required_argument,  0, OP_LISTDELIM},
		{"nossl",         no_argument,        0, OP_NOSSL},
		{"quiet",         no_argument,        0, 'q'},
		{"target",        required_argument,  0, 't'},
		{"threads",       required_argument,  0, OP_THREADS},
		{"timeout",       required_argument,  0, OP_TIMEOUT},
		{"verbose",       no_argument,        0, 'v'},
		{"version",       no_argument,        0, 'V'},
		{0, 0, 0, 0}
	};

	while((opt = getopt_long(argc, argv, "bc::dfhimoqst:uvV", opts, &option_index)) != -1) {
		char *token;

		switch(opt) {
			/* operations */
			case 's':
				cfg.opmask |= OP_SEARCH;
				break;
			case 'u':
				cfg.opmask |= OP_UPDATE;
				break;
			case 'i':
				if(!(cfg.opmask & OP_INFO)) {
					cfg.opmask |= OP_INFO;
				} else {
					cfg.extinfo |= 1;
				}
				break;
			case 'd':
				if(!(cfg.opmask & OP_DOWNLOAD)) {
					cfg.opmask |= OP_DOWNLOAD;
				} else {
					cfg.getdeps |= 1;
				}
				break;
			case 'm':
				cfg.opmask |= OP_MSEARCH;
				break;

			/* options */
			case 'b':
				cfg.logmask |= LOG_BRIEF;
				break;
			case 'c':
				if(!optarg || STREQ(optarg, "auto")) {
					if(isatty(fileno(stdout))) {
						cfg.color = 1;
					} else {
						cfg.color = 0;
					}
				} else if(STREQ(optarg, "always")) {
					cfg.color = 1;
				} else if(STREQ(optarg, "never")) {
					cfg.color = 0;
				} else {
					fprintf(stderr, "invalid argument to --color\n");
					return 1;
				}
				break;
			case 'f':
				cfg.force |= 1;
				break;
			case 'h':
				usage();
				return 1;
			case 'q':
				cfg.quiet |= 1;
				break;
			case 't':
				cfg.dlpath = strdup(optarg);
				break;
			case 'v':
				cfg.logmask |= LOG_VERBOSE;
				break;
			case 'V':
				version();
				return 2;
			case OP_DEBUG:
				cfg.logmask |= LOG_DEBUG;
				break;
			case OP_FORMAT:
				cfg.format = optarg;
				break;
			case 'o':
				cfg.ignoreood = 1;
				break;
			case OP_IGNOREPKG:
				for(token = strtok(optarg, ","); token; token = strtok(NULL, ",")) {
					cwr_printf(LOG_DEBUG, "ignoring package: %s\n", token);
					cfg.ignore.pkgs = alpm_list_add(cfg.ignore.pkgs, strdup(token));
				}
				break;
			case OP_IGNOREREPO:
				if(!optarg) {
					cfg.skiprepos |= 1;
				} else {
					for(token = strtok(optarg, ","); token; token = strtok(NULL, ",")) {
						cwr_printf(LOG_DEBUG, "ignoring repos: %s\n", token);
						cfg.ignore.repos = alpm_list_add(cfg.ignore.repos, strdup(token));
					}
				}
				break;
			case OP_NOIGNOREOOD:
				cfg.ignoreood = 0;
				break;
			case OP_LISTDELIM:
				cfg.delim = optarg;
				break;
			case OP_NOSSL:
				cfg.secure &= 0;
				cfg.proto = "http";
				break;
			case OP_THREADS:
				cfg.maxthreads = strtol(optarg, &token, 10);
				if(*token != '\0' || cfg.maxthreads <= 0) {
					fprintf(stderr, "error: invalid argument to --threads\n");
					return 1;
				}
				break;
			case OP_TIMEOUT:
				cfg.timeout = strtol(optarg, &token, 10);
				if(*token != '\0') {
					fprintf(stderr, "error: invalid argument to --timeout\n");
					return 1;
				}
				break;
			case '?':
			default:
				return 1;
		}
	}

	if(!cfg.opmask) {
		return 3;
	}

#define NOT_EXCL(val) (cfg.opmask & (val) && (cfg.opmask & ~(val)))
	/* check for invalid operation combos */
	if(NOT_EXCL(OP_INFO) || NOT_EXCL(OP_SEARCH) || NOT_EXCL(OP_MSEARCH) ||
			NOT_EXCL(OP_UPDATE|OP_DOWNLOAD)) {
		fprintf(stderr, "error: invalid operation\n");
		return 2;
	}

	while(optind < argc) {
		if(!alpm_list_find_str(cfg.targets, argv[optind])) {
			cwr_fprintf(stderr, LOG_DEBUG, "adding target: %s\n", argv[optind]);
			cfg.targets = alpm_list_add(cfg.targets, strdup(argv[optind]));
		}
		optind++;
	}

	return 0;
} /* }}} */

int pkg_is_binary(const char *pkg) /* {{{ */
{
	const char *db = alpm_provides_pkg(pkg);

	if(db) {
		cwr_fprintf(stderr, LOG_BRIEF, BRIEF_WARN "\t%s\t", pkg);
		cwr_fprintf(stderr, LOG_WARN, "%s%s%s is available in %s%s%s\n",
				colstr->pkg, pkg, colstr->nc,
				colstr->repo, db, colstr->nc);
		return 1;
	}

	return 0;
} /* }}} */

void pkgbuild_get_extinfo(char *pkgbuild, alpm_list_t **details[]) /* {{{ */
{
	char *lineptr;

	for(lineptr = pkgbuild; lineptr; lineptr = strchr(lineptr, '\n')) {
		char *arrayend;
		int depth = 1, type = 0;
		alpm_list_t **deplist;
		size_t linelen;

		linelen = strtrim(++lineptr);
		if(!linelen || *lineptr == '#') {
			continue;
		}

		if(STR_STARTS_WITH(lineptr, PKGBUILD_DEPENDS)) {
			deplist = details[PKGDETAIL_DEPENDS];
		} else if(STR_STARTS_WITH(lineptr, PKGBUILD_MAKEDEPENDS)) {
			deplist = details[PKGDETAIL_MAKEDEPENDS];
		} else if(STR_STARTS_WITH(lineptr, PKGBUILD_OPTDEPENDS)) {
			deplist = details[PKGDETAIL_OPTDEPENDS];
			type = PKGDETAIL_OPTDEPENDS;
		} else if(STR_STARTS_WITH(lineptr, PKGBUILD_PROVIDES)) {
			deplist = details[PKGDETAIL_PROVIDES];
		} else if(STR_STARTS_WITH(lineptr, PKGBUILD_REPLACES)) {
			deplist = details[PKGDETAIL_REPLACES];
		} else if(STR_STARTS_WITH(lineptr, PKGBUILD_CONFLICTS)) {
			deplist = details[PKGDETAIL_CONFLICTS];
		} else {
			continue;
		}

		if(deplist) {
			char *arrayptr = (char*)memchr(lineptr, '(', linelen) + 1;
			for(arrayend = arrayptr; depth; arrayend++) {
				switch(*arrayend) {
					case ')':
						depth--;
						break;
					case '(':
						depth++;
						break;
				}
			}
			*(arrayend - 1) = '\0';
			*deplist = parse_bash_array(*deplist, arrayptr, type);
			lineptr = arrayend;
		}
	}
} /* }}} */

int print_escaped(const char *delim) /* {{{ */
{
	const char *f;
	int out = 0;

	for(f = delim; *f != '\0'; f++) {
		if(*f == '\\') {
			switch(*++f) {
				case '\\':
					fputc('\\', stdout);
					break;
				case '"':
					fputc('\"', stdout);
					break;
				case 'a':
					fputc('\a', stdout);
					break;
				case 'b':
					fputc('\b', stdout);
					break;
				case 'e': /* \e is nonstandard */
					fputc('\033', stdout);
					break;
				case 'n':
					fputc('\n', stdout);
					break;
				case 'r':
					fputc('\r', stdout);
					break;
				case 't':
					fputc('\t', stdout);
					break;
				case 'v':
					fputc('\v', stdout);
					break;
				++out;
			}
		} else {
			fputc(*f, stdout);
			++out;
		}
	}

	return(out);
} /* }}} */

void print_extinfo_list(alpm_list_t *list, const char *fieldname, const char *delim, int wrap) /* {{{ */
{
	const alpm_list_t *next, *i;
	size_t cols, count = 0;

	if(!list) {
		return;
	}

	cols = wrap ? getcols() : 0;

	if(fieldname) {
		count += printf("%-*s: ", INFO_INDENT - 2, fieldname);
	}

	for(i = list; i; i = next) {
		next = alpm_list_next(i);
		if(wrap && cols > 0 && count + strlen(i->data) >= cols) {
			printf("%-*c", INFO_INDENT + 1, '\n');
			count = INFO_INDENT;
		}
		count += printf("%s", (const char*)i->data);
		if(next) {
			count += print_escaped(delim);
		}
	}
	fputc('\n', stdout);
} /* }}} */

void print_pkg_formatted(struct aurpkg_t *pkg) /* {{{ */
{
	const char *p, *end;
	char fmt[32], buf[64];
	int len;

	end = rawmemchr(cfg.format, '\0');

	for(p = cfg.format; p < end; p++) {
		len = 0;
		if(*p == '%') {
			len = strspn(p + 1 + len, printf_flags);
			len += strspn(p + 1 + len, digits);
			snprintf(fmt, len + 3, "%ss", p);
			fmt[len + 1] = 's';
			p += len + 1;
			switch(*p) {
				/* simple attributes */
				case 'a':
					snprintf(buf, 64, "%ld", pkg->lastmod);
					printf(fmt, buf);
					break;
				case 'c':
					printf(fmt, aur_cat[pkg->cat]);
					break;
				case 'd':
					printf(fmt, pkg->desc);
					break;
				case 'i':
					snprintf(buf, 64, "%d", pkg->id);
					printf(fmt, buf);
					break;
				case 'l':
					printf(fmt, pkg->lic);
					break;
				case 'm':
					printf(fmt, pkg->maint ? pkg->maint : "(orphan)");
					break;
				case 'n':
					printf(fmt, pkg->name);
					break;
				case 'o':
					snprintf(buf, 64, "%d", pkg->votes);
					printf(fmt, buf);
					break;
				case 'p':
					snprintf(buf, 64, AUR_PKG_URL_FORMAT "%d", cfg.proto, pkg->id);
					printf(fmt, buf);
					break;
				case 's':
					snprintf(buf, 64, "%ld", pkg->firstsub);
					printf(fmt, buf);
					break;
				case 't':
					printf(fmt, pkg->ood ? "yes" : "no");
					break;
				case 'u':
					printf(fmt, pkg->url);
					break;
				case 'v':
					printf(fmt, pkg->ver);
					break;
				/* list based attributes */
				case 'C':
					print_extinfo_list(pkg->conflicts, NULL, cfg.delim, 0);
					break;
				case 'D':
					print_extinfo_list(pkg->depends, NULL, cfg.delim, 0);
					break;
				case 'M':
					print_extinfo_list(pkg->makedepends, NULL, cfg.delim, 0);
					break;
				case 'O':
					print_extinfo_list(pkg->optdepends, NULL, cfg.delim, 0);
					break;
				case 'P':
					print_extinfo_list(pkg->provides, NULL, cfg.delim, 0);
					break;
				case 'R':
					print_extinfo_list(pkg->replaces, NULL, cfg.delim, 0);
					break;
				case '%':
					fputc('%', stdout);
					break;
				default:
					fputc('?', stdout);
					break;
			}
		} else if(*p == '\\') {
			char ebuf[3];
			ebuf[0] = *p;
			ebuf[1] = *++p;
			ebuf[2] = '\0';
			print_escaped(ebuf);
		} else {
			fputc(*p, stdout);
		}
	}

	fputc('\n', stdout);

	return;
} /* }}} */

void print_pkg_info(struct aurpkg_t *pkg) /* {{{ */
{
	char datestring[42];
	struct tm *ts;
	alpm_pkg_t *ipkg;

	printf(PKG_REPO "     : %saur%s\n", colstr->repo, colstr->nc);
	printf(NAME "           : %s%s%s", colstr->pkg, pkg->name, colstr->nc);
	if((ipkg = alpm_db_get_pkg(db_local, pkg->name))) {
		const char *instcolor;
		if(alpm_pkg_vercmp(pkg->ver, alpm_pkg_get_version(ipkg)) > 0) {
			instcolor = colstr->ood;
		} else {
			instcolor = colstr->utd;
		}
		printf(" %s[%sinstalled%s]%s", colstr->url, instcolor, colstr->url, colstr->nc);
	}
	fputc('\n', stdout);

	printf(VERSION "        : %s%s%s\n",
			pkg->ood ? colstr->ood : colstr->utd, pkg->ver, colstr->nc);
	printf(URL "            : %s%s%s\n", colstr->url, pkg->url, colstr->nc);
	printf(PKG_AURPAGE "       : %s" AUR_PKG_URL_FORMAT "%d%s\n",
			colstr->url, cfg.proto, pkg->id, colstr->nc);

	print_extinfo_list(pkg->depends, PKG_DEPENDS, LIST_DELIM, 1);
	print_extinfo_list(pkg->makedepends, PKG_MAKEDEPENDS, LIST_DELIM, 1);
	print_extinfo_list(pkg->provides, PKG_PROVIDES, LIST_DELIM, 1);
	print_extinfo_list(pkg->conflicts, PKG_CONFLICTS, LIST_DELIM, 1);

	if(pkg->optdepends) {
		const alpm_list_t *i;
		printf(PKG_OPTDEPENDS "  : %s\n", (const char*)pkg->optdepends->data);
		for(i = pkg->optdepends->next; i; i = alpm_list_next(i)) {
			printf("%-*s%s\n", INFO_INDENT, "", (const char*)i->data);
		}
	}

	print_extinfo_list(pkg->replaces, PKG_REPLACES, LIST_DELIM, 1);

	printf(PKG_CAT "       : %s\n"
				 PKG_LICENSE "        : %s\n"
				 PKG_NUMVOTES "          : %d\n"
				 PKG_OOD "    : %s%s%s\n",
				 aur_cat[pkg->cat], pkg->lic, pkg->votes,
				 pkg->ood ? colstr->ood : colstr->utd,
				 pkg->ood ? "Yes" : "No", colstr->nc);

	printf(PKG_MAINT "     : %s\n", pkg->maint ? pkg->maint : "(orphan)");

	ts = localtime(&pkg->firstsub);
	strftime(datestring, 42, PKG_TIMEFMT, ts);
	printf(PKG_FIRSTSUB "      : %s\n", datestring);

	ts = localtime(&pkg->lastmod);
	strftime(datestring, 42, PKG_TIMEFMT, ts);
	printf(PKG_LASTMOD "  : %s\n", datestring);

	printf(PKG_DESC "    : ");
	indentprint(pkg->desc, INFO_INDENT);
	printf("\n\n");
} /* }}} */

void print_pkg_search(struct aurpkg_t *pkg) /* {{{ */
{
	if(cfg.quiet) {
		printf("%s%s%s\n", colstr->pkg, pkg->name, colstr->nc);
	} else {
		alpm_pkg_t *ipkg;
		printf("%saur/%s%s%s %s%s%s%s (%d)", colstr->repo, colstr->nc, colstr->pkg,
				pkg->name, pkg->ood ? colstr->ood : colstr->utd, pkg->ver,
				NCFLAG(pkg->ood, " <!>"), colstr->nc, pkg->votes);
		if((ipkg = alpm_db_get_pkg(db_local, pkg->name))) {
			const char *instcolor;
			if(alpm_pkg_vercmp(pkg->ver, alpm_pkg_get_version(ipkg)) > 0) {
				instcolor = colstr->ood;
			} else {
				instcolor = colstr->utd;
			}
			printf(" %s[%sinstalled%s]%s", colstr->url, instcolor, colstr->url, colstr->nc);
		}
		printf("\n    ");
		indentprint(pkg->desc, SRCH_INDENT);
		fputc('\n', stdout);
	}
} /* }}} */

void print_results(alpm_list_t *results, void (*printfn)(struct aurpkg_t*)) /* {{{ */
{
	const alpm_list_t *i;
	struct aurpkg_t *prev = NULL;

	if(!printfn) {
		return;
	}

	if(!results && (cfg.opmask & OP_INFO)) {
		cwr_fprintf(stderr, LOG_ERROR, "no results found\n");
		return;
	}

	for(i = results; i; i = alpm_list_next(i)) {
		struct aurpkg_t *pkg = i->data;

		/* don't print duplicates */
		if(!prev || aurpkg_cmp(pkg, prev) != 0) {
			printfn(pkg);
		}
		prev = pkg;
	}
} /* }}} */

int resolve_dependencies(CURL *curl, const char *pkgname, const char *subdir) /* {{{ */
{
	const alpm_list_t *i;
	alpm_list_t *deplist = NULL;
	char *filename, *pkgbuild;
	void *retval;

	curl = curl_init_easy_handle(curl);

	cwr_asprintf(&filename, "%s/%s/PKGBUILD", cfg.dlpath, subdir ? subdir : pkgname);

	pkgbuild = get_file_as_buffer(filename);
	if(!pkgbuild) {
		return 1;
	}

	alpm_list_t **pkg_details[PKGDETAIL_MAX] = {
		&deplist, &deplist, NULL, NULL, NULL, NULL
	};

	cwr_printf(LOG_DEBUG, "Parsing %s for extended info\n", filename);
	pkgbuild_get_extinfo(pkgbuild, pkg_details);
	free(pkgbuild);
	free(filename);

	for(i = deplist; i; i = alpm_list_next(i)) {
		const char *depend = i->data;
		char *sanitized = strdup(depend);

		sanitized[strcspn(sanitized, "<>=")] = '\0';

		if(!alpm_list_find_str(cfg.targets, sanitized)) {
			pthread_mutex_lock(&listlock);
			cfg.targets = alpm_list_add(cfg.targets, sanitized);
			pthread_mutex_unlock(&listlock);
		} else {
			if(cfg.logmask & LOG_BRIEF &&
							!alpm_find_satisfier(alpm_db_get_pkgcache(db_local), depend)) {
					cwr_printf(LOG_BRIEF, "S\t%s\n", sanitized);
			}
			FREE(sanitized);
		}

		if(sanitized) {
			if(alpm_find_satisfier(alpm_db_get_pkgcache(db_local), depend)) {
				cwr_printf(LOG_DEBUG, "%s is already satisified\n", depend);
			} else {
				if(!pkg_is_binary(depend)) {
					retval = task_download(curl, sanitized);
					alpm_list_free_inner(retval, aurpkg_free);
					alpm_list_free(retval);
				}
			}
		}
	}

	FREELIST(deplist);

	return 0;
} /* }}} */

int set_working_dir(void) /* {{{ */
{
	char *resolved;

	if(!(cfg.opmask & OP_DOWNLOAD)) {
		FREE(cfg.dlpath);
		return 0;
	}

	resolved = cfg.dlpath ? realpath(cfg.dlpath, NULL) : getcwd(NULL, 0);
	if(!resolved) {
		cwr_fprintf(stderr, LOG_ERROR, "%s: %s\n", cfg.dlpath, strerror(errno));
		FREE(cfg.dlpath);
		return 1;
	}

	free(cfg.dlpath);
	cfg.dlpath = resolved;

	if(access(cfg.dlpath, W_OK) != 0) {
		cwr_fprintf(stderr, LOG_ERROR, "cannot write to %s: %s\n",
				cfg.dlpath, strerror(errno));
		FREE(cfg.dlpath);
		return 1;
	}

	if(chdir(cfg.dlpath) != 0) {
		cwr_fprintf(stderr, LOG_ERROR, "%s: %s\n", cfg.dlpath, strerror(errno));
		return 1;
	}

	cwr_printf(LOG_DEBUG, "working directory set to: %s\n", cfg.dlpath);

	return 0;
} /* }}} */

int strings_init(void) /* {{{ */
{
	MALLOC(colstr, sizeof(struct strings_t), return 1);

	if(cfg.color > 0) {
		colstr->error = BOLDRED "::" NC;
		colstr->warn = BOLDYELLOW "::" NC;
		colstr->info = BOLDBLUE "::" NC;
		colstr->pkg = BOLD;
		colstr->repo = BOLDMAGENTA;
		colstr->url = BOLDCYAN;
		colstr->ood = BOLDRED;
		colstr->utd = BOLDGREEN;
		colstr->nc = NC;
	} else {
		colstr->error = "error:";
		colstr->warn = "warning:";
		colstr->info = "::";
		colstr->pkg = "";
		colstr->repo = "";
		colstr->url = "";
		colstr->ood = "";
		colstr->utd = "";
		colstr->nc = "";
	}

	/* guard against delim being something other than LIST_DELIM if extinfo
	 * and format aren't provided */
	cfg.delim = (cfg.extinfo && cfg.format) ? cfg.delim : LIST_DELIM;

	return 0;
} /* }}} */

size_t strtrim(char *str) /* {{{ */
{
	char *left = str, *right;

	if(!str || *str == '\0') {
		return 0;
	}

	while(isspace((unsigned char)*left)) {
		left++;
	}
	if(left != str) {
		memmove(str, left, (strlen(left) + 1));
	}

	if(*str == '\0') {
		return 0;
	}

	right = (char*)rawmemchr(str, '\0') - 1;
	while(isspace((unsigned char)*right)) {
		right--;
	}
	*++right = '\0';

	return right - left;
} /* }}} */

void *task_download(CURL *curl, void *arg) /* {{{ */
{
	if(pkg_is_binary(arg)) {
		return NULL;
	} else {
		return download(curl, arg);
	}
} /* }}} */

void *task_query(CURL *curl, void *arg) /* {{{ */
{
	alpm_list_t *pkglist = NULL;
	CURLcode curlstat;
	struct yajl_handle_t *yajl_hand = NULL;
	const char *argstr;
	char *escaped, *url;
	long httpcode;
	int span = 0;
	struct yajl_parser_t *parse_struct;

	/* find a valid chunk of search string */
	if(cfg.opmask & OP_SEARCH) {
		for(argstr = arg; *argstr; argstr++) {
			span = strcspn(argstr, REGEX_CHARS);

			/* given 'cow?', we can't include w in the search */
			if(argstr[span] == '?' || argstr[span] == '*') {
				span--;
			}

			/* a string inside [] or {} cannot be a valid span */
			if(strchr("[{", *argstr)) {
				argstr = strpbrk(argstr + span, "]}");
				if(!argstr) {
					cwr_fprintf(stderr, LOG_ERROR, "invalid regular expression: %s\n", (const char*)arg);
					return NULL;
				}
				continue;
			}

			if(span >= 2) {
				break;
			}
		}

		if(span < 2) {
			cwr_fprintf(stderr, LOG_ERROR, "search string '%s' too short\n", (const char*)arg);
			return NULL;
		}
	} else {
		argstr = arg;
	}

	CALLOC(parse_struct, 1, sizeof(struct yajl_parser_t), return NULL);
	CALLOC(parse_struct->aurpkg, 1, sizeof(struct aurpkg_t), return NULL);
	yajl_hand = yajl_alloc(&callbacks, NULL, (void*)parse_struct);

	curl = curl_init_easy_handle(curl);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, yajl_parse_stream);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, yajl_hand);

	escaped = url_escape((char*)argstr, span, NULL);
	if(cfg.opmask & OP_SEARCH) {
		cwr_asprintf(&url, AUR_RPC_URL, cfg.proto, AUR_QUERY_TYPE_SEARCH, escaped);
	} else if(cfg.opmask & OP_MSEARCH) {
		cwr_asprintf(&url, AUR_RPC_URL, cfg.proto, AUR_QUERY_TYPE_MSRCH, escaped);
	} else {
		cwr_asprintf(&url, AUR_RPC_URL, cfg.proto, AUR_QUERY_TYPE_INFO, escaped);
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);

	cwr_printf(LOG_DEBUG, "[%s]: curl_easy_perform %s\n", (const char *)arg, url);
	curlstat = curl_easy_perform(curl);

	if(curlstat != CURLE_OK) {
		cwr_fprintf(stderr, LOG_ERROR, "[%s]: %s\n", (const char*)arg,
				curl_easy_strerror(curlstat));
		goto finish;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpcode);
	cwr_printf(LOG_DEBUG, "[%s]: server responded with %ld\n", (const char *)arg, httpcode);
	if(httpcode >= 400) {
		cwr_fprintf(stderr, LOG_ERROR, "[%s]: server responded with HTTP %ld\n",
				(const char*)arg, httpcode);
		goto finish;
	}

	yajl_complete_parse(yajl_hand);

	pkglist = parse_struct->pkglist;

	if(pkglist && cfg.extinfo) {
		struct aurpkg_t *aurpkg;
		char *pburl, *escaped, *pkgbuild;

		aurpkg = pkglist->data;
		escaped = url_escape(aurpkg->urlpath, 0, "/");
		cwr_asprintf(&pburl, AUR_BASE_URL, cfg.proto, escaped);
		memcpy(strrchr(pburl, '/') + 1, "PKGBUILD\0", 9);

		pkgbuild = curl_get_url_as_buffer(curl, pburl);
		free(escaped);
		free(pburl);

		alpm_list_t **pkg_details[PKGDETAIL_MAX] = {
			&aurpkg->depends, &aurpkg->makedepends, &aurpkg->optdepends,
			&aurpkg->provides, &aurpkg->conflicts, &aurpkg->replaces
		};

		pkgbuild_get_extinfo(pkgbuild, pkg_details);
		free(pkgbuild);
	}

finish:
	yajl_free(yajl_hand);
	curl_free(escaped);
	FREE(parse_struct->aurpkg);
	FREE(parse_struct);
	FREE(url);

	return pkglist;
} /* }}} */

void *task_update(CURL *curl, void *arg) /* {{{ */
{
	alpm_pkg_t *pmpkg;
	struct aurpkg_t *aurpkg;
	void *dlretval, *qretval;
	const char *candidate = arg;

	cwr_printf(LOG_VERBOSE, "Checking %s%s%s for updates...\n",
			colstr->pkg, candidate, colstr->nc);

	qretval = task_query(curl, arg);
	aurpkg = qretval ? ((alpm_list_t*)qretval)->data : NULL;
	if(aurpkg) {

		pmpkg = alpm_db_get_pkg(db_local, arg);

		if(!pmpkg) {
			cwr_fprintf(stderr, LOG_WARN, "skipping uninstalled package %s\n",
					candidate);
			goto finish;
		}

		if(alpm_pkg_vercmp(aurpkg->ver, alpm_pkg_get_version(pmpkg)) > 0) {
			if(alpm_list_find_str(cfg.ignore.pkgs, arg)) {
				if(!cfg.quiet && !(cfg.logmask & LOG_BRIEF)) {
					cwr_fprintf(stderr, LOG_WARN, "%s%s%s [ignored] %s%s%s -> %s%s%s\n",
							colstr->pkg, candidate, colstr->nc,
							colstr->ood, alpm_pkg_get_version(pmpkg), colstr->nc,
							colstr->utd, aurpkg->ver, colstr->nc);
				}
				return NULL;
			}

			if(cfg.opmask & OP_DOWNLOAD) {
				/* we don't care about the return, but we do care about leaks */
				dlretval = task_download(curl, (void*)aurpkg->name);
				alpm_list_free_inner(dlretval, aurpkg_free);
				alpm_list_free(dlretval);
			} else {
				if(cfg.quiet) {
					printf("%s%s%s\n", colstr->pkg, candidate, colstr->nc);
				} else {
					cwr_printf(LOG_INFO, "%s%s %s%s%s -> %s%s%s\n",
							colstr->pkg, candidate,
							colstr->ood, alpm_pkg_get_version(pmpkg), colstr->nc,
							colstr->utd, aurpkg->ver, colstr->nc);
				}
			}

			return qretval;
		}
	}

finish:
	alpm_list_free_inner(qretval, aurpkg_free);
	alpm_list_free(qretval);
	return NULL;
} /* }}} */

void *thread_pool(void *arg) /* {{{ */
{
	alpm_list_t *ret = NULL;
	CURL *curl;
	void *job;
	struct task_t *task;

	task = (struct task_t*)arg;
	curl = curl_easy_init();
	if(!curl) {
		cwr_fprintf(stderr, LOG_ERROR, "curl: failed to initialize handle\n");
		return NULL;
	}

	while(1) {
		job = NULL;

		/* try to pop off the work queue */
		pthread_mutex_lock(&listlock);
		if(workq) {
			job = workq->data;
			workq = alpm_list_next(workq);
		}
		pthread_mutex_unlock(&listlock);

		/* make sure we hooked a new job */
		if(!job) {
			break;
		}

		ret = alpm_list_join(ret, task->threadfn(curl, job));
	}

	curl_easy_cleanup(curl);

	return ret;
} /* }}} */

static char *url_escape(char *in, int len, const char *delim) /* {{{ */
{
	char *tok, *escaped;
	char buf[128] = { 0 };

	if(!delim) {
		return curl_easy_escape(NULL, in, len);
	}

	while((tok = strsep(&in, delim))) {
		escaped = curl_easy_escape(NULL, tok, 0);
		strcat(buf, escaped);
		curl_free(escaped);
		strcat(buf, delim);
	}

	return strndup(buf, strlen(buf) - 1);
} /* }}} */

void usage(void) /* {{{ */
{
	fprintf(stderr, "cower %s\n"
	    "Usage: cower <operations> [options] target...\n\n", COWER_VERSION);
	fprintf(stderr,
	    " Operations:\n"
	    "  -d, --download          download target(s) -- pass twice to "
	                                 "download AUR dependencies\n"
	    "  -i, --info              show info for target(s) -- pass twice for "
	                                 "more detail\n"
	    "  -m, --msearch           show packages maintained by target(s)\n"
	    "  -s, --search            search for target(s)\n"
	    "  -u, --update            check for updates against AUR -- can be combined "
	                                 "with the -d flag\n\n");
	fprintf(stderr, " General options:\n"
	    "  -f, --force             overwrite existing files when downloading\n"
	    "  -h, --help              display this help and exit\n"
	    "      --ignore <pkg>      ignore a package upgrade (can be used more than once)\n"
	    "      --ignorerepo <repo> ignore some or all binary repos\n"
	    "      --nossl             do not use https connections\n"
	    "  -t, --target <dir>      specify an alternate download directory\n"
	    "      --threads <num>     limit number of threads created\n"
	    "      --timeout <num>     specify connection timeout in seconds\n"
	    "  -V, --version           display version\n\n");
	fprintf(stderr, " Output options:\n"
	    "  -b, --brief             show output in a more script friendly format\n"
	    "  -c, --color[=WHEN]      use colored output. WHEN is `never', `always', or `auto'\n"
	    "      --debug             show debug output\n"
	    "      --format <string>   print package output according to format string\n"
	    "  -o, --ignore-ood        skip displaying out of date packages\n"
	    "      --no-ignore-ood     the opposite of --ignore-ood\n"
	    "      --listdelim <delim> change list format delimeter\n"
	    "  -q, --quiet             output less\n"
	    "  -v, --verbose           output more\n\n");
} /* }}} */

void version(void) /* {{{ */
{
	printf("\n  " COWER_VERSION "\n");
	printf("     \\\n"
	       "      \\\n"
	       "        ,__, |    |\n"
	       "        (oo)\\|    |___\n"
	       "        (__)\\|    |   )\\_\n"
	       "          U  |    |_w |  \\\n"
	       "             |    |  ||   *\n"
	       "\n"
	       "             Cower....\n\n");
} /* }}} */

size_t yajl_parse_stream(void *ptr, size_t size, size_t nmemb, void *stream) /* {{{ */
{
	struct yajl_handle_t *hand;
	size_t realsize = size * nmemb;

	hand = (struct yajl_handle_t*)stream;
	yajl_parse(hand, ptr, realsize);

	return realsize;
} /* }}} */

int main(int argc, char *argv[]) {
	alpm_list_t *results = NULL, *thread_return = NULL;
	int ret, n, num_threads;
	pthread_attr_t attr;
	pthread_t *threads;
	struct task_t task = {
		.printfn = NULL,
		.threadfn = task_query
	};

	setlocale(LC_ALL, "");

	/* initialize config */
	cfg.color = cfg.maxthreads = cfg.timeout = UNSET;
	cfg.delim = LIST_DELIM;
	cfg.logmask = LOG_ERROR|LOG_WARN|LOG_INFO;
	cfg.secure |= 1;
	cfg.proto = "https";
	cfg.ignoreood = UNSET;

	ret = parse_options(argc, argv);
	switch(ret) {
		case 0: /* everything's cool */
			break;
		case 3:
			fprintf(stderr, "error: no operation specified (use -h for help)\n");
		case 1: /* these provide their own error mesg */
		case 2:
			return ret;
	}

	if((ret = parse_configfile() != 0)) {
		return ret;
	}

	/* fallback from sentinel values */
	cfg.maxthreads = cfg.maxthreads == UNSET ? THREAD_DEFAULT : cfg.maxthreads;
	cfg.timeout = cfg.timeout == UNSET ? TIMEOUT_DEFAULT : cfg.timeout;
	cfg.color = cfg.color == UNSET ? 0 : cfg.color;
	cfg.ignoreood = cfg.ignoreood == UNSET ? 0 : cfg.ignoreood;

	if((ret = strings_init()) != 0) {
		return ret;
	}

	if((ret = set_working_dir()) != 0) {
		goto finish;
	}

	cwr_printf(LOG_DEBUG, "initializing curl\n");
	if(cfg.secure) {
		ret = curl_global_init(CURL_GLOBAL_SSL);
		openssl_crypto_init();
	} else {
		ret = curl_global_init(CURL_GLOBAL_NOTHING);
	}
	if(ret != 0) {
		cwr_fprintf(stderr, LOG_ERROR, "failed to initialize curl\n");
		goto finish;
	}

	pmhandle = alpm_init();
	if(!pmhandle) {
		cwr_fprintf(stderr, LOG_ERROR, "failed to initialize alpm library\n");
		goto finish;
	}

	/* allow specific updates to be provided instead of examining all foreign pkgs */
	if((cfg.opmask & OP_UPDATE) && !cfg.targets) {
		cfg.targets = alpm_find_foreign_pkgs();
	}

	workq = cfg.targets;
	num_threads = alpm_list_count(cfg.targets);
	if(num_threads == 0) {
		fprintf(stderr, "error: no targets specified (use -h for help)\n");
		goto finish;
	} else if(num_threads > cfg.maxthreads) {
		num_threads = cfg.maxthreads;
	}

	CALLOC(threads, num_threads, sizeof(pthread_t), goto finish);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	/* override task behavior */
	if(cfg.opmask & OP_UPDATE) {
		task.threadfn = task_update;
	} else if(cfg.opmask & OP_INFO) {
		task.printfn = cfg.format ? print_pkg_formatted : print_pkg_info;
	} else if(cfg.opmask & (OP_SEARCH|OP_MSEARCH)) {
		task.printfn = cfg.format ? print_pkg_formatted : print_pkg_search;
	} else if(cfg.opmask & OP_DOWNLOAD) {
		task.threadfn = task_download;
	}

	/* filthy, filthy hack: prepopulate the package cache */
	alpm_db_get_pkgcache(db_local);

	for(n = 0; n < num_threads; n++) {
		ret = pthread_create(&threads[n], &attr, thread_pool, &task);
		if(ret != 0) {
			cwr_fprintf(stderr, LOG_ERROR, "failed to spawn new thread: %s\n",
					strerror(ret));
			return(ret); /* we don't want to recover from this */
		}
	}

	for(n = 0; n < num_threads; n++) {
		pthread_join(threads[n], (void**)&thread_return);
		results = alpm_list_join(results, thread_return);
	}

	free(threads);
	pthread_attr_destroy(&attr);

	/* we need to exit with a non-zero value when:
	 * a) search/info/download returns nothing
	 * b) update (without download) returns something
	 * this is opposing behavior, so just XOR the result on a pure update */
	results = filter_results(results);
	ret = ((results == NULL) ^ !(cfg.opmask & ~OP_UPDATE));
	print_results(results, task.printfn);
	alpm_list_free_inner(results, aurpkg_free);
	alpm_list_free(results);

	openssl_crypto_cleanup();

finish:
	FREE(cfg.dlpath);
	FREELIST(cfg.targets);
	FREELIST(cfg.ignore.pkgs);
	FREELIST(cfg.ignore.repos);
	FREE(colstr);

	cwr_printf(LOG_DEBUG, "releasing curl\n");
	curl_global_cleanup();

	cwr_printf(LOG_DEBUG, "releasing alpm\n");
	alpm_release(pmhandle);

	return ret;
}

/* vim: set noet ts=2 sw=2: */
