#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <git2.h>

#include "compat.h"

#ifdef WITH_MD4C
#include <md4c-html.h>

/* Buffer for capturing md4c output */
struct md_buffer {
    char *data;
    size_t size;
    size_t capacity;
};

/* md4c output callback for buffer */
static void md4c_buffer_cb(const MD_CHAR *data, MD_SIZE size, void *ud)
{
    struct md_buffer *buf = (struct md_buffer *)ud;
    size_t needed = buf->size + size;
    
    if (needed > buf->capacity) {
        size_t new_cap = buf->capacity ? buf->capacity * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        buf->data = realloc(buf->data, new_cap);
        if (!buf->data) err(1, "realloc");
        buf->capacity = new_cap;
    }
    
    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
}

/* Convert relative .md links to .md.html in HTML */
static void convert_md_links(FILE *fp, const char *html, size_t len)
{
    size_t i = 0;
    while (i < len) {
        /* Look for href=" */
        if (i + 6 < len && 
            html[i] == 'h' && html[i+1] == 'r' && html[i+2] == 'e' && 
            html[i+3] == 'f' && html[i+4] == '=' && html[i+5] == '"') {
            
            fwrite(&html[i], 1, 6, fp); /* write href=" */
            i += 6;
            
            /* Capture the URL until the closing " */
            size_t url_start = i;
            while (i < len && html[i] != '"') i++;
            size_t url_len = i - url_start;
            
            /* Check if it's a relative .md link */
            if (url_len > 3 && 
                html[url_start] != '/' && 
                html[url_start] != 'h' && /* not http */
                html[url_start] != '#') { /* not anchor */
                
                /* Check for .md extension */
                const char *url = &html[url_start];
                const char *ext = NULL;
                for (size_t j = 0; j < url_len; j++) {
                    if (url[j] == '.') ext = &url[j];
                }
                
                if (ext && url_len - (ext - url) >= 3 && url_len - (ext - url) <= 9) {
                    /* Check various markdown extensions */
                    int is_md = 0;
                    if (!strncasecmp(ext, ".md\"", 4) || 
                        !strncasecmp(ext, ".md#", 4) ||
                        !strncasecmp(ext, ".markdown\"", 10) ||
                        !strncasecmp(ext, ".markdown#", 10)) {
                        is_md = 1;
                    }
                    
                    if (is_md) {
                        /* Write URL up to the extension */
                        fwrite(url, 1, ext - url, fp);
                        /* Write extension + .html */
                        size_t ext_len = 0;
                        while (ext[ext_len] && ext[ext_len] != '"' && ext[ext_len] != '#') 
                            ext_len++;
                        fwrite(ext, 1, ext_len, fp);
                        fputs(".html", fp);
                        /* Write rest of URL (anchor, etc) */
                        fwrite(&ext[ext_len], 1, url_len - (ext - url) - ext_len, fp);
                    } else {
                        fwrite(url, 1, url_len, fp);
                    }
                } else {
                    fwrite(url, 1, url_len, fp);
                }
            } else {
                fwrite(&html[url_start], 1, url_len, fp);
            }
        } else {
            fputc(html[i], fp);
            i++;
        }
    }
}

/* 拡張子で Markdown 判定（大小無視） */
static int is_markdown_filename(const char *name)
{
    if (!name) return 0;
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    return !strcasecmp(ext, ".md")
        || !strcasecmp(ext, ".markdown")
        || !strcasecmp(ext, ".mdown")
        || !strcasecmp(ext, ".mkd");
}

/* Markdown → HTML with link conversion */
static int render_markdown(FILE *fp, const char *buf, size_t len)
{
    struct md_buffer output = {0};
    unsigned parser_flags = MD_DIALECT_GITHUB;
#ifdef STAGIT_MD_NOHTML
    parser_flags |= MD_FLAG_NOHTML;
#endif
    unsigned renderer_flags = 0;
    
    /* Render to buffer first */
    int ret = md_html((const MD_CHAR*)buf, (MD_SIZE)len, md4c_buffer_cb, &output,
                      parser_flags, renderer_flags);
    
    if (ret == 0 && output.data && output.size > 0) {
        /* Convert .md links to .md.html and write to file */
        convert_md_links(fp, output.data, output.size);
    }
    
    free(output.data);
    return ret;
}
#endif /* WITH_MD4C */

struct deltainfo {
	git_patch *patch;

	size_t addcount;
	size_t delcount;
};

struct commitinfo {
	const git_oid *id;

	char oid[GIT_OID_HEXSZ + 1];
	char parentoid[GIT_OID_HEXSZ + 1];

	const git_signature *author;
	const git_signature *committer;
	const char          *summary;
	const char          *msg;

	git_diff   *diff;
	git_commit *commit;
	git_commit *parent;
	git_tree   *commit_tree;
	git_tree   *parent_tree;

	size_t addcount;
	size_t delcount;
	size_t filecount;

	struct deltainfo **deltas;
	size_t ndeltas;
};

/* reference and associated data for sorting */
struct referenceinfo {
	struct git_reference *ref;
	struct commitinfo *ci;
};

static git_repository *repo;

static const char *relpath = "";
static const char *repodir;

static char *name = "";
static char *strippedname = "";
static char description[255];
static char cloneurl[1024];
static char *submodules;
static char *licensefiles[] = { "HEAD:LICENSE", "HEAD:LICENSE.md", "HEAD:COPYING" };
static char *license;
static char *readmefiles[] = { "HEAD:README", "HEAD:README.md" };
static char *readme;
static long long nlogcommits = -1; /* < 0 indicates not used */

/* cache */
static git_oid lastoid;
static char lastoidstr[GIT_OID_HEXSZ + 2]; /* id + newline + NUL byte */
static FILE *rcachefp, *wcachefp;
static const char *cachefile;

void
joinpath(char *buf, size_t bufsiz, const char *path, const char *path2)
{
	int r;

	r = snprintf(buf, bufsiz, "%s%s%s",
		path, path[0] && path[strlen(path) - 1] != '/' ? "/" : "", path2);
	if (r < 0 || (size_t)r >= bufsiz)
		errx(1, "path truncated: '%s%s%s'",
			path, path[0] && path[strlen(path) - 1] != '/' ? "/" : "", path2);
}

void
deltainfo_free(struct deltainfo *di)
{
	if (!di)
		return;
	git_patch_free(di->patch);
	memset(di, 0, sizeof(*di));
	free(di);
}

int
commitinfo_getstats(struct commitinfo *ci)
{
	struct deltainfo *di;
	git_diff_options opts;
	git_diff_find_options fopts;
	const git_diff_delta *delta;
	const git_diff_hunk *hunk;
	const git_diff_line *line;
	git_patch *patch = NULL;
	size_t ndeltas, nhunks, nhunklines;
	size_t i, j, k;

	if (git_tree_lookup(&(ci->commit_tree), repo, git_commit_tree_id(ci->commit)))
		goto err;
	if (!git_commit_parent(&(ci->parent), ci->commit, 0)) {
		if (git_tree_lookup(&(ci->parent_tree), repo, git_commit_tree_id(ci->parent))) {
			ci->parent = NULL;
			ci->parent_tree = NULL;
		}
	}

	git_diff_init_options(&opts, GIT_DIFF_OPTIONS_VERSION);
	opts.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH |
	              GIT_DIFF_IGNORE_SUBMODULES |
		      GIT_DIFF_INCLUDE_TYPECHANGE;
	if (git_diff_tree_to_tree(&(ci->diff), repo, ci->parent_tree, ci->commit_tree, &opts))
		goto err;

	if (git_diff_find_init_options(&fopts, GIT_DIFF_FIND_OPTIONS_VERSION))
		goto err;
	/* find renames and copies, exact matches (no heuristic) for renames. */
	fopts.flags |= GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES |
	               GIT_DIFF_FIND_EXACT_MATCH_ONLY;
	if (git_diff_find_similar(ci->diff, &fopts))
		goto err;

	ndeltas = git_diff_num_deltas(ci->diff);
	if (ndeltas && !(ci->deltas = calloc(ndeltas, sizeof(struct deltainfo *))))
		err(1, "calloc");

	for (i = 0; i < ndeltas; i++) {
		if (git_patch_from_diff(&patch, ci->diff, i))
			goto err;

		if (!(di = calloc(1, sizeof(struct deltainfo))))
			err(1, "calloc");
		di->patch = patch;
		ci->deltas[i] = di;

		delta = git_patch_get_delta(patch);

		/* skip stats for binary data */
		if (delta->flags & GIT_DIFF_FLAG_BINARY)
			continue;

		nhunks = git_patch_num_hunks(patch);
		for (j = 0; j < nhunks; j++) {
			if (git_patch_get_hunk(&hunk, &nhunklines, patch, j))
				break;
			for (k = 0; ; k++) {
				if (git_patch_get_line_in_hunk(&line, patch, j, k))
					break;
				if (line->old_lineno == -1) {
					di->addcount++;
					ci->addcount++;
				} else if (line->new_lineno == -1) {
					di->delcount++;
					ci->delcount++;
				}
			}
		}
	}
	ci->ndeltas = i;
	ci->filecount = i;

	return 0;

err:
	git_diff_free(ci->diff);
	ci->diff = NULL;
	git_tree_free(ci->commit_tree);
	ci->commit_tree = NULL;
	git_tree_free(ci->parent_tree);
	ci->parent_tree = NULL;
	git_commit_free(ci->parent);
	ci->parent = NULL;

	if (ci->deltas)
		for (i = 0; i < ci->ndeltas; i++)
			deltainfo_free(ci->deltas[i]);
	free(ci->deltas);
	ci->deltas = NULL;
	ci->ndeltas = 0;
	ci->addcount = 0;
	ci->delcount = 0;
	ci->filecount = 0;

	return -1;
}

