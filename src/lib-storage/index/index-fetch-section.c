/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "temp-string.h"
#include "iobuffer.h"
#include "rfc822-tokenize.h"
#include "message-send.h"
#include "index-storage.h"
#include "index-fetch.h"

#include <ctype.h>
#include <unistd.h>

ImapCacheField index_fetch_body_get_cache(const char *section)
{
	if (*section >= '0' && *section <= '9')
		return IMAP_CACHE_MESSAGE_PART | IMAP_CACHE_MESSAGE_OPEN;

	if (*section == '\0' || strcasecmp(section, "TEXT") == 0) {
		/* no IMAP_CACHE_MESSAGE_BODY_SIZE, so that we don't
		   uselessly check it when we want to read partial data */
		return IMAP_CACHE_MESSAGE_OPEN;
	}

	if (strncasecmp(section, "HEADER", 6) == 0 ||
	    strcasecmp(section, "MIME") == 0)
		return IMAP_CACHE_MESSAGE_HDR_SIZE | IMAP_CACHE_MESSAGE_OPEN;

	/* error */
	return 0;
}

/* fetch BODY[] or BODY[TEXT] */
static int fetch_body(MailIndexRecord *rec, MailFetchBodyData *sect,
		      FetchContext *ctx, const char *prefix, int fetch_header)
{
	MessageSize size;
	IOBuffer *inbuf;
	const char *str;

	if (!imap_msgcache_get_rfc822_partial(ctx->cache, sect->skip,
					      sect->max_size, fetch_header,
					      &size, &inbuf)) {
		i_error("Couldn't get BODY[] for UID %u (index %s)",
			rec->uid, ctx->index->filepath);
		return FALSE;
	}

	str = t_strdup_printf("%s {%"PRIuUOFF_T"}\r\n",
			      prefix, size.virtual_size);
	if (io_buffer_send(ctx->outbuf, str, strlen(str)) < 0)
		return FALSE;

	return message_send(ctx->outbuf, inbuf, &size, 0, sect->max_size);
}

static char *const *get_fields_array(const char *fields)
{
	char **field_list, **field;

	while (*fields == ' ')
		fields++;
	if (*fields == '(')
		fields++;

	field_list = (char **) t_strsplit(fields, " )");

	/* array ends at ")" element */
	for (field = field_list; *field != NULL; field++) {
		if (strcasecmp(*field, ")") == 0)
			*field = NULL;
	}

	return field_list;
}

static int header_match(char *const *fields, const char *name, size_t size)
{
	const char *field, *name_start, *name_end;

	i_assert(size > 0);

	name_start = name;
	name_end = name + size;

	for (; *fields != NULL; fields++) {
		field = *fields;
		if (*field == '\0')
			continue;

		for (name = name_start; name != name_end; name++) {
			/* field has been uppercased long time ago while
			   parsing FETCH command */
			if (i_toupper(*name) != *field)
				break;

			field++;
			if (*field == '\0') {
				if (name+1 == name_end)
					return TRUE;
				break;
			}
		}
	}

	return FALSE;
}

static int header_match_not(char *const *fields, const char *name, size_t size)
{
	return !header_match(fields, name, size);
}

static int header_match_mime(char *const *fields __attr_unused__,
			     const char *name, size_t size)
{
	if (size > 8 && strncasecmp(name, "Content-", 8) == 0)
		return TRUE;

	if (size == 12 && strncasecmp(name, "Mime-Version", 13) == 0)
		return TRUE;

	return FALSE;
}

typedef struct {
	char *dest;
	char *const *fields;
	int (*match_func) (char *const *, const char *, size_t);
} FetchHeaderFieldContext;

