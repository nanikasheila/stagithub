#include <err.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <git2.h>

#include "md4c-wrapper.h"

static git_repository *repo;

static const char *relpath = "";

static char description[255] = "Repositories";
static char *name = "";
static char owner[255];

/* 追加：README 候補（順に優先）*/
static const char *readme_candidates[] = {
    "HEAD:README.md",
    "HEAD:README.markdown",
    "HEAD:README.mdown",
    "HEAD:README.mkd",
    "HEAD:README"
};
static const size_t n_readme_candidates = sizeof(readme_candidates)/sizeof(readme_candidates[0]);

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

/* Escape characters below as HTML 2.0 / XML 1.0. */
void
xmlencode(FILE *fp, const char *s, size_t len)
{
	size_t i;

	for (i = 0; *s && i < len; s++, i++) {
		switch(*s) {
		case '<':  fputs("&lt;",   fp); break;
		case '>':  fputs("&gt;",   fp); break;
		case '\'': fputs("&#39;" , fp); break;
		case '&':  fputs("&amp;",  fp); break;
		case '"':  fputs("&quot;", fp); break;
		default:   fputc(*s, fp);
		}
	}
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
writeheader(FILE *fp)
{
	fputs("<!DOCTYPE html>\n"
		"<html>\n<head>\n"
		"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\" />\n"
		"<title>", fp);
	xmlencode(fp, description, strlen(description));
	fprintf(fp, "</title>\n<link rel=\"icon\" type=\"image/png\" href=\"%sfavicon.png\" />\n", relpath);
	fprintf(fp, "<link rel=\"stylesheet\" type=\"text/css\" href=\"%sstyle.css\" />\n", relpath);
	/* mermaid.js for diagram rendering */
	fputs("<script type=\"module\">\n", fp);
	fputs("import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';\n", fp);
	fputs("mermaid.initialize({ startOnLoad: true, theme: 'default' });\n", fp);
	fputs("</script>\n", fp);
	fputs("</head>\n<body>\n", fp);
	
	/* Theme toggle button */
	fputs("<button id=\"theme-toggle\" aria-label=\"Toggle dark mode\" title=\"Toggle theme\">🌓</button>\n", fp);
	
	/* Header */
	fputs("<header class=\"repo-header\"><div class=\"container\">\n", fp);
	fputs("<div class=\"repo-title\">", fp);
	fprintf(fp, "<img src=\"%slogo.png\" alt=\"\" width=\"24\" height=\"24\" />", relpath);
	fputs("<h1>", fp);
	xmlencode(fp, description, strlen(description));
	fputs("</h1>", fp);
	fputs("<span class=\"desc\">Git Repositories</span>", fp);
	fputs("</div>\n", fp);
	fputs("</div></header>\n", fp);
	
	/* Main content wrapper */
	fputs("<main><div id=\"content\" class=\"container\">\n", fp);
	
	/* Search box */
	fputs("<div class=\"file-search\">\n", fp);
	fputs("<input type=\"search\" id=\"repo-search\" placeholder=\"Find repository...\" aria-label=\"Search repositories\" />\n", fp);
	fputs("</div>\n", fp);
	
	fputs("<table id=\"index\"><thead>\n"
		"<tr><td><b>Name</b></td><td><b>Description</b></td><td><b>Owner</b></td>"
		"<td><b>Last commit</b></td></tr>"
		"</thead><tbody>\n", fp);
}

void
write_readme_section(FILE *fp, const char *readme_path)
{
#ifdef WITH_MD4C
	FILE *readme_fp;
	char *content = NULL;
	size_t size = 0, capacity = 0;
	int c;
	
	if (!(readme_fp = fopen(readme_path, "r")))
		return;
	
	/* Read entire file into buffer */
	while ((c = fgetc(readme_fp)) != EOF) {
		if (size >= capacity) {
			capacity = capacity ? capacity * 2 : 4096;
			if (!(content = realloc(content, capacity)))
				err(1, "realloc");
		}
		content[size++] = c;
	}
	fclose(readme_fp);
	
	if (size > 0) {
		fputs("<div class=\"readme-section\">\n", fp);
		fputs("<div class=\"readme-content\">\n", fp);
		render_markdown(fp, content, size);
		fputs("</div>\n</div>\n", fp);
	}
	
	free(content);
#endif
}

void
writefooter(FILE *fp)
{
	fputs("</tbody>\n</table>\n", fp);
	
	/* Try to display README.md from current directory */
	write_readme_section(fp, "README.md");
	
	fputs("</div></main>\n", fp);
	
	/* JavaScript for theme toggle and search */
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
		"/* Repository search */\n"
		"(function(){\n"
		"  var input=document.getElementById('repo-search');\n"
		"  var table=document.getElementById('index');\n"
		"  if(!input||!table)return;\n"
		"  var rows=table.querySelectorAll('tbody tr');\n"
		"  input.addEventListener('input',function(){\n"
		"    var filter=input.value.toLowerCase();\n"
		"    for(var i=0;i<rows.length;i++){\n"
		"      var nameCell=rows[i].querySelector('td:first-child');\n"
		"      if(!nameCell)continue;\n"
		"      var text=nameCell.textContent||nameCell.innerText;\n"
		"      if(text.toLowerCase().indexOf(filter)>-1){\n"
		"        rows[i].style.display='';\n"
		"      }else{\n"
		"        rows[i].style.display='none';\n"
		"      }\n"
		"    }\n"
		"  });\n"
		"  /* Keyboard shortcut: / to focus search */\n"
		"  document.addEventListener('keydown',function(e){\n"
		"    if(e.key==='/'&&document.activeElement!==input){\n"
		"      e.preventDefault();\n"
		"      input.focus();\n"
		"    }\n"
		"  });\n"
		"})();\n"
		"</script>\n", fp);
	
	fputs("</body>\n</html>\n", fp);
}