void
commitinfo_free(struct commitinfo *ci)
{
	size_t i;

	if (!ci)
		return;
	if (ci->deltas)
		for (i = 0; i < ci->ndeltas; i++)
			deltainfo_free(ci->deltas[i]);

	free(ci->deltas);
	git_diff_free(ci->diff);
	git_tree_free(ci->commit_tree);
	git_tree_free(ci->parent_tree);
	git_commit_free(ci->commit);
	git_commit_free(ci->parent);
	memset(ci, 0, sizeof(*ci));
	free(ci);
}

struct commitinfo *
commitinfo_getbyoid(const git_oid *id)
{
	struct commitinfo *ci;

	if (!(ci = calloc(1, sizeof(struct commitinfo))))
		err(1, "calloc");

	if (git_commit_lookup(&(ci->commit), repo, id))
		goto err;
	ci->id = id;

	git_oid_tostr(ci->oid, sizeof(ci->oid), git_commit_id(ci->commit));
	git_oid_tostr(ci->parentoid, sizeof(ci->parentoid), git_commit_parent_id(ci->commit, 0));

	ci->author = git_commit_author(ci->commit);
	ci->committer = git_commit_committer(ci->commit);
	ci->summary = git_commit_summary(ci->commit);
	ci->msg = git_commit_message(ci->commit);

	return ci;

err:
	commitinfo_free(ci);

	return NULL;
}

int
refs_cmp(const void *v1, const void *v2)
{
	struct referenceinfo *r1 = (struct referenceinfo *)v1;
	struct referenceinfo *r2 = (struct referenceinfo *)v2;
	time_t t1, t2;
	int r;

	if ((r = git_reference_is_tag(r1->ref) - git_reference_is_tag(r2->ref)))
		return r;

	t1 = r1->ci->author ? r1->ci->author->when.time : 0;
	t2 = r2->ci->author ? r2->ci->author->when.time : 0;
	if ((r = t1 > t2 ? -1 : (t1 == t2 ? 0 : 1)))
		return r;

	return strcmp(git_reference_shorthand(r1->ref),
	              git_reference_shorthand(r2->ref));
}

int
getrefs(struct referenceinfo **pris, size_t *prefcount)
{
	struct referenceinfo *ris = NULL;
	struct commitinfo *ci = NULL;
	git_reference_iterator *it = NULL;
	const git_oid *id = NULL;
	git_object *obj = NULL;
	git_reference *dref = NULL, *r, *ref = NULL;
	size_t i, refcount;

	*pris = NULL;
	*prefcount = 0;

	if (git_reference_iterator_new(&it, repo))
		return -1;

	for (refcount = 0; !git_reference_next(&ref, it); ) {
		if (!git_reference_is_branch(ref) && !git_reference_is_tag(ref)) {
			git_reference_free(ref);
			ref = NULL;
			continue;
		}

		switch (git_reference_type(ref)) {
		case GIT_REF_SYMBOLIC:
			if (git_reference_resolve(&dref, ref))
				goto err;
			r = dref;
			break;
		case GIT_REF_OID:
			r = ref;
			break;
		default:
			continue;
		}
		if (!git_reference_target(r) ||
		    git_reference_peel(&obj, r, GIT_OBJ_ANY))
			goto err;
		if (!(id = git_object_id(obj)))
			goto err;
		if (!(ci = commitinfo_getbyoid(id)))
			break;

		if (!(ris = reallocarray(ris, refcount + 1, sizeof(*ris))))
			err(1, "realloc");
		ris[refcount].ci = ci;
		ris[refcount].ref = r;
		refcount++;

		git_object_free(obj);
		obj = NULL;
		git_reference_free(dref);
		dref = NULL;
	}
	git_reference_iterator_free(it);

	/* sort by type, date then shorthand name */
	qsort(ris, refcount, sizeof(*ris), refs_cmp);

	*pris = ris;
	*prefcount = refcount;

	return 0;

err:
	git_object_free(obj);
	git_reference_free(dref);
	commitinfo_free(ci);
	for (i = 0; i < refcount; i++) {
		commitinfo_free(ris[i].ci);
		git_reference_free(ris[i].ref);
	}
	free(ris);

	return -1;
}

FILE *
efopen(const char *name, const char *flags)
{
	FILE *fp;

	if (!(fp = fopen(name, flags)))
		err(1, "fopen: '%s'", name);

	return fp;
}

/* Escape characters below as HTML 2.0 / XML 1.0. */
void
xmlencode(FILE *fp, const char *s, size_t len)
{
	size_t i;

	for (i = 0; *s && i < len; s++, i++) {
		switch(*s) {
		case '<':  fputs("&lt;",   fp); break;
		case '>':  fputs("&gt;",   fp); break;
		case '\'': fputs("&#39;",  fp); break;
		case '&':  fputs("&amp;",  fp); break;
		case '"':  fputs("&quot;", fp); break;
		default:   fputc(*s, fp);
		}
	}
}

int
mkdirp(const char *path)
{
	char tmp[PATH_MAX], *p;

	if (strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp))
		errx(1, "path truncated: '%s'", path);
	for (p = tmp + (tmp[0] == '/'); *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, S_IRWXU | S_IRWXG | S_IRWXO) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, S_IRWXU | S_IRWXG | S_IRWXO) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

void
printtimez(FILE *fp, const git_time *intime)
{
	struct tm *intm;
	time_t t;
	char out[32];

	t = (time_t)intime->time;
	if (!(intm = gmtime(&t)))
		return;
	strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", intm);
	fputs(out, fp);
}

void
printtime(FILE *fp, const git_time *intime)
{
	struct tm *intm;
	time_t t;
	char out[32];

	t = (time_t)intime->time + (intime->offset * 60);
	if (!(intm = gmtime(&t)))
		return;
	strftime(out, sizeof(out), "%a, %e %b %Y %H:%M:%S", intm);
	if (intime->offset < 0)
		fprintf(fp, "%s -%02d%02d", out,
		            -(intime->offset) / 60, -(intime->offset) % 60);
	else
		fprintf(fp, "%s +%02d%02d", out,
		            intime->offset / 60, intime->offset % 60);
}

void
printtimeshort(FILE *fp, const git_time *intime)
{
	struct tm *intm;
	time_t t;
	char out[32];

	t = (time_t)intime->time;
	if (!(intm = gmtime(&t)))
		return;
	strftime(out, sizeof(out), "%Y-%m-%d %H:%M", intm);
	fputs(out, fp);
}