static void fetch_header_field(MessagePart *part __attr_unused__,
			       const char *name, size_t name_len,
			       const char *value __attr_unused__,
			       size_t value_len __attr_unused__,
			       void *context)
{
	FetchHeaderFieldContext *ctx = context;
	const char *field_start, *field_end, *cr, *p;
	size_t len;

	/* see if we want this field */
	if (!ctx->match_func(ctx->fields, name, name_len))
		return;

	/* add the field, inserting CRs when needed. FIXME: is this too
	   kludgy? we assume name continues with ": value".. */
	field_start = name;
	field_end = value + value_len;

	cr = NULL;
	for (p = field_start; p != field_end; p++) {
		if (*p == '\r')
			cr = p;
		else if (*p == '\n' && cr != p-1) {
			/* missing CR */
			len = (size_t) (p-field_start);
			memcpy(ctx->dest, field_start, len);

			ctx->dest[len++] = '\r';
			ctx->dest[len++] = '\n';
			ctx->dest += len;

			field_start = p+1;
		}
	}

	if (field_start != field_end) {
		len = (size_t) (field_end-field_start);
		memcpy(ctx->dest, field_start, len);
		ctx->dest += len;
	}

	ctx->dest[0] = '\r';
	ctx->dest[1] = '\n';
	ctx->dest += 2;
}

/* Store headers into dest, returns number of bytes written. */
static size_t
fetch_header_fields(IOBuffer *inbuf, char *dest, char *const *fields,
		    int (*match_func) (char *const *, const char *, size_t))
{
	FetchHeaderFieldContext ctx;

	ctx.dest = dest;
	ctx.fields = fields;
	ctx.match_func = match_func;

	message_parse_header(NULL, inbuf, NULL, fetch_header_field, &ctx);
	return (size_t) (ctx.dest - dest);
}

/* fetch wanted headers from given data */
static int fetch_header_from(IOBuffer *inbuf, MessageSize *size,
			     const char *section, MailFetchBodyData *sect,
			     FetchContext *ctx, const char *prefix)
{
	const char *str;
	char *dest;
	size_t len;
	int failed;

	/* HEADER, MIME, HEADER.FIELDS (list), HEADER.FIELDS.NOT (list) */

	if (strcasecmp(section, "HEADER") == 0) {
		/* all headers */
		str = t_strdup_printf("%s {%"PRIuUOFF_T"}\r\n",
				      prefix, size->virtual_size);
		if (io_buffer_send(ctx->outbuf, str, strlen(str)) < 0)
			return FALSE;
		return message_send(ctx->outbuf, inbuf, size,
				    sect->skip, sect->max_size);
	}

	/* partial headers - copy the wanted fields into temporary memory.
	   Insert missing CRs on the way. FIXME: not a good idea with huge
	   headers. */
	if (size->virtual_size > SSIZE_T_MAX) {
		i_error("Message header too large");
		return FALSE;
	}

	t_push();
	dest = t_malloc((size_t)size->virtual_size);

	if (strncasecmp(section, "HEADER.FIELDS ", 14) == 0) {
		len = fetch_header_fields(inbuf, dest,
					  get_fields_array(section + 14),
					  header_match);
	} else if (strncasecmp(section, "HEADER.FIELDS.NOT ", 18) == 0) {
		len = fetch_header_fields(inbuf, dest,
					  get_fields_array(section + 18),
					  header_match_not);
	} else if (strcasecmp(section, "MIME") == 0) {
		/* Mime-Version + Content-* fields */
		len = fetch_header_fields(inbuf, dest, NULL, header_match_mime);
	} else {
		/* error */
		len = 0;
	}

	i_assert(len <= size->virtual_size);

	if (len <= sect->skip)
		len = 0;
	else {
		dest += sect->skip;
		len -= sect->skip;

		if (len > sect->max_size)
			len = sect->max_size;
	}

	str = t_strdup_printf("%s {%"PRIuSIZE_T"}\r\n", prefix, len);
	failed = io_buffer_send(ctx->outbuf, str, strlen(str)) < 0 ||
		io_buffer_send(ctx->outbuf, dest, len);

	t_pop();
	return !failed;
}

/* fetch BODY[HEADER...] */
static int fetch_header(MailFetchBodyData *sect, FetchContext *ctx,
			const char *prefix)
{
	MessageSize hdr_size;
	IOBuffer *inbuf;

	if (!imap_msgcache_get_rfc822(ctx->cache, &inbuf, &hdr_size, NULL))
		return FALSE;

	return fetch_header_from(inbuf, &hdr_size, sect->section,
				 sect, ctx, prefix);
}

