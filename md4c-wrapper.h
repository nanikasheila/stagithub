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

/* Convert mermaid code blocks and write to buffer */
static void
convert_mermaid_blocks_to_buffer(struct md_buffer *out, const char *html, size_t len)
{
	size_t i = 0;
	while (i < len) {
		/* Look for class="language-mermaid" */
		if (i + 23 < len && 
		    !strncmp(&html[i], "class=\"language-mermaid\"", 24)) {
			/* Back up to find <pre><code  */
			size_t start = i;
			while (start > 0 && html[start] != '<') start--;
			
			/* Check if we found <code */
			if (start > 0 && start + 5 < len && !strncmp(&html[start], "<code ", 6)) {
				/* Back up more to find <pre> */
				size_t pre_start = start - 1;
				while (pre_start > 0 && html[pre_start] != '<') pre_start--;
				
				if (pre_start > 0 && !strncmp(&html[pre_start], "<pre>", 5)) {
					/* Copy everything before <pre> */
					if (out->size + pre_start > out->capacity) {
						size_t new_cap = out->capacity ? out->capacity * 2 : 4096;
						while (new_cap < out->size + pre_start) new_cap *= 2;
						out->data = realloc(out->data, new_cap);
						if (!out->data) err(1, "realloc");
						out->capacity = new_cap;
					}
					memcpy(out->data + out->size, html, pre_start);
					out->size += pre_start;
					
					/* Write <pre class="mermaid"> */
					const char *replacement = "<pre class=\"mermaid\">";
					size_t repl_len = strlen(replacement);
					if (out->size + repl_len > out->capacity) {
						size_t new_cap = out->capacity * 2;
						while (new_cap < out->size + repl_len) new_cap *= 2;
						out->data = realloc(out->data, new_cap);
						if (!out->data) err(1, "realloc");
						out->capacity = new_cap;
					}
					memcpy(out->data + out->size, replacement, repl_len);
					out->size += repl_len;
					
					/* Skip to after the closing > of <code class="language-mermaid"> */
					i = i + 24; /* skip class="language-mermaid" */
					while (i < len && html[i] != '>') i++;
					i++; /* skip the > */
					
					/* Copy content until </code></pre> */
					size_t content_start = i;
					while (i + 13 < len) {
						if (!strncmp(&html[i], "</code></pre>", 13)) {
							/* Copy the content */
							size_t content_len = i - content_start;
							if (out->size + content_len > out->capacity) {
								size_t new_cap = out->capacity * 2;
								while (new_cap < out->size + content_len) new_cap *= 2;
								out->data = realloc(out->data, new_cap);
								if (!out->data) err(1, "realloc");
								out->capacity = new_cap;
							}
							memcpy(out->data + out->size, &html[content_start], content_len);
							out->size += content_len;
							
							/* Write </pre> */
							const char *end_tag = "</pre>";
							size_t end_len = strlen(end_tag);
							if (out->size + end_len > out->capacity) {
								size_t new_cap = out->capacity * 2;
								out->data = realloc(out->data, new_cap);
								if (!out->data) err(1, "realloc");
								out->capacity = new_cap;
							}
							memcpy(out->data + out->size, end_tag, end_len);
							out->size += end_len;
							
							i += 13; /* skip </code></pre> */
							html = &html[i];
							len -= i;
							i = 0;
							break;
						}
						i++;
					}
					continue;
				}
			}
		}
		
		/* Add character to buffer */
		size_t needed = out->size + 1;
		if (needed > out->capacity) {
			size_t new_cap = out->capacity ? out->capacity * 2 : 4096;
			while (new_cap < needed) new_cap *= 2;
			out->data = realloc(out->data, new_cap);
			if (!out->data) err(1, "realloc");
			out->capacity = new_cap;
		}
		out->data[out->size++] = html[i++];
	}
}

/* Convert mermaid code blocks to mermaid divs */
static void
convert_mermaid_blocks(FILE *fp, const char *html, size_t len)
{
	size_t i = 0;
	size_t last_write = 0;
	
	while (i < len) {
		/* Look for class="language-mermaid" */
		if (i + 23 < len && 
		    !strncmp(&html[i], "class=\"language-mermaid\"", 24)) {
			/* Back up to find <pre><code  */
			size_t start = i;
			while (start > 0 && html[start] != '<') start--;
			
			/* Check if we found <code */
			if (start > 0 && start + 5 < len && !strncmp(&html[start], "<code ", 6)) {
				/* Back up more to find <pre> */
				size_t pre_start = start - 1;
				while (pre_start > 0 && html[pre_start] != '<') pre_start--;
				
				if (pre_start > 0 && !strncmp(&html[pre_start], "<pre>", 5)) {
					/* Write everything before <pre> */
					fwrite(&html[last_write], 1, pre_start - last_write, fp);
					
					/* Write <pre class="mermaid"> */
					fputs("<pre class=\"mermaid\">", fp);
					
					/* Skip to after the closing > of <code class="language-mermaid"> */
					i = i + 24; /* skip class="language-mermaid" */
					while (i < len && html[i] != '>') i++;
					i++; /* skip the > */
					
					/* Copy content until </code></pre> */
					size_t content_start = i;
					while (i + 13 < len) {
						if (!strncmp(&html[i], "</code></pre>", 13)) {
							/* Write the content */
							fwrite(&html[content_start], 1, i - content_start, fp);
							/* Write </pre> */
							fputs("</pre>", fp);
							i += 13; /* skip </code></pre> */
							last_write = i;
							break;
						}
						i++;
					}
					continue;
				}
			}
		}
		i++;
	}
	
	/* Write any remaining content */
	if (last_write < len) {
		fwrite(&html[last_write], 1, len - last_write, fp);
	}
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
	struct md_buffer temp = {0};
	unsigned parser_flags = MD_DIALECT_GITHUB;
#ifdef STAGIT_MD_NOHTML
	parser_flags |= MD_FLAG_NOHTML;
#endif
	unsigned renderer_flags = 0;
	
	/* Render to buffer first */
	int ret = md_html((const MD_CHAR*)buf, (MD_SIZE)len, md4c_buffer_cb, &output,
	                  parser_flags, renderer_flags);
	
	if (ret == 0 && output.data && output.size > 0) {
		/* Directly apply both conversions using a temporary FILE* */
		FILE *temp_fp = tmpfile();
		if (temp_fp) {
			/* First convert mermaid blocks */
			convert_mermaid_blocks(temp_fp, output.data, output.size);
			
			/* Get the size and rewind */
			fseek(temp_fp, 0, SEEK_END);
			long temp_size = ftell(temp_fp);
			fseek(temp_fp, 0, SEEK_SET);
			
			/* Read into temp buffer */
			if (temp_size > 0) {
				temp.data = malloc(temp_size);
				if (temp.data) {
					temp.size = fread(temp.data, 1, temp_size, temp_fp);
					
					/* Then convert .md links to .md.html and write to final output */
					convert_md_links(fp, temp.data, temp.size);
					
					free(temp.data);
				}
			}
			fclose(temp_fp);
		}
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
		/* Convert mermaid blocks and write to output */
		convert_mermaid_blocks(fp, output.data, output.size);
	}
	
	free(output.data);
	return ret;
}

#endif /* WITH_MD4C */

#endif /* MD4C_WRAPPER_H */