void
writeheader(FILE *fp, const char *title)
{
	fputs("<!DOCTYPE html>\n"
		"<html>\n<head>\n"
		"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\" />\n"
		"<title>", fp);
	xmlencode(fp, title, strlen(title));
	if (title[0] && strippedname[0])
		fputs(" - ", fp);
	xmlencode(fp, strippedname, strlen(strippedname));
	if (description[0])
		fputs(" - ", fp);
	xmlencode(fp, description, strlen(description));
	/* Asset files are in parent directory */
	fputs("</title>\n", fp);
	fprintf(fp, "<link rel=\"icon\" type=\"image/png\" href=\"%s../favicon.png\" />\n", relpath);
	fprintf(fp, "<link rel=\"alternate\" type=\"application/atom+xml\" title=\"%s Atom Feed\" href=\"%satom.xml\" />\n",
		name, relpath);
	fprintf(fp, "<link rel=\"alternate\" type=\"application/atom+xml\" title=\"%s Atom Feed (tags)\" href=\"%stags.xml\" />\n",
		name, relpath);
	fprintf(fp, "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s../style.css\" />\n", relpath);
	/* highlight.js for syntax highlighting */
	fputs("<link rel=\"stylesheet\" href=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github.min.css\" media=\"(prefers-color-scheme: light)\" />\n", fp);
	fputs("<link rel=\"stylesheet\" href=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css\" media=\"(prefers-color-scheme: dark)\" />\n", fp);
	fputs("<script src=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js\"></script>\n", fp);
	fputs("</head>\n<body>\n", fp);
	
	/* Theme toggle button */
	fputs("<button id=\"theme-toggle\" aria-label=\"Toggle dark mode\" title=\"Toggle theme\">🌓</button>\n", fp);
	
	/* Header */
	fputs("<header class=\"repo-header\"><div class=\"container\">\n", fp);
	fputs("<div class=\"repo-title\">", fp);
	fprintf(fp, "<a href=\"%s../index.html\"><img src=\"%s../logo.png\" alt=\"\" width=\"24\" height=\"24\" /></a>",
	        relpath, relpath);
	fputs("<h1>", fp);
	xmlencode(fp, strippedname, strlen(strippedname));
	fputs("</h1>", fp);
	if (description[0]) {
		fputs("<span class=\"desc\">", fp);
		xmlencode(fp, description, strlen(description));
		fputs("</span>", fp);
	}
	fputs("</div>\n", fp);

	if (cloneurl[0]) {
		fputs("<div class=\"url\" style=\"margin: 12px 0;\">", fp);
		fputs("<input id=\"clone-url\" class=\"clone-url\" type=\"text\" readonly value=\"git clone ", fp);
		xmlencode(fp, cloneurl, strlen(cloneurl));
		fputs("\" /> ", fp);
		fputs("<button id=\"copy-btn\" class=\"copy-btn\" type=\"button\" aria-label=\"Copy clone URL\">Copy</button>", fp);
		fputs("</div>\n", fp);
	}

	/* Navigation */
	fputs("<nav class=\"nav\"><ul class=\"nav__list\">\n", fp);

	/* Log */
	fprintf(fp,
	"<li class=\"nav__item\"><a class=\"nav__link\" href=\"%slog.html\">"
	"<svg class=\"nav__icon\" width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" aria-hidden=\"true\" role=\"img\" fill=\"currentColor\">"
		"<path d=\"M12 1.75a10.25 10.25 0 1 0 0 20.5 10.25 10.25 0 0 0 0-20.5Zm0 1.5a8.75 8.75 0 1 1 0 17.5 8.75 8.75 0 0 1 0-17.5Zm-.75 3.75a.75.75 0 0 1 1.5 0v5.19l3.22 1.86a.75.75 0 0 1-.75 1.3l-3.72-2.15a.75.75 0 0 1-.37-.65V7z\"/>"
	"</svg>"
	"<span class=\"nav__text\">Log</span></a></li>\n", relpath);

	/* Files */
	fprintf(fp,
	"<li class=\"nav__item\"><a class=\"nav__link\" href=\"%sfiles.html\">"
	"<svg class=\"nav__icon\" width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" aria-hidden=\"true\" role=\"img\" fill=\"currentColor\">"
		"<path d=\"M4 5.5A1.5 1.5 0 0 1 5.5 4h4.38c.4 0 .78.16 1.06.44l1.12 1.12c.28.28.66.44 1.06.44H18.5A1.5 1.5 0 0 1 20 7.5v10A2.5 2.5 0 0 1 17.5 20h-11A2.5 2.5 0 0 1 4 17.5v-12Z\"/>"
	"</svg>"
	"<span class=\"nav__text\">Files</span></a></li>\n", relpath);

	/* Refs */
	fprintf(fp,
	"<li class=\"nav__item\"><a class=\"nav__link\" href=\"%srefs.html\">"
	"<svg class=\"nav__icon\" width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" aria-hidden=\"true\" role=\"img\" fill=\"currentColor\">"
		"<path d=\"M7 4.5A2.5 2.5 0 1 1 7 9.5 2.5 2.5 0 0 1 7 4.5Zm0 1.5a1 1 0 1 0 0 2 1 1 0 0 0 0-2Zm2 4.75h6.19l-2.22-2.22a.75.75 0 0 1 1.06-1.06l3.5 3.5a.75.75 0 0 1 0 1.06l-3.5 3.5a.75.75 0 1 1-1.06-1.06l2.22-2.22H9a2 2 0 0 0-2 2V19a.75.75 0 0 1-1.5 0v-6a3.5 3.5 0 0 1 3.5-3.5Z\"/>"
	"</svg>"
	"<span class=\"nav__text\">Refs</span></a></li>\n", relpath);

	/* Submodules（存在する場合のみ） */
	if (submodules)
	fprintf(fp,
		"<li class=\"nav__item\"><a class=\"nav__link\" href=\"%sfile/%s.html\">"
		"<svg class=\"nav__icon\" width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" aria-hidden=\"true\" role=\"img\" fill=\"currentColor\">"
		"<path d=\"M4.5 7A2.5 2.5 0 0 1 7 4.5h10A2.5 2.5 0 0 1 19.5 7v10A2.5 2.5 0 0 1 17 19.5H7A2.5 2.5 0 0 1 4.5 17V7Zm3 1.5h9v7h-9v-7Zm-1.5 0v7A1 1 0 0 0 7 16.5h.5v-9H7A1 1 0 0 0 6 8.5Z\"/>"
		"</svg>"
		"<span class=\"nav__text\">Submodules</span></a></li>\n", relpath, submodules);

	/* README（存在する場合のみ） */
	if (readme)
	fprintf(fp,
		"<li class=\"nav__item\"><a class=\"nav__link\" href=\"%sfile/%s.html\">"
		"<svg class=\"nav__icon\" width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" aria-hidden=\"true\" role=\"img\" fill=\"currentColor\">"
		"<path d=\"M6.5 4A2.5 2.5 0 0 0 4 6.5v11A2.5 2.5 0 0 0 6.5 20h9A2.5 2.5 0 0 0 18 17.5v-11A2.5 2.5 0 0 0 15.5 4h-9Zm0 1.5h9A1 1 0 0 1 16.5 6.5v9.25c-.55-.3-1.2-.5-2-.5H7a3.5 3.5 0 0 0-2 .5V6.5A1 1 0 0 1 6.5 5.5Z\"/>"
		"</svg>"
		"<span class=\"nav__text\">README</span></a></li>\n", relpath, readme);

	/* LICENSE（存在する場合のみ） */
	if (license)
	fprintf(fp,
		"<li class=\"nav__item\"><a class=\"nav__link\" href=\"%sfile/%s.html\">"
		"<svg class=\"nav__icon\" width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" aria-hidden=\"true\" role=\"img\" fill=\"currentColor\">"
		"<path d=\"M12 2a6 6 0 0 1 6 6v4.59l1.3 1.3a1 1 0 0 1-1.41 1.41l-.89-.9A6.97 6.97 0 0 1 12 17a6.97 6.97 0 0 1-5-2.2l-.89.9a1 1 0 1 1-1.41-1.41L6 12.59V8a6 6 0 0 1 6-6Zm0 2A4 4 0 0 0 8 8v5.17A4.97 4.97 0 0 0 12 15c1.93 0 3.65-.55 5-1.83V8a4 4 0 0 0-4-4Z\"/>"
		"</svg>"
		"<span class=\"nav__text\">LICENSE</span></a></li>\n", relpath, license);

	fputs("</ul></nav>\n", fp);
	fputs("</div></header>\n", fp);
	
	/* Breadcrumb navigation */
	fputs("<nav aria-label=\"Breadcrumb\" class=\"container\" style=\"padding-top:16px;\">\n", fp);
	fputs("<ol class=\"breadcrumb\">\n", fp);
	fprintf(fp, "<li><a href=\"%s../index.html\">Home</a></li>\n", relpath);
	fputs("<li><span id=\"breadcrumb-page\"></span></li>\n", fp);
	fputs("</ol>\n</nav>\n", fp);
	
	/* Main content wrapper */
	fputs("<main><div id=\"content\" class=\"container\">\n", fp);
	
	/* JavaScript for theme toggle and clipboard */
	fputs("<script>\n"
		"/* Theme toggle */\n"
		"(function(){\n"
		"  var toggle=document.getElementById('theme-toggle');\n"
		"  var body=document.body;\n"
		"  var theme=localStorage.getItem('theme');\n"
		"  if(theme){body.className=theme;}\n"
		"  if(toggle){\n"
		"    toggle.addEventListener('click',function(){\n"
		"      var current=body.className||'';\n"
		"      var next=current==='theme-dark'?'theme-light':'theme-dark';\n"
		"      body.className=next;\n"
		"      localStorage.setItem('theme',next);\n"
		"    });\n"
		"  }\n"
		"})();\n"
		"/* Clipboard copy */\n"
		"(function(){\n"
		"  var b=document.getElementById('copy-btn');\n"
		"  var i=document.getElementById('clone-url');\n"
		"  if(!b||!i)return;\n"
		"  b.addEventListener('click',function(){\n"
		"    var v=i.value;\n"
		"    if(navigator.clipboard&&navigator.clipboard.writeText){\n"
		"      navigator.clipboard.writeText(v).then(function(){\n"
		"        b.textContent='Copied!';setTimeout(function(){b.textContent='Copy';},1200);\n"
		"      });\n"
		"    }else{\n"
		"      i.select();\n"
		"      try{document.execCommand('copy');b.textContent='Copied!';setTimeout(function(){b.textContent='Copy';},1200);}catch(e){}\n"
		"      if(window.getSelection)window.getSelection().removeAllRanges();\n"
		"    }\n"
		"  });\n"
		"})();\n"
		"/* Set active page and breadcrumb */\n"
		"(function(){\n"
		"  var path=window.location.pathname;\n"
		"  var filename=path.split('/').pop();\n"
		"  var links=document.querySelectorAll('.nav__link');\n"
		"  var breadcrumb=document.getElementById('breadcrumb-page');\n"
		"  var pageName='';\n"
		"  for(var i=0;i<links.length;i++){\n"
		"    var link=links[i];\n"
		"    if(link.getAttribute('href').indexOf(filename)>-1){\n"
		"      link.setAttribute('aria-current','page');\n"
		"      pageName=link.querySelector('.nav__text').textContent;\n"
		"      break;\n"
		"    }\n"
		"  }\n"
		"  if(breadcrumb&&pageName){breadcrumb.textContent=pageName;}\n"
		"  else if(breadcrumb){breadcrumb.textContent=document.title.split(' - ')[0];}\n"
		"})();\n"
		"/* highlight.js initialization */\n"
		"if(typeof hljs!=='undefined'){hljs.highlightAll();}\n"
		"</script>\n", fp);
}

void
writefooter(FILE *fp)
{
	fputs("</div></main>\n</body>\n</html>\n", fp);
}

