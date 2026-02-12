/* md4c-wrapper.h - Markdown rendering utilities for stagit */
#ifndef MD4C_WRAPPER_H
#define MD4C_WRAPPER_H

#ifdef WITH_MD4C
#include <md4c-html.h>

/* Buffer for capturing md4c output */
struct md_buffer {
	char *data;
	size_t size;
	size_t capacity;
};

/* md4c output callback for buffer */
static void
md4c_buffer_cb(const MD_CHAR *data, MD_SIZE size, void *ud)
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
static void
convert_md_links(FILE *fp, const char *html, size_t len)
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

/* Check if filename has markdown extension */
static int
is_markdown_filename(const char *name)
{
	if (!name) return 0;
	const char *ext = strrchr(name, '.');
	if (!ext) return 0;
	return !strcasecmp(ext, ".md")
	    || !strcasecmp(ext, ".markdown")
	    || !strcasecmp(ext, ".mdown")
	    || !strcasecmp(ext, ".mkd");
}

/* Render Markdown to HTML with link conversion (for stagit) */
static int
render_markdown_with_links(FILE *fp, const char *buf, size_t len)
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

/* Render Markdown to HTML without link conversion (for stagit-index) */
static int
render_markdown(FILE *fp, const char *buf, size_t len)
{
	struct md_buffer output = {0};
	unsigned parser_flags = MD_DIALECT_GITHUB;
	unsigned renderer_flags = 0;
	
	/* Render to buffer first */
	int ret = md_html((const MD_CHAR*)buf, (MD_SIZE)len, md4c_buffer_cb, &output,
	                  parser_flags, renderer_flags);
	
	if (ret == 0 && output.data && output.size > 0) {
		fwrite(output.data, 1, output.size, fp);
	}
	
	free(output.data);
	return ret;
}

#endif /* WITH_MD4C */

#endif /* MD4C_WRAPPER_H */