/* Find MessagePart for section (eg. 1.3.4) */
static MessagePart *part_find(MailFetchBodyData *sect, FetchContext *ctx,
			      const char **section)
{
	MessagePart *part;
	const char *path;
	unsigned int num;

	part = imap_msgcache_get_parts(ctx->cache);

	path = sect->section;
	while (*path >= '0' && *path <= '9' && part != NULL) {
		/* get part number */
		num = 0;
		while (*path != '\0' && *path != '.') {
			if (*path < '0' || *path > '9')
				return NULL;
			num = num*10 + (*path - '0');
			path++;
		}

		if (*path == '.')
			path++;

		if (part->flags & MESSAGE_PART_FLAG_MULTIPART) {
			/* find the part */
			part = part->children;
			for (; num > 1 && part != NULL; num--)
				part = part->next;
		} else {
			/* only 1 allowed with non-multipart messages */
			if (num != 1)
				return NULL;
		}
	}

	*section = path;
	return part;
}

/* fetch BODY[1.2] or BODY[1.2.TEXT] */
static int fetch_part_body(MessagePart *part, MailFetchBodyData *sect,
			   FetchContext *ctx, const char *prefix)
{
	IOBuffer *inbuf;
	const char *str;
	uoff_t skip_pos;

	if (!imap_msgcache_get_data(ctx->cache, &inbuf))
		return FALSE;

	/* jump to beginning of wanted data */
	skip_pos = part->physical_pos + part->header_size.physical_size;
	io_buffer_skip(inbuf, skip_pos);

	str = t_strdup_printf("%s {%"PRIuUOFF_T"}\r\n",
			      prefix, part->body_size.virtual_size);
	if (io_buffer_send(ctx->outbuf, str, strlen(str)) < 0)
		return FALSE;

	/* FIXME: potential performance problem with big messages:
	   FETCH BODY[1]<100000..1024>, hopefully no clients do this */
	return message_send(ctx->outbuf, inbuf, &part->body_size,
			    sect->skip, sect->max_size);
}

/* fetch BODY[1.2.MIME|HEADER...] */
static int fetch_part_header(MessagePart *part, const char *section,
			     MailFetchBodyData *sect, FetchContext *ctx,
			     const char *prefix)
{
	IOBuffer *inbuf;

	if (!imap_msgcache_get_data(ctx->cache, &inbuf))
		return FALSE;

	io_buffer_skip(inbuf, part->physical_pos);
	return fetch_header_from(inbuf, &part->header_size, section,
				 sect, ctx, prefix);
}

static int fetch_part(MailFetchBodyData *sect, FetchContext *ctx,
		      const char *prefix)
{
	MessagePart *part;
	const char *section;

	part = part_find(sect, ctx, &section);
	if (part == NULL)
		return FALSE;

	if (*section == '\0' || strcasecmp(section, "TEXT") == 0)
		return fetch_part_body(part, sect, ctx, prefix);

	if (strncasecmp(section, "HEADER", 6) == 0)
		return fetch_part_header(part, section, sect, ctx, prefix);
	if (strcasecmp(section, "MIME") == 0)
		return fetch_part_header(part, section, sect, ctx, prefix);

	return FALSE;
}

int index_fetch_body_section(MailIndexRecord *rec, MailFetchBodyData *sect,
			     FetchContext *ctx)
{
	const char *prefix;

	prefix = !sect->skip_set ?
		t_strdup_printf(" BODY[%s]", sect->section) :
		t_strdup_printf(" BODY[%s]<%"PRIuUOFF_T">",
				sect->section, sect->skip);
	if (ctx->first) {
		prefix++; ctx->first = FALSE;
	}

	if (*sect->section == '\0')
		return fetch_body(rec, sect, ctx, prefix, TRUE);
	if (strcasecmp(sect->section, "TEXT") == 0)
		return fetch_body(rec, sect, ctx, prefix, FALSE);
	if (strncasecmp(sect->section, "HEADER", 6) == 0)
		return fetch_header(sect, ctx, prefix);
	if (*sect->section >= '0' && *sect->section <= '9')
		return fetch_part(sect, ctx, prefix);

	/* FIXME: point the error to user */
	return FALSE;
}