void
printfileicon(FILE *fp, const char *filename, int isdir)
{
	const char *ext;
	
	if (isdir) {
		/* Directory icon */
		fputs("<svg class=\"file-icon file-icon-dir\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"currentColor\">"
			"<path d=\"M1.75 1A1.75 1.75 0 0 0 0 2.75v10.5C0 14.216.784 15 1.75 15h12.5A1.75 1.75 0 0 0 16 13.25v-8.5A1.75 1.75 0 0 0 14.25 3H7.5a.25.25 0 0 1-.2-.1l-.9-1.2C6.07 1.26 5.55 1 5 1H1.75Z\"></path>"
			"</svg>", fp);
		return;
	}
	
	/* File icon based on extension */
	ext = strrchr(filename, '.');
	if (ext) {
		ext++; /* skip the dot */
		/* Code files */
		if (!strcasecmp(ext, "c") || !strcasecmp(ext, "h") ||
		    !strcasecmp(ext, "cpp") || !strcasecmp(ext, "cc") ||
		    !strcasecmp(ext, "cxx") || !strcasecmp(ext, "java") ||
		    !strcasecmp(ext, "py") || !strcasecmp(ext, "js") ||
		    !strcasecmp(ext, "ts") || !strcasecmp(ext, "go") ||
		    !strcasecmp(ext, "rs") || !strcasecmp(ext, "rb")) {
			fputs("<svg class=\"file-icon\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"currentColor\">"
				"<path d=\"M4 1.75C4 .784 4.784 0 5.75 0h5.586c.464 0 .909.184 1.237.513l2.914 2.914c.329.328.513.773.513 1.237v8.586A1.75 1.75 0 0 1 14.25 15h-9a.75.75 0 0 1 0-1.5h9a.25.25 0 0 0 .25-.25V6h-2.75A1.75 1.75 0 0 1 10 4.25V1.5H5.75a.25.25 0 0 0-.25.25v2.5a.75.75 0 0 1-1.5 0V1.75Zm-1 10.5a.75.75 0 0 1 .75-.75h.5a.75.75 0 0 1 0 1.5h-.5a.75.75 0 0 1-.75-.75Zm3.75-.75a.75.75 0 0 0 0 1.5h.5a.75.75 0 0 0 0-1.5h-.5Z\"></path>"
				"</svg>", fp);
		/* Markdown */
		} else if (!strcasecmp(ext, "md") || !strcasecmp(ext, "markdown")) {
			fputs("<svg class=\"file-icon\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"currentColor\">"
				"<path d=\"M14.85 3c.63 0 1.15.52 1.14 1.15v7.7c0 .63-.51 1.15-1.15 1.15H1.15C.52 13 0 12.48 0 11.84V4.15C0 3.52.52 3 1.15 3ZM9 11V5H7L5.5 7 4 5H2v6h2V8l1.5 1.92L7 8v3Zm2.99.5L14.5 8H13V5h-2v3H9.5Z\"></path>"
				"</svg>", fp);
		/* Config files */
		} else if (!strcasecmp(ext, "json") || !strcasecmp(ext, "xml") ||
		           !strcasecmp(ext, "yaml") || !strcasecmp(ext, "yml") ||
		           !strcasecmp(ext, "toml") || !strcasecmp(ext, "conf") ||
		           !strcasecmp(ext, "cfg") || !strcasecmp(ext, "ini")) {
			fputs("<svg class=\"file-icon\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"currentColor\">"
				"<path d=\"M9.5 1.25a3.25 3.25 0 1 1 4.22 3.1c.14.155.28.347.395.562.113.214.2.488.254.782.09.49.09 1.066.09 1.681V9.5a.75.75 0 0 1-1.5 0V7.375c0-.676 0-1.163-.08-1.565a2.583 2.583 0 0 0-.17-.522 1.78 1.78 0 0 0-.248-.363A3.25 3.25 0 0 1 9.5 1.25ZM6.25 4a3.25 3.25 0 0 0-3.226 3.575.75.75 0 0 1-1.476.236A4.75 4.75 0 0 1 6.25 2.5h.5a.75.75 0 0 1 0 1.5h-.5Z\"></path>"
				"</svg>", fp);
		/* Images */
		} else if (!strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") ||
		           !strcasecmp(ext, "jpeg") || !strcasecmp(ext, "gif") ||
		           !strcasecmp(ext, "svg") || !strcasecmp(ext, "webp")) {
			fputs("<svg class=\"file-icon\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"currentColor\">"
				"<path d=\"M16 13.25A1.75 1.75 0 0 1 14.25 15H1.75A1.75 1.75 0 0 1 0 13.25V2.75C0 1.784.784 1 1.75 1h12.5c.966 0 1.75.784 1.75 1.75ZM1.75 2.5a.25.25 0 0 0-.25.25v10.5c0 .138.112.25.25.25h.94l.03-.03 6.077-6.078a1.75 1.75 0 0 1 2.412-.06L14.5 10.31V2.75a.25.25 0 0 0-.25-.25Z\"></path>"
				"</svg>", fp);
		} else {
			/* Default file icon */
			fputs("<svg class=\"file-icon file-icon-file\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"currentColor\">"
				"<path d=\"M2 1.75C2 .784 2.784 0 3.75 0h6.586c.464 0 .909.184 1.237.513l2.914 2.914c.329.328.513.773.513 1.237v9.586A1.75 1.75 0 0 1 13.25 16h-9.5A1.75 1.75 0 0 1 2 14.25Zm1.75-.25a.25.25 0 0 0-.25.25v12.5c0 .138.112.25.25.25h9.5a.25.25 0 0 0 .25-.25V6h-2.75A1.75 1.75 0 0 1 9 4.25V1.5Zm6.75.062V4.25c0 .138.112.25.25.25h2.688l-.011-.013-2.914-2.914-.013-.011Z\"></path>"
				"</svg>", fp);
		}
	} else {
		/* Default file icon (no extension) */
		fputs("<svg class=\"file-icon file-icon-file\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"currentColor\">"
			"<path d=\"M2 1.75C2 .784 2.784 0 3.75 0h6.586c.464 0 .909.184 1.237.513l2.914 2.914c.329.328.513.773.513 1.237v9.586A1.75 1.75 0 0 1 13.25 16h-9.5A1.75 1.75 0 0 1 2 14.25Zm1.75-.25a.25.25 0 0 0-.25.25v12.5c0 .138.112.25.25.25h9.5a.25.25 0 0 0 .25-.25V6h-2.75A1.75 1.75 0 0 1 9 4.25V1.5Zm6.75.062V4.25c0 .138.112.25.25.25h2.688l-.011-.013-2.914-2.914-.013-.011Z\"></path>"
			"</svg>", fp);
	}
}

int
writeblobhtml(FILE *fp, const git_blob *blob, const char *filename)
{
	size_t n = 0, i, prev;
	const char *nfmt = "<a href=\"#l%d\" class=\"line\" id=\"l%d\">%7d</a> ";
	const char *s = git_blob_rawcontent(blob);
	git_off_t len = git_blob_rawsize(blob);
	const char *ext, *lang = "";
	
	/* Detect language from file extension */
	if (filename && (ext = strrchr(filename, '.'))) {
		ext++; /* skip dot */
		if (!strcasecmp(ext, "c")) lang = "language-c";
		else if (!strcasecmp(ext, "h")) lang = "language-c";
		else if (!strcasecmp(ext, "cpp") || !strcasecmp(ext, "cc") || 
		         !strcasecmp(ext, "cxx")) lang = "language-cpp";
		else if (!strcasecmp(ext, "py")) lang = "language-python";
		else if (!strcasecmp(ext, "js")) lang = "language-javascript";
		else if (!strcasecmp(ext, "ts")) lang = "language-typescript";
		else if (!strcasecmp(ext, "java")) lang = "language-java";
		else if (!strcasecmp(ext, "go")) lang = "language-go";
		else if (!strcasecmp(ext, "rs")) lang = "language-rust";
		else if (!strcasecmp(ext, "rb")) lang = "language-ruby";
		else if (!strcasecmp(ext, "html") || !strcasecmp(ext, "htm")) lang = "language-html";
		else if (!strcasecmp(ext, "css")) lang = "language-css";
		else if (!strcasecmp(ext, "json")) lang = "language-json";
		else if (!strcasecmp(ext, "xml")) lang = "language-xml";
		else if (!strcasecmp(ext, "sh") || !strcasecmp(ext, "bash")) lang = "language-bash";
		else if (!strcasecmp(ext, "md") || !strcasecmp(ext, "markdown")) lang = "language-markdown";
	}

	fprintf(fp, "<pre id=\"blob\"><code class=\"%s\">\n", lang);

	if (len > 0) {
		for (i = 0, prev = 0; i < (size_t)len; i++) {
			if (s[i] != '\n')
				continue;
			n++;
			fprintf(fp, nfmt, n, n, n);
			xmlencode(fp, &s[prev], i - prev + 1);
			prev = i + 1;
		}
		/* trailing data */
		if ((len - prev) > 0) {
			n++;
			fprintf(fp, nfmt, n, n, n);
			xmlencode(fp, &s[prev], len - prev);
		}
	}

	fputs("</code></pre>\n", fp);

	return n;
}