int
writelog(FILE *fp)
{
	git_commit *commit = NULL;
	const git_signature *author;
	git_revwalk *w = NULL;
	git_oid id;
	char *stripped_name = NULL, *p;
	char readme_path[255];
	const char *readme_link = NULL;
	git_object *readme_obj = NULL;
	size_t i;
	int ret = 0;

	git_revwalk_new(&w, repo);
	git_revwalk_push_head(w);
	git_revwalk_simplify_first_parent(w);

	if (git_revwalk_next(&id, w) ||
	    git_commit_lookup(&commit, repo, &id)) {
		ret = -1;
		goto err;
	}

	author = git_commit_author(commit);

	/* strip .git suffix */
	if (!(stripped_name = strdup(name)))
		err(1, "strdup");
	if ((p = strrchr(stripped_name, '.')))
		if (!strcmp(p, ".git"))
			*p = '\0';

	/* Find README file (try candidates in order) */
	for (i = 0; i < n_readme_candidates; i++) {
		if (!git_revparse_single(&readme_obj, repo, readme_candidates[i])) {
			/* Extract filename from "HEAD:filename" */
			const char *colon = strchr(readme_candidates[i], ':');
			if (colon) {
				readme_link = colon + 1;
				break;
			}
			git_object_free(readme_obj);
			readme_obj = NULL;
		}
	}

	/* Repository row with icon */
	fputs("<tr><td>", fp);
	/* Repository icon (folder SVG) */
	fputs("<svg width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" fill=\"currentColor\" style=\"vertical-align:middle;margin-right:6px;\" aria-hidden=\"true\">" 
	      "<path d=\"M4 5.5A1.5 1.5 0 0 1 5.5 4h4.38c.4 0 .78.16 1.06.44l1.12 1.12c.28.28.66.44 1.06.44H18.5A1.5 1.5 0 0 1 20 7.5v10A2.5 2.5 0 0 1 17.5 20h-11A2.5 2.5 0 0 1 4 17.5v-12Z\"/>" 
	      "</svg>", fp);
	
	/* Link to log.html or README */
	if (readme_link) {
		fprintf(fp, "<a href=\"%s/file/", stripped_name);
		xmlencode(fp, readme_link, strlen(readme_link));
		fputs(".html\">", fp);
	} else {
		fprintf(fp, "<a href=\"%s/log.html\">", stripped_name);
	}
	xmlencode(fp, stripped_name, strlen(stripped_name));
	fputs("</a></td><td>", fp);
	xmlencode(fp, description, strlen(description));
	fputs("</td><td>", fp);
	xmlencode(fp, owner, strlen(owner));
	fputs("</td><td>", fp);
	if (author)
		printtimeshort(fp, &(author->when));
	fputs("</td></tr>\n", fp);

	if (readme_obj)
		git_object_free(readme_obj);
	git_commit_free(commit);
err:
	git_revwalk_free(w);
	free(stripped_name);

	return ret;
}

int
main(int argc, char *argv[])
{
	FILE *fp;
	char path[PATH_MAX], repodirabs[PATH_MAX + 1];
	const char *repodir;
	int i, ret = 0;

	if (argc < 2) {
		fprintf(stderr, "%s [repodir...]\n", argv[0]);
		return 1;
	}

	git_libgit2_init();

#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		err(1, "pledge");
#endif

	writeheader(stdout);

	for (i = 1; i < argc; i++) {
		repodir = argv[i];
		if (!realpath(repodir, repodirabs))
			err(1, "realpath");

		if (git_repository_open_ext(&repo, repodir,
		    GIT_REPOSITORY_OPEN_NO_SEARCH, NULL)) {
			fprintf(stderr, "%s: cannot open repository\n", argv[0]);
			ret = 1;
			continue;
		}

		/* use directory name as name */
		if ((name = strrchr(repodirabs, '/')))
			name++;
		else
			name = "";

		/* read description or .git/description */
		joinpath(path, sizeof(path), repodir, "description");
		if (!(fp = fopen(path, "r"))) {
			joinpath(path, sizeof(path), repodir, ".git/description");
			fp = fopen(path, "r");
		}
		description[0] = '\0';
		if (fp) {
			if (!fgets(description, sizeof(description), fp))
				description[0] = '\0';
			fclose(fp);
		}

		/* read owner or .git/owner */
		joinpath(path, sizeof(path), repodir, "owner");
		if (!(fp = fopen(path, "r"))) {
			joinpath(path, sizeof(path), repodir, ".git/owner");
			fp = fopen(path, "r");
		}
		owner[0] = '\0';
		if (fp) {
			if (!fgets(owner, sizeof(owner), fp))
				owner[0] = '\0';
			owner[strcspn(owner, "\n")] = '\0';
			fclose(fp);
		}
		writelog(stdout);
	}
	writefooter(stdout);

	/* cleanup */
	git_repository_free(repo);
	git_libgit2_shutdown();

	return ret;
}