void
printcommit(FILE *fp, struct commitinfo *ci)
{
	fprintf(fp, "<b>commit</b> <a href=\"%scommit/%s.html\">%s</a>\n",
		relpath, ci->oid, ci->oid);

	if (ci->parentoid[0])
		fprintf(fp, "<b>parent</b> <a href=\"%scommit/%s.html\">%s</a>\n",
			relpath, ci->parentoid, ci->parentoid);

	if (ci->author) {
		fputs("<b>Author:</b> ", fp);
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs(" &lt;<a href=\"mailto:", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("\">", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("</a>&gt;\n<b>Date:</b>   ", fp);
		printtime(fp, &(ci->author->when));
		fputc('\n', fp);
	}
	if (ci->msg) {
		fputc('\n', fp);
		xmlencode(fp, ci->msg, strlen(ci->msg));
		fputc('\n', fp);
	}
}

void
printshowfile(FILE *fp, struct commitinfo *ci)
{
	const git_diff_delta *delta;
	const git_diff_hunk *hunk;
	const git_diff_line *line;
	git_patch *patch;
	size_t nhunks, nhunklines, changed, add, del, total, i, j, k;
	char linestr[80];
	int c;

	printcommit(fp, ci);

	if (!ci->deltas)
		return;

	if (ci->filecount > 1000   ||
	    ci->ndeltas   > 1000   ||
	    ci->addcount  > 100000 ||
	    ci->delcount  > 100000) {
		fputs("Diff is too large, output suppressed.\n", fp);
		return;
	}

	/* diff stat */
	fputs("<b>Diffstat:</b>\n<table>", fp);
	for (i = 0; i < ci->ndeltas; i++) {
		delta = git_patch_get_delta(ci->deltas[i]->patch);

		switch (delta->status) {
		case GIT_DELTA_ADDED:      c = 'A'; break;
		case GIT_DELTA_COPIED:     c = 'C'; break;
		case GIT_DELTA_DELETED:    c = 'D'; break;
		case GIT_DELTA_MODIFIED:   c = 'M'; break;
		case GIT_DELTA_RENAMED:    c = 'R'; break;
		case GIT_DELTA_TYPECHANGE: c = 'T'; break;
		default:                   c = ' '; break;
		}
		if (c == ' ')
			fprintf(fp, "<tr><td>%c", c);
		else
			fprintf(fp, "<tr><td class=\"%c\">%c", c, c);

		fprintf(fp, "</td><td><a href=\"#h%zu\">", i);
		xmlencode(fp, delta->old_file.path, strlen(delta->old_file.path));
		if (strcmp(delta->old_file.path, delta->new_file.path)) {
			fputs(" -&gt; ", fp);
			xmlencode(fp, delta->new_file.path, strlen(delta->new_file.path));
		}

		add = ci->deltas[i]->addcount;
		del = ci->deltas[i]->delcount;
		changed = add + del;
		total = sizeof(linestr) - 2;
		if (changed > total) {
			if (add)
				add = ((float)total / changed * add) + 1;
			if (del)
				del = ((float)total / changed * del) + 1;
		}
		memset(&linestr, '+', add);
		memset(&linestr[add], '-', del);

		fprintf(fp, "</a></td><td> | </td><td class=\"num\">%zu</td><td><span class=\"i\">",
		        ci->deltas[i]->addcount + ci->deltas[i]->delcount);
		fwrite(&linestr, 1, add, fp);
		fputs("</span><span class=\"d\">", fp);
		fwrite(&linestr[add], 1, del, fp);
		fputs("</span></td></tr>\n", fp);
	}
	fprintf(fp, "</table></pre><pre>%zu file%s changed, %zu insertion%s(+), %zu deletion%s(-)\n",
		ci->filecount, ci->filecount == 1 ? "" : "s",
	        ci->addcount,  ci->addcount  == 1 ? "" : "s",
	        ci->delcount,  ci->delcount  == 1 ? "" : "s");

	fputs("<hr/>", fp);

	for (i = 0; i < ci->ndeltas; i++) {
		patch = ci->deltas[i]->patch;
		delta = git_patch_get_delta(patch);
		fprintf(fp, "<b>diff --git a/<a id=\"h%zu\" href=\"%sfile/", i, relpath);
		xmlencode(fp, delta->old_file.path, strlen(delta->old_file.path));
		fputs(".html\">", fp);
		xmlencode(fp, delta->old_file.path, strlen(delta->old_file.path));
		fprintf(fp, "</a> b/<a href=\"%sfile/", relpath);
		xmlencode(fp, delta->new_file.path, strlen(delta->new_file.path));
		fprintf(fp, ".html\">");
		xmlencode(fp, delta->new_file.path, strlen(delta->new_file.path));
		fprintf(fp, "</a></b>\n");

		/* check binary data */
		if (delta->flags & GIT_DIFF_FLAG_BINARY) {
			fputs("Binary files differ.\n", fp);
			continue;
		}

		nhunks = git_patch_num_hunks(patch);
		for (j = 0; j < nhunks; j++) {
			if (git_patch_get_hunk(&hunk, &nhunklines, patch, j))
				break;

			fprintf(fp, "<a href=\"#h%zu-%zu\" id=\"h%zu-%zu\" class=\"h\">", i, j, i, j);
			xmlencode(fp, hunk->header, hunk->header_len);
			fputs("</a>", fp);

			for (k = 0; ; k++) {
				if (git_patch_get_line_in_hunk(&line, patch, j, k))
					break;
				if (line->old_lineno == -1)
					fprintf(fp, "<a href=\"#h%zu-%zu-%zu\" id=\"h%zu-%zu-%zu\" class=\"i\">+",
						i, j, k, i, j, k);
				else if (line->new_lineno == -1)
					fprintf(fp, "<a href=\"#h%zu-%zu-%zu\" id=\"h%zu-%zu-%zu\" class=\"d\">-",
						i, j, k, i, j, k);
				else
					fputc(' ', fp);
				xmlencode(fp, line->content, line->content_len);
				if (line->old_lineno == -1 || line->new_lineno == -1)
					fputs("</a>", fp);
			}
		}
	}
}

void
writelogline(FILE *fp, struct commitinfo *ci)
{
	fputs("<tr><td>", fp);
	if (ci->author)
		printtimeshort(fp, &(ci->author->when));
	fputs("</td><td>", fp);
	if (ci->summary) {
		fprintf(fp, "<a href=\"%scommit/%s.html\">", relpath, ci->oid);
		xmlencode(fp, ci->summary, strlen(ci->summary));
		fputs("</a>", fp);
	}
	fputs("</td><td>", fp);
	if (ci->author)
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
	fputs("</td><td class=\"num\" align=\"right\">", fp);
	fprintf(fp, "%zu", ci->filecount);
	fputs("</td><td class=\"num\" align=\"right\">", fp);
	fputs("<span class=\"add-stat\">+", fp);
	fprintf(fp, "%zu", ci->addcount);
	fputs("</span></td><td class=\"num\" align=\"right\">", fp);
	fputs("<span class=\"del-stat\">-", fp);
	fprintf(fp, "%zu", ci->delcount);
	fputs("</span></td></tr>\n", fp);
}

int
writelog(FILE *fp, const git_oid *oid)
{
	struct commitinfo *ci;
	git_revwalk *w = NULL;
	git_oid id;
	char path[PATH_MAX], oidstr[GIT_OID_HEXSZ + 1];
	FILE *fpfile;
	int r;

	git_revwalk_new(&w, repo);
	git_revwalk_push(w, oid);
	git_revwalk_simplify_first_parent(w);

	while (!git_revwalk_next(&id, w)) {
		relpath = "";

		if (cachefile && !memcmp(&id, &lastoid, sizeof(id)))
			break;

		git_oid_tostr(oidstr, sizeof(oidstr), &id);
		r = snprintf(path, sizeof(path), "commit/%s.html", oidstr);
		if (r < 0 || (size_t)r >= sizeof(path))
			errx(1, "path truncated: 'commit/%s.html'", oidstr);
		r = access(path, F_OK);

		/* optimization: if there are no log lines to write and
		   the commit file already exists: skip the diffstat */
		if (!nlogcommits && !r)
			continue;

		if (!(ci = commitinfo_getbyoid(&id)))
			break;
		/* diffstat: for stagit HTML required for the log.html line */
		if (commitinfo_getstats(ci) == -1)
			goto err;

		if (nlogcommits < 0) {
			writelogline(fp, ci);
		} else if (nlogcommits > 0) {
			writelogline(fp, ci);
			nlogcommits--;
			if (!nlogcommits && ci->parentoid[0])
				fputs("<tr><td></td><td colspan=\"5\">"
				      "More commits remaining [...]</td>"
				      "</tr>\n", fp);
		}

		if (cachefile)
			writelogline(wcachefp, ci);

		/* check if file exists if so skip it */
		if (r) {
			relpath = "../";
			fpfile = efopen(path, "w");
			writeheader(fpfile, ci->summary);
			fputs("<pre>", fpfile);
			printshowfile(fpfile, ci);
			fputs("</pre>\n", fpfile);
			writefooter(fpfile);
			fclose(fpfile);
		}
err:
		commitinfo_free(ci);
	}
	git_revwalk_free(w);

	relpath = "";

	return 0;
}

void
printcommitatom(FILE *fp, struct commitinfo *ci, const char *tag)
{
	fputs("<entry>\n", fp);

	fprintf(fp, "<id>%s</id>\n", ci->oid);
	if (ci->author) {
		fputs("<published>", fp);
		printtimez(fp, &(ci->author->when));
		fputs("</published>\n", fp);
	}
	if (ci->committer) {
		fputs("<updated>", fp);
		printtimez(fp, &(ci->committer->when));
		fputs("</updated>\n", fp);
	}
	if (ci->summary) {
		fputs("<title type=\"text\">", fp);
		if (tag && tag[0]) {
			fputs("[", fp);
			xmlencode(fp, tag, strlen(tag));
			fputs("] ", fp);
		}
		xmlencode(fp, ci->summary, strlen(ci->summary));
		fputs("</title>\n", fp);
	}
	fprintf(fp, "<link rel=\"alternate\" type=\"text/html\" href=\"commit/%s.html\" />\n",
	        ci->oid);

	if (ci->author) {
		fputs("<author>\n<name>", fp);
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs("</name>\n<email>", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("</email>\n</author>\n", fp);
	}

	fputs("<content type=\"text\">", fp);
	fprintf(fp, "commit %s\n", ci->oid);
	if (ci->parentoid[0])
		fprintf(fp, "parent %s\n", ci->parentoid);
	if (ci->author) {
		fputs("Author: ", fp);
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs(" &lt;", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("&gt;\nDate:   ", fp);
		printtime(fp, &(ci->author->when));
		fputc('\n', fp);
	}
	if (ci->msg) {
		fputc('\n', fp);
		xmlencode(fp, ci->msg, strlen(ci->msg));
	}
	fputs("\n</content>\n</entry>\n", fp);
}

int
writeatom(FILE *fp, int all)
{
	struct referenceinfo *ris = NULL;
	size_t refcount = 0;
	struct commitinfo *ci;
	git_revwalk *w = NULL;
	git_oid id;
	size_t i, m = 100; /* last 'm' commits */

	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	      "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n<title>", fp);
	xmlencode(fp, strippedname, strlen(strippedname));
	fputs(", branch HEAD</title>\n<subtitle>", fp);
	xmlencode(fp, description, strlen(description));
	fputs("</subtitle>\n", fp);

	/* all commits or only tags? */
	if (all) {
		git_revwalk_new(&w, repo);
		git_revwalk_push_head(w);
		git_revwalk_simplify_first_parent(w);
		for (i = 0; i < m && !git_revwalk_next(&id, w); i++) {
			if (!(ci = commitinfo_getbyoid(&id)))
				break;
			printcommitatom(fp, ci, "");
			commitinfo_free(ci);
		}
		git_revwalk_free(w);
	} else if (getrefs(&ris, &refcount) != -1) {
		/* references: tags */
		for (i = 0; i < refcount; i++) {
			if (git_reference_is_tag(ris[i].ref))
				printcommitatom(fp, ris[i].ci,
				                git_reference_shorthand(ris[i].ref));

			commitinfo_free(ris[i].ci);
			git_reference_free(ris[i].ref);
		}
		free(ris);
	}

	fputs("</feed>\n", fp);

	return 0;
}

int
writeblob(git_object *obj, const char *fpath, const char *filename, git_off_t filesize)
{
	char tmp[PATH_MAX] = "", *d;
	const char *p;
	int lc = 0;
	FILE *fp;

	if (strlcpy(tmp, fpath, sizeof(tmp)) >= sizeof(tmp))
		errx(1, "path truncated: '%s'", fpath);
	if (!(d = dirname(tmp)))
		err(1, "dirname");
	if (mkdirp(d))
		return -1;

	for (p = fpath, tmp[0] = '\0'; *p; p++) {
		if (*p == '/' && strlcat(tmp, "../", sizeof(tmp)) >= sizeof(tmp))
			errx(1, "path truncated: '../%s'", tmp);
	}
	relpath = tmp;

	fp = efopen(fpath, "w");
	writeheader(fp, filename);
	fputs("<p class=\"filename\"> ", fp);
	xmlencode(fp, filename, strlen(filename));
	fprintf(fp, " (%juB)", (uintmax_t)filesize);
	fputs("</p>", fp);


    if (git_blob_is_binary((git_blob *)obj)) {
        fputs("<p class=\"binary-file\">Binary file.</p>\n", fp);
    } else {
#ifdef WITH_MD4C
        /* README.md など Markdown なら md4c で HTML 描画 */
        if (is_markdown_filename(filename)) {
            const char *s = git_blob_rawcontent((git_blob *)obj);
            git_off_t len = git_blob_rawsize((git_blob *)obj);
            /* お好みでラッパー要素を追加（CSS: .markdown-body を当てやすく） */
            fputs("<section class=\"panel markdown-body\">\n", fp);
            if (render_markdown(fp, s, (size_t)len) != 0) {
                /* 失敗時は従来のプレーン表示へフォールバック */
                fputs("</section>\n", fp);
                lc = writeblobhtml(fp, (git_blob *)obj, filename);
            } else {
                fputs("\n</section>\n", fp);
                lc = 0; /* 行番号を出していないので 0 扱い */
            }
        } else
#endif
        {
            lc = writeblobhtml(fp, (git_blob *)obj, filename);
        }
        if (ferror(fp))
            err(1, "fwrite");
    }

	writefooter(fp);
	fclose(fp);

	relpath = "";

	return lc;
}

const char *
filemode(git_filemode_t m)
{
	static char mode[11];

	memset(mode, '-', sizeof(mode) - 1);
	mode[10] = '\0';

	if (S_ISREG(m))
		mode[0] = '-';
	else if (S_ISBLK(m))
		mode[0] = 'b';
	else if (S_ISCHR(m))
		mode[0] = 'c';
	else if (S_ISDIR(m))
		mode[0] = 'd';
	else if (S_ISFIFO(m))
		mode[0] = 'p';
	else if (S_ISLNK(m))
		mode[0] = 'l';
	else if (S_ISSOCK(m))
		mode[0] = 's';
	else
		mode[0] = '?';

	if (m & S_IRUSR) mode[1] = 'r';
	if (m & S_IWUSR) mode[2] = 'w';
	if (m & S_IXUSR) mode[3] = 'x';
	if (m & S_IRGRP) mode[4] = 'r';
	if (m & S_IWGRP) mode[5] = 'w';
	if (m & S_IXGRP) mode[6] = 'x';
	if (m & S_IROTH) mode[7] = 'r';
	if (m & S_IWOTH) mode[8] = 'w';
	if (m & S_IXOTH) mode[9] = 'x';

	if (m & S_ISUID) mode[3] = (mode[3] == 'x') ? 's' : 'S';
	if (m & S_ISGID) mode[6] = (mode[6] == 'x') ? 's' : 'S';
	if (m & S_ISVTX) mode[9] = (mode[9] == 'x') ? 't' : 'T';

	return mode;
}

int
writefilestree(FILE *fp, git_tree *tree, const char *path)
{
	const git_tree_entry *entry = NULL;
	git_object *obj = NULL;
	git_off_t filesize;
	const char *entryname;
	char filepath[PATH_MAX], entrypath[PATH_MAX];
	size_t count, i;
	int lc, r, ret;
	int depth = 0;
	const char *p;

	/* Calculate depth from path - entries in this directory */
	if (*path) {
		depth = 1;
		for (p = path; *p; p++)
			if (*p == '/') depth++;
	}

	count = git_tree_entrycount(tree);
	
	/* First pass: directories */
	for (i = 0; i < count; i++) {
		if (!(entry = git_tree_entry_byindex(tree, i)) ||
		    !(entryname = git_tree_entry_name(entry)))
			return -1;
			
		if (git_tree_entry_type(entry) != GIT_OBJ_TREE)
			continue;
			
		joinpath(entrypath, sizeof(entrypath), path, entryname);
		
		/* Directory row */
		fprintf(fp, "<tr class=\"dir-row\" data-path=\"%s\" data-parent=\"%s\" data-depth=\"%d\">", 
		        entrypath, path, depth);
		fputs("<td>", fp);
		
		/* Indentation */
		for (int d = 0; d < depth; d++)
			fputs("<span class=\"tree-indent\"></span>", fp);
		
		/* Toggle icon */
		fputs("<span class=\"dir-toggle\">▸</span>", fp);
		printfileicon(fp, entryname, 1);
		fputs("<span class=\"dirname-clickable\">", fp);
		xmlencode(fp, entryname, strlen(entryname));
		fputs("/</span>", fp);
		
		fputs("</td><td>d---------</td><td class=\"num\" align=\"right\">-</td></tr>\n", fp);
		
		/* Recursively write directory contents */
		if (!git_tree_entry_to_object(&obj, repo, entry)) {
			ret = writefilestree(fp, (git_tree *)obj, entrypath);
			git_object_free(obj);
			if (ret)
				return ret;
		}
	}
	
	/* Second pass: files */
	for (i = 0; i < count; i++) {
		if (!(entry = git_tree_entry_byindex(tree, i)) ||
		    !(entryname = git_tree_entry_name(entry)))
			return -1;
		joinpath(entrypath, sizeof(entrypath), path, entryname);

		r = snprintf(filepath, sizeof(filepath), "file/%s.html",
		         entrypath);
		if (r < 0 || (size_t)r >= sizeof(filepath))
			errx(1, "path truncated: 'file/%s.html'", entrypath);

		if (git_tree_entry_type(entry) == GIT_OBJ_TREE)
			continue; /* Already handled in first pass */
			
		if (!git_tree_entry_to_object(&obj, repo, entry)) {
			if (git_object_type(obj) != GIT_OBJ_BLOB) {
				git_object_free(obj);
				continue;
			}

			filesize = git_blob_rawsize((git_blob *)obj);
			lc = writeblob(obj, filepath, entryname, filesize);

			fprintf(fp, "<tr class=\"file-row\" data-path=\"%s\" data-parent=\"%s\" data-depth=\"%d\">",
			        entrypath, path, depth);
			fputs("<td><a href=\"", fp);
			fprintf(fp, "%s", relpath);
			xmlencode(fp, filepath, strlen(filepath));
			fputs("\">", fp);
			
			/* Indentation */
			for (int d = 0; d < depth; d++)
				fputs("<span class=\"tree-indent\"></span>", fp);
			
			printfileicon(fp, entryname, 0);
			xmlencode(fp, entryname, strlen(entryname));
			fputs("</a></td><td>", fp);
			fputs(filemode(git_tree_entry_filemode(entry)), fp);
			
			fputs("</td><td class=\"num\" align=\"right\">", fp);
			if (lc > 0)
				fprintf(fp, "%dL", lc);
			else
				fprintf(fp, "%juB", (uintmax_t)filesize);
			fputs("</td></tr>\n", fp);
			git_object_free(obj);
		} else if (git_tree_entry_type(entry) == GIT_OBJ_COMMIT) {
			/* commit object in tree is a submodule */
			fprintf(fp, "<tr class=\"file-row\" data-path=\"%s\" data-parent=\"%s\" data-depth=\"%d\">",
			        entrypath, path, depth);
			fprintf(fp, "<td><a href=\"%sfile/.gitmodules.html\">",
				relpath);
			
			/* Indentation */
			for (int d = 0; d < depth; d++)
				fputs("<span class=\"tree-indent\"></span>", fp);
			
			printfileicon(fp, entryname, 0);
			xmlencode(fp, entryname, strlen(entryname));
			fputs("</a></td><td>m---------</td><td class=\"num\" align=\"right\">@</td></tr>\n", fp);
		}
	}

	return 0;
}

int
writefiles(FILE *fp, const git_oid *id)
{
	git_tree *tree = NULL;
	git_commit *commit = NULL;
	int ret = -1;

	/* File search box */
	fputs("<div class=\"file-search\">\n", fp);
	fputs("<input type=\"search\" id=\"file-search\" placeholder=\"Find file...\" aria-label=\"Search files\" />\n", fp);
	fputs("</div>\n", fp);
	
	fputs("<table id=\"files\"><thead>\n<tr>"
	      "<td><b>Name</b></td><td><b>Mode</b></td>"
	      "<td class=\"num\" align=\"right\"><b>Size</b></td>"
	      "</tr>\n</thead><tbody>\n", fp);

	if (!git_commit_lookup(&commit, repo, id) &&
	    !git_commit_tree(&tree, commit))
		ret = writefilestree(fp, tree, "");

	fputs("</tbody></table>", fp);
	
	/* File tree and search scripts */
	fputs("<script>\n"
		"/* Directory toggle functionality */\n"
		"(function(){\n"
		"  var dirRows=document.querySelectorAll('.dir-row');\n"
		"  var collapsedDirs={};\n"
		"  \n"
		"  function toggleDir(path,expand){\n"
		"    var rows=document.querySelectorAll('[data-parent=\"'+path+'\"]');\n"
		"    for(var i=0;i<rows.length;i++){\n"
		"      if(expand){\n"
		"        rows[i].style.display='';\n"
		"        if(rows[i].classList.contains('dir-row')){\n"
		"          var subpath=rows[i].getAttribute('data-path');\n"
		"          if(!collapsedDirs[subpath]){\n"
		"            toggleDir(subpath,true);\n"
		"          }\n"
		"        }\n"
		"      }else{\n"
		"        rows[i].style.display='none';\n"
		"        if(rows[i].classList.contains('dir-row')){\n"
		"          var subpath=rows[i].getAttribute('data-path');\n"
		"          toggleDir(subpath,false);\n"
		"        }\n"
		"      }\n"
		"    }\n"
		"  }\n"
		"  \n"
		"  for(var i=0;i<dirRows.length;i++){\n"
		"    dirRows[i].style.cursor='pointer';\n"
		"    dirRows[i].addEventListener('click',function(e){\n"
		"      var path=this.getAttribute('data-path');\n"
		"      var toggle=this.querySelector('.dir-toggle');\n"
		"      var isCollapsed=collapsedDirs[path];\n"
		"      \n"
		"      if(isCollapsed){\n"
		"        delete collapsedDirs[path];\n"
		"        toggle.textContent='▾';\n"
		"        toggleDir(path,true);\n"
		"      }else{\n"
		"        collapsedDirs[path]=true;\n"
		"        toggle.textContent='▸';\n"
		"        toggleDir(path,false);\n"
		"      }\n"
		"    });\n"
		"  }\n"
		"  \n"
		"  /* Initialize all directories as collapsed */\n"
		"  for(var i=0;i<dirRows.length;i++){\n"
		"    var path=dirRows[i].getAttribute('data-path');\n"
		"    collapsedDirs[path]=true;\n"
		"    toggleDir(path,false);\n"
		"  }\n"
		"})();\n"
		"\n"
		"/* File search */\n"
		"(function(){\n"
		"  var input=document.getElementById('file-search');\n"
		"  var table=document.getElementById('files');\n"
		"  if(!input||!table)return;\n"
		"  var allRows=table.querySelectorAll('tbody tr');\n"
		"  \n"
		"  input.addEventListener('input',function(){\n"
		"    var filter=input.value.toLowerCase();\n"
		"    if(!filter){\n"
		"      /* Reset visibility */\n"
		"      for(var i=0;i<allRows.length;i++){\n"
		"        allRows[i].style.display='';\n"
		"      }\n"
		"      return;\n"
		"    }\n"
		"    \n"
		"    /* Search and show matching rows with their parents */\n"
		"    var visiblePaths={};\n"
		"    for(var i=0;i<allRows.length;i++){\n"
		"      var row=allRows[i];\n"
		"      var nameCell=row.querySelector('td:nth-child(1)');\n"
		"      if(!nameCell)continue;\n"
		"      \n"
		"      var text=nameCell.textContent||nameCell.innerText;\n"
		"      var path=row.getAttribute('data-path')||'';\n"
		"      \n"
		"      if(text.toLowerCase().indexOf(filter)>-1){\n"
		"        row.style.display='';\n"
		"        visiblePaths[path]=true;\n"
		"        /* Show parent directories */\n"
		"        var parts=path.split('/');\n"
		"        var parentPath='';\n"
		"        for(var j=0;j<parts.length-1;j++){\n"
		"          parentPath+=parts[j];\n"
		"          visiblePaths[parentPath]=true;\n"
		"          parentPath+='/';\n"
		"        }\n"
		"      }else{\n"
		"        row.style.display='none';\n"
		"      }\n"
		"    }\n"
		"    \n"
		"    /* Show visible parent directories */\n"
		"    for(var i=0;i<allRows.length;i++){\n"
		"      var path=allRows[i].getAttribute('data-path');\n"
		"      if(path&&visiblePaths[path]){\n"
		"        allRows[i].style.display='';\n"
		"      }\n"
		"    }\n"
		"  });\n"
		"  \n"
		"  /* Keyboard shortcut: / to focus search */\n"
		"  document.addEventListener('keydown',function(e){\n"
		"    if(e.key==='/'&&document.activeElement!==input){\n"
		"      e.preventDefault();input.focus();\n"
		"    }\n"
		"  });\n"
		"})();\n"
		"</script>\n", fp);

	git_commit_free(commit);
	git_tree_free(tree);

	return ret;
}

int
writerefs(FILE *fp)
{
	struct referenceinfo *ris = NULL;
	struct commitinfo *ci;
	size_t count, i, j, refcount;
	const char *titles[] = { "Branches", "Tags" };
	const char *ids[] = { "branches", "tags" };
	const char *s;

	if (getrefs(&ris, &refcount) == -1)
		return -1;

	for (i = 0, j = 0, count = 0; i < refcount; i++) {
		if (j == 0 && git_reference_is_tag(ris[i].ref)) {
			if (count)
				fputs("</tbody></table><br/>\n", fp);
			count = 0;
			j = 1;
		}

		/* print header if it has an entry (first). */
		if (++count == 1) {
			fprintf(fp, "<h2>%s</h2><table id=\"%s\">"
		                "<thead>\n<tr><td><b>Name</b></td>"
			        "<td><b>Last commit date</b></td>"
			        "<td><b>Author</b></td>\n</tr>\n"
			        "</thead><tbody>\n",
			         titles[j], ids[j]);
		}

		ci = ris[i].ci;
		s = git_reference_shorthand(ris[i].ref);

		fputs("<tr><td>", fp);
		xmlencode(fp, s, strlen(s));
		fputs("</td><td>", fp);
		if (ci->author)
			printtimeshort(fp, &(ci->author->when));
		fputs("</td><td>", fp);
		if (ci->author)
			xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs("</td></tr>\n", fp);
	}
	/* table footer */
	if (count)
		fputs("</tbody></table><br/>\n", fp);

	for (i = 0; i < refcount; i++) {
		commitinfo_free(ris[i].ci);
		git_reference_free(ris[i].ref);
	}
	free(ris);

	return 0;
}

void
usage(char *argv0)
{
	fprintf(stderr, "%s [-c cachefile | -l commits] repodir\n", argv0);
	exit(1);
}

int
main(int argc, char *argv[])
{
	git_object *obj = NULL;
	const git_oid *head = NULL;
	mode_t mask;
	FILE *fp, *fpread;
	char path[PATH_MAX], repodirabs[PATH_MAX + 1], *p;
	char tmppath[64] = "cache.XXXXXXXXXXXX", buf[BUFSIZ];
	size_t n;
	int i, fd;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			if (repodir)
				usage(argv[0]);
			repodir = argv[i];
		} else if (argv[i][1] == 'c') {
			if (nlogcommits > 0 || i + 1 >= argc)
				usage(argv[0]);
			cachefile = argv[++i];
		} else if (argv[i][1] == 'l') {
			if (cachefile || i + 1 >= argc)
				usage(argv[0]);
			errno = 0;
			nlogcommits = strtoll(argv[++i], &p, 10);
			if (argv[i][0] == '\0' || *p != '\0' ||
			    nlogcommits <= 0 || errno)
				usage(argv[0]);
		}
	}
	if (!repodir)
		usage(argv[0]);

	if (!realpath(repodir, repodirabs))
		err(1, "realpath");

	git_libgit2_init();

#ifdef __OpenBSD__
	if (unveil(repodir, "r") == -1)
		err(1, "unveil: %s", repodir);
	if (unveil(".", "rwc") == -1)
		err(1, "unveil: .");
	if (cachefile && unveil(cachefile, "rwc") == -1)
		err(1, "unveil: %s", cachefile);

	if (cachefile) {
		if (pledge("stdio rpath wpath cpath fattr", NULL) == -1)
			err(1, "pledge");
	} else {
		if (pledge("stdio rpath wpath cpath", NULL) == -1)
			err(1, "pledge");
	}
#endif

	if (git_repository_open_ext(&repo, repodir,
		GIT_REPOSITORY_OPEN_NO_SEARCH, NULL) < 0) {
		fprintf(stderr, "%s: cannot open repository\n", argv[0]);
		return 1;
	}

	/* find HEAD */
	if (!git_revparse_single(&obj, repo, "HEAD"))
		head = git_object_id(obj);
	git_object_free(obj);

	/* use directory name as name */
	if ((name = strrchr(repodirabs, '/')))
		name++;
	else
		name = "";

	/* strip .git suffix */
	if (!(strippedname = strdup(name)))
		err(1, "strdup");
	if ((p = strrchr(strippedname, '.')))
		if (!strcmp(p, ".git"))
			*p = '\0';

	/* read description or .git/description */
	joinpath(path, sizeof(path), repodir, "description");
	if (!(fpread = fopen(path, "r"))) {
		joinpath(path, sizeof(path), repodir, ".git/description");
		fpread = fopen(path, "r");
	}
	if (fpread) {
		if (!fgets(description, sizeof(description), fpread))
			description[0] = '\0';
		fclose(fpread);
	}

	/* read url or .git/url */
	joinpath(path, sizeof(path), repodir, "url");
	if (!(fpread = fopen(path, "r"))) {
		joinpath(path, sizeof(path), repodir, ".git/url");
		fpread = fopen(path, "r");
	}
	if (fpread) {
		if (!fgets(cloneurl, sizeof(cloneurl), fpread))
			cloneurl[0] = '\0';
		cloneurl[strcspn(cloneurl, "\n")] = '\0';
		fclose(fpread);
	}

	/* check LICENSE */
	for (i = 0; i < sizeof(licensefiles) / sizeof(*licensefiles) && !license; i++) {
		if (!git_revparse_single(&obj, repo, licensefiles[i]) &&
		    git_object_type(obj) == GIT_OBJ_BLOB)
			license = licensefiles[i] + strlen("HEAD:");
		git_object_free(obj);
	}

	/* check README */
	for (i = 0; i < sizeof(readmefiles) / sizeof(*readmefiles) && !readme; i++) {
		if (!git_revparse_single(&obj, repo, readmefiles[i]) &&
		    git_object_type(obj) == GIT_OBJ_BLOB)
			readme = readmefiles[i] + strlen("HEAD:");
		git_object_free(obj);
	}

	if (!git_revparse_single(&obj, repo, "HEAD:.gitmodules") &&
	    git_object_type(obj) == GIT_OBJ_BLOB)
		submodules = ".gitmodules";
	git_object_free(obj);

	/* log for HEAD */
	fp = efopen("log.html", "w");
	relpath = "";
	mkdir("commit", S_IRWXU | S_IRWXG | S_IRWXO);
	writeheader(fp, "Log");
	fputs("<table id=\"log\"><thead>\n<tr><td><b>Date</b></td>"
	      "<td><b>Commit message</b></td>"
	      "<td><b>Author</b></td><td class=\"num\" align=\"right\"><b>Files</b></td>"
	      "<td class=\"num\" align=\"right\"><b>+</b></td>"
	      "<td class=\"num\" align=\"right\"><b>-</b></td></tr>\n</thead><tbody>\n", fp);

	if (cachefile && head) {
		/* read from cache file (does not need to exist) */
		if ((rcachefp = fopen(cachefile, "r"))) {
			if (!fgets(lastoidstr, sizeof(lastoidstr), rcachefp))
				errx(1, "%s: no object id", cachefile);
			if (git_oid_fromstr(&lastoid, lastoidstr))
				errx(1, "%s: invalid object id", cachefile);
		}

		/* write log to (temporary) cache */
		if ((fd = mkstemp(tmppath)) == -1)
			err(1, "mkstemp");
		if (!(wcachefp = fdopen(fd, "w")))
			err(1, "fdopen: '%s'", tmppath);
		/* write last commit id (HEAD) */
		git_oid_tostr(buf, sizeof(buf), head);
		fprintf(wcachefp, "%s\n", buf);

		writelog(fp, head);

		if (rcachefp) {
			/* append previous log to log.html and the new cache */
			while (!feof(rcachefp)) {
				n = fread(buf, 1, sizeof(buf), rcachefp);
				if (ferror(rcachefp))
					err(1, "fread");
				if (fwrite(buf, 1, n, fp) != n ||
				    fwrite(buf, 1, n, wcachefp) != n)
					err(1, "fwrite");
			}
			fclose(rcachefp);
		}
		fclose(wcachefp);
	} else {
		if (head)
			writelog(fp, head);
	}

	fputs("</tbody></table>", fp);
	writefooter(fp);
	fclose(fp);

	/* files for HEAD */
	fp = efopen("files.html", "w");
	writeheader(fp, "Files");
	if (head)
		writefiles(fp, head);
	writefooter(fp);
	fclose(fp);

	/* summary page with branches and tags */
	fp = efopen("refs.html", "w");
	writeheader(fp, "Refs");
	writerefs(fp);
	writefooter(fp);
	fclose(fp);

	/* Atom feed */
	fp = efopen("atom.xml", "w");
	writeatom(fp, 1);
	fclose(fp);

	/* Atom feed for tags / releases */
	fp = efopen("tags.xml", "w");
	writeatom(fp, 0);
	fclose(fp);

	/* rename new cache file on success */
	if (cachefile && head) {
		if (rename(tmppath, cachefile))
			err(1, "rename: '%s' to '%s'", tmppath, cachefile);
		umask((mask = umask(0)));
		if (chmod(cachefile,
		    (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH) & ~mask))
			err(1, "chmod: '%s'", cachefile);
	}

	/* copy asset files (style.css, logo.png, favicon.png) to parent directory */
	{
		const char *assets[] = {"style.css", "logo.png", "favicon.png"};
		const char *search_paths[] = {
			".",                          /* current directory */
			"/usr/local/share/stagit",    /* system install */
			"/usr/share/stagit",          /* system install */
			NULL
		};
		FILE *src, *dst;
		char srcpath[PATH_MAX], dstpath[PATH_MAX];
		unsigned char copybuf[BUFSIZ];
		size_t nread;
		int j, k, found;
		
		for (j = 0; j < 3; j++) {
			/* destination is parent directory */
			snprintf(dstpath, sizeof(dstpath), "../%s", assets[j]);
			
			/* skip if file already exists in parent directory */
			if (access(dstpath, F_OK) == 0)
				continue;
			
			found = 0;
			
			/* try to find the asset file */
			for (k = 0; search_paths[k] && !found; k++) {
				snprintf(srcpath, sizeof(srcpath), "%s/%s", 
				         search_paths[k], assets[j]);
				
				if ((src = fopen(srcpath, "rb"))) {
					if ((dst = fopen(dstpath, "wb"))) {
						while ((nread = fread(copybuf, 1, sizeof(copybuf), src)) > 0) {
							if (fwrite(copybuf, 1, nread, dst) != nread) {
								fprintf(stderr, "warning: failed to write %s\n", dstpath);
								break;
							}
						}
						fclose(dst);
						found = 1;
					}
					fclose(src);
				}
			}
			
			if (!found && j == 0) {
				/* style.css is critical, write a minimal default */
				if ((dst = fopen(dstpath, "w"))) {
					fputs("/* stagit default style */\n"
					      "body { font-family: monospace; }\n", dst);
					fclose(dst);
				}
			}
		}
	}

	/* cleanup */
	git_repository_free(repo);
	git_libgit2_shutdown();

	return 0;
}
