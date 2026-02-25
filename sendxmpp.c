// Build (dynamic):
//   gcc sendxmpp.c $(pkg-config --cflags --libs libstrophe) \
//     -Os -ffunction-sections -fdata-sections -Wl,--gc-sections -s -o sendxmpp
//
// Quick uses:
//   # From .env: JID/PASS + TO or MUC/NICK
//   ./sendxmpp "hello from env"
//
//   # Pipe from stdin (dash optional)
//   echo "alert @ $(date)" | ./sendxmpp
//   echo "alert @ $(date)" | ./sendxmpp -
//
//   # Direct chat (one-shot)
//   ./sendxmpp JID PASS to@domain "hello"
//
//   # MUC (one-shot)
//   ./sendxmpp --muc room@conference.domain --nick Nick JID PASS "hello room"
//
//   # Interactive DM
//   ./sendxmpp -i JID PASS to@domain
//
//   # Interactive MUC
//   ./sendxmpp -i --muc room@conference.domain --nick Nick JID PASS
//
// Notes:
// - Uses system CA store (no cafile/capath calls) for broad compatibility.
// - Supports STARTTLS (default), --directtls (5223), --plaintext, --require-tls, --insecure.
// - Host/port override: -H / -P. SRV is used if host not provided.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <poll.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <strophe.h>

typedef enum { TLS_STARTTLS, TLS_DIRECTTLS, TLS_PLAINTEXT } tls_mode_t;

typedef struct {
    // connection
    const char *jid, *pass, *host;
    unsigned short port;
    tls_mode_t tls_mode;
    int require_tls, insecure, debug, interactive;

    // direct chat
    const char *to;

    // MUC
    const char *muc_jid; // room@conference.domain
    const char *nick;    // nickname
    bool muc_joined;

    // PubSub / IQ
    const char *pubsub_node;
    const char *pubsub_service;
    const char *pubsub_item;
    int pubsub_raw;
    int pubsub_append;
    int mode_fifo;
    const char *zinc_title;
    const char *zinc_category;
    const char *zinc_language;
    const char *zinc_tags;
    const char *zinc_author_name;
    const char *zinc_author_email;
    const char *zinc_generator;

    // runtime
    bool connected;
    bool one_shot_sent;
    const char *body; // one-shot message body (or buffer from stdin)

    // append buffer (text mode)
    char *append_buf;
    size_t append_len;
    size_t append_cap;
} app_t;

/* --------- tiny .env parser (key=value, # comments, quotes optional) ---------- */
static char *trim(char *s){
    while(*s && isspace((unsigned char)*s)) s++;
    if(!*s) return s;
    char *e = s + strlen(s) - 1;
    while(e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}
static void unquote(char *s){
    size_t n = strlen(s);
    if(n>=2 && ((s[0]=='"' && s[n-1]=='"') || (s[0]=='\'' && s[n-1]=='\''))) {
        s[n-1]=0; memmove(s, s+1, n-1);
    }
}
static void load_env_file(const char *path){
    FILE *f = fopen(path, "r");
    if(!f) return;
    char line[4096];
    while(fgets(line, sizeof(line), f)){
        char *p = line;
        char *hash = strchr(p, '#'); if(hash) *hash = 0;
        p = trim(p);
        if(*p==0) continue;
        char *eq = strchr(p, '=');
        if(!eq) continue;
        *eq = 0;
        char *k = trim(p);
        char *v = trim(eq+1);
        unquote(v);
        if(*k && *v) setenv(k, v, 0); // do not overwrite existing env
    }
    fclose(f);
}
static void try_load_default_envs(void){
    struct stat st;
    if(stat(".env", &st) == 0 && S_ISREG(st.st_mode)) load_env_file(".env");
    const char *home = getenv("HOME");
    if(home && *home){
        char path[1024];
        snprintf(path, sizeof(path), "%s/.sendxmpp.env", home);
        if(stat(path, &st) == 0 && S_ISREG(st.st_mode)) load_env_file(path);
    }
}

/* -------------------------- messaging helpers -------------------------- */
static void send_chat(xmpp_conn_t *conn, xmpp_ctx_t *ctx, const char *to, const char *text) {
    xmpp_stanza_t *msg = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(msg, "message");
    xmpp_stanza_set_type(msg, "chat");
    xmpp_stanza_set_attribute(msg, "to", to);

    xmpp_stanza_t *body = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(body, "body");
    xmpp_stanza_t *txt = xmpp_stanza_new(ctx);
    xmpp_stanza_set_text(txt, text);
    xmpp_stanza_add_child(body, txt);
    xmpp_stanza_add_child(msg, body);

    xmpp_send(conn, msg);
    xmpp_stanza_release(txt);
    xmpp_stanza_release(body);
    xmpp_stanza_release(msg);
}

static void send_groupchat(xmpp_conn_t *conn, xmpp_ctx_t *ctx, const char *room, const char *text) {
    xmpp_stanza_t *msg = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(msg, "message");
    xmpp_stanza_set_type(msg, "groupchat");
    xmpp_stanza_set_attribute(msg, "to", room);

    xmpp_stanza_t *body = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(body, "body");
    xmpp_stanza_t *txt = xmpp_stanza_new(ctx);
    xmpp_stanza_set_text(txt, text);
    xmpp_stanza_add_child(body, txt);
    xmpp_stanza_add_child(msg, body);

    xmpp_send(conn, msg);
    xmpp_stanza_release(txt);
    xmpp_stanza_release(body);
    xmpp_stanza_release(msg);
}

/* ---------------------------- PubSub helpers --------------------------- */
static char *xml_escape(const char *s) {
    if (!s) return strdup("");
    size_t len = 0;
    for (const char *p = s; *p; ++p) {
        switch (*p) {
            case '&': len += 5; break;  // &amp;
            case '<': len += 4; break;  // &lt;
            case '>': len += 4; break;  // &gt;
            case '"': len += 6; break;  // &quot;
            case '\'': len += 6; break; // &apos;
            default: len += 1; break;
        }
    }
    char *out = malloc(len + 1);
    if (!out) return NULL;
    char *w = out;
    for (const char *p = s; *p; ++p) {
        switch (*p) {
            case '&': memcpy(w, "&amp;", 5); w += 5; break;
            case '<': memcpy(w, "&lt;", 4); w += 4; break;
            case '>': memcpy(w, "&gt;", 4); w += 4; break;
            case '"': memcpy(w, "&quot;", 6); w += 6; break;
            case '\'': memcpy(w, "&apos;", 6); w += 6; break;
            default: *w++ = *p; break;
        }
    }
    *w = '\0';
    return out;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strbuf_t;

static int sb_reserve(strbuf_t *sb, size_t add) {
    if (sb->len + add + 1 <= sb->cap) return 0;
    size_t newcap = sb->cap ? sb->cap * 2 : 256;
    while (newcap < sb->len + add + 1) newcap *= 2;
    char *nb = realloc(sb->buf, newcap);
    if (!nb) return -1;
    sb->buf = nb;
    sb->cap = newcap;
    return 0;
}

static int sb_appendn(strbuf_t *sb, const char *s, size_t n) {
    if (!s || n == 0) return 0;
    if (sb_reserve(sb, n) != 0) return -1;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

static int sb_append(strbuf_t *sb, const char *s) {
    return sb_appendn(sb, s, s ? strlen(s) : 0);
}

static int sb_appendf(strbuf_t *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return -1;
    if (sb_reserve(sb, (size_t)need) != 0) return -1;
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)need;
    return 0;
}

static void sb_free(strbuf_t *sb) {
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
}

static void iso8601_now(char *buf, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int ms = (int)(ts.tv_nsec / 1000000);
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

static char *jid_bare(const char *jid) {
    if (!jid) return strdup("");
    const char *slash = strchr(jid, '/');
    size_t n = slash ? (size_t)(slash - jid) : strlen(jid);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, jid, n);
    out[n] = '\0';
    return out;
}

static char *first_line_trunc(const char *s, size_t maxlen) {
    if (!s) return strdup("");
    size_t n = 0;
    while (s[n] && s[n] != '\n' && s[n] != '\r' && n < maxlen) n++;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static int append_text(app_t *app, const char *line) {
    if (!line) return -1;
    size_t line_len = strlen(line);
    size_t add = line_len + (app->append_len ? 1 : 0);
    if (app->append_len + add + 1 > app->append_cap) {
        size_t newcap = app->append_cap ? app->append_cap * 2 : 4096;
        while (newcap < app->append_len + add + 1) newcap *= 2;
        char *nb = realloc(app->append_buf, newcap);
        if (!nb) return -1;
        app->append_buf = nb;
        app->append_cap = newcap;
    }
    if (app->append_len) app->append_buf[app->append_len++] = '\n';
    memcpy(app->append_buf + app->append_len, line, line_len);
    app->append_len += line_len;
    app->append_buf[app->append_len] = '\0';
    return 0;
}

static void make_iq_id(char *buf, size_t n) {
    static unsigned long counter = 0;
    unsigned long c = ++counter;
    snprintf(buf, n, "sx%lu-%lu", (unsigned long)getpid(), c);
}

static void append_tags(strbuf_t *cat_buf, strbuf_t *idx_tags, strbuf_t *tags_attr,
                        const char *tags_csv) {
    if (!tags_csv) return;
    const char *p = tags_csv;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        if (end > start) {
            size_t n = (size_t)(end - start);
            char *raw = malloc(n + 1);
            if (!raw) return;
            memcpy(raw, start, n);
            raw[n] = '\0';

            if (tags_attr->len) sb_append(tags_attr, ",");
            sb_append(tags_attr, raw);

            char *esc = xml_escape(raw);
            if (esc) {
                sb_appendf(cat_buf, "<category scheme=\"urn:zinc:workspace:index:tag\" term=\"%s\"/>", esc);
                sb_appendf(idx_tags, "<tag>%s</tag>", esc);
                free(esc);
            }
            free(raw);
        }
    }
}

static char *build_pubsub_payload_entry(const app_t *app, const char *text) {
    char ts[64];
    iso8601_now(ts, sizeof(ts));

    char *title_raw = app->zinc_title ? strdup(app->zinc_title) : first_line_trunc(text ? text : "", 120);
    if (!title_raw) return NULL;
    char *title = xml_escape(title_raw);
    free(title_raw);
    if (!title) return NULL;

    char *content = xml_escape(text ? text : "");
    if (!content) { free(title); return NULL; }

    char *author_name_raw = app->zinc_author_name ? strdup(app->zinc_author_name) : jid_bare(app->jid);
    char *author_email_raw = app->zinc_author_email ? strdup(app->zinc_author_email) : (app->jid ? strdup(app->jid) : strdup(""));
    if (!author_name_raw || !author_email_raw) { free(title); free(content); free(author_name_raw); free(author_email_raw); return NULL; }
    char *author_name = xml_escape(author_name_raw);
    char *author_email = xml_escape(author_email_raw);
    free(author_name_raw); free(author_email_raw);
    if (!author_name || !author_email) { free(title); free(content); free(author_name); free(author_email); return NULL; }

    const char *category_raw = app->zinc_category ? app->zinc_category : app->pubsub_node;
    const char *language_raw = app->zinc_language ? app->zinc_language : "text";
    char *category = category_raw ? xml_escape(category_raw) : NULL;
    char *language = language_raw ? xml_escape(language_raw) : NULL;

    const char *generator_raw = app->zinc_generator ? app->zinc_generator : "sendxmpp";
    char *generator = xml_escape(generator_raw);

    strbuf_t cat_buf = {0};
    strbuf_t idx_tags = {0};
    strbuf_t tags_attr = {0};
    if (category) sb_appendf(&cat_buf, "<category scheme=\"urn:zinc:workspace:index:category\" term=\"%s\"/>", category);
    if (language) sb_appendf(&cat_buf, "<category scheme=\"urn:zinc:workspace:index:language\" term=\"%s\"/>", language);
    append_tags(&cat_buf, &idx_tags, &tags_attr, app->zinc_tags);

    char *tags_attr_esc = tags_attr.len ? xml_escape(tags_attr.buf) : NULL;

    strbuf_t idx_attrs = {0};
    if (category && *category) sb_appendf(&idx_attrs, " category=\"%s\"", category);
    if (language && *language) sb_appendf(&idx_attrs, " language=\"%s\"", language);
    if (tags_attr_esc && *tags_attr_esc) sb_appendf(&idx_attrs, " tags=\"%s\"", tags_attr_esc);

    strbuf_t sb = {0};
    sb_append(&sb, "<entry xmlns=\"http://www.w3.org/2005/Atom\">");
    sb_appendf(&sb, "<title>%s</title>", title);
    sb_appendf(&sb, "<updated>%s</updated>", ts);
    sb_appendf(&sb, "<published>%s</published>", ts);
    sb_appendf(&sb, "<author><name>%s</name><email>%s</email></author>", author_name, author_email);
    if (cat_buf.len) sb_append(&sb, cat_buf.buf);
    if (idx_attrs.len || idx_tags.len) {
        sb_appendf(&sb, "<index xmlns=\"urn:zinc:workspace:index\"%s>", idx_attrs.buf ? idx_attrs.buf : "");
        if (idx_tags.len) sb_append(&sb, idx_tags.buf);
        sb_append(&sb, "</index>");
    }
    sb_appendf(&sb, "<content type=\"text\">%s</content>", content);
    if (generator) sb_appendf(&sb, "<generator>%s</generator>", generator);
    sb_append(&sb, "</entry>");

    free(title);
    free(content);
    free(author_name);
    free(author_email);
    free(category);
    free(language);
    free(generator);
    free(tags_attr_esc);
    sb_free(&cat_buf);
    sb_free(&idx_tags);
    sb_free(&tags_attr);
    sb_free(&idx_attrs);

    return sb.buf;
}

static char *build_pubsub_publish_iq(const app_t *app, const char *payload_xml) {
    char iq_id[64];
    make_iq_id(iq_id, sizeof(iq_id));
    char *to = xml_escape(app->pubsub_service);
    char *node = xml_escape(app->pubsub_node);
    char *item = xml_escape(app->pubsub_item);
    if (!to || !node || !item) { free(to); free(node); free(item); return NULL; }

    const char *tmpl =
        "<iq type='set' to='%s' id='%s'>"
        "<pubsub xmlns='http://jabber.org/protocol/pubsub'>"
        "<publish node='%s'>"
        "<item id='%s'>%s</item>"
        "</publish>"
        "</pubsub>"
        "</iq>";
    size_t len = snprintf(NULL, 0, tmpl, to, iq_id, node, item, payload_xml) + 1;
    char *out = malloc(len);
    if (!out) { free(to); free(node); free(item); return NULL; }
    snprintf(out, len, tmpl, to, iq_id, node, item, payload_xml);

    free(to); free(node); free(item);
    return out;
}

static void send_pubsub(xmpp_conn_t *conn, xmpp_ctx_t *ctx, app_t *app, const char *text) {
    (void)ctx;
    if (!app || !app->pubsub_node || !app->pubsub_service) return;
    const char *payload_src = text ? text : "";

    if (app->pubsub_append) {
        if (append_text(app, payload_src) != 0) return;
        payload_src = app->append_buf ? app->append_buf : "";
    }

    char *payload = NULL;
    const char *payload_xml = NULL;
    if (app->pubsub_raw) {
        payload_xml = payload_src;
    } else {
        payload = build_pubsub_payload_entry(app, payload_src);
        if (!payload) return;
        payload_xml = payload;
    }

    char *iq = build_pubsub_publish_iq(app, payload_xml);
    if (iq) {
        xmpp_send_raw_string(conn, iq);
        free(iq);
    }
    free(payload);
}

static void send_line(xmpp_conn_t *conn, xmpp_ctx_t *ctx, app_t *app, const char *line) {
    if (app->pubsub_node) send_pubsub(conn, ctx, app, line);
    else if (app->muc_jid) send_groupchat(conn, ctx, app->muc_jid, line);
    else                   send_chat(conn, ctx, app->to, line);
}

/* ------------------------------ MUC helpers ---------------------------- */
static void muc_leave(xmpp_conn_t *conn, xmpp_ctx_t *ctx, const char *room, const char *nick) {
    xmpp_stanza_t *pres = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(pres, "presence");
    xmpp_stanza_set_type(pres, "unavailable");
    char tobuf[512];
    snprintf(tobuf, sizeof(tobuf), "%s/%s", room, nick);
    xmpp_stanza_set_attribute(pres, "to", tobuf);
    xmpp_send(conn, pres);
    xmpp_stanza_release(pres);
}

static void muc_join(xmpp_conn_t *conn, xmpp_ctx_t *ctx, const char *room, const char *nick) {
    xmpp_stanza_t *pres = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(pres, "presence");

    char tobuf[512];
    snprintf(tobuf, sizeof(tobuf), "%s/%s", room, nick);
    xmpp_stanza_set_attribute(pres, "to", tobuf);

    xmpp_stanza_t *x = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(x, "x");
    xmpp_stanza_set_ns(x, "http://jabber.org/protocol/muc");

    xmpp_stanza_t *hist = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(hist, "history");
    xmpp_stanza_set_attribute(hist, "maxstanzas", "0");

    xmpp_stanza_add_child(x, hist);
    xmpp_stanza_add_child(pres, x);

    xmpp_send(conn, pres);

    xmpp_stanza_release(hist);
    xmpp_stanza_release(x);
    xmpp_stanza_release(pres);
}

static int muc_presence_handler(xmpp_conn_t *const conn, xmpp_stanza_t *const stanza, void *const userdata) {
    app_t *app = (app_t *)userdata;
    xmpp_ctx_t *ctx = xmpp_conn_get_context(conn);

    const char *from = xmpp_stanza_get_attribute(stanza, "from");
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    if (!from) return 1;

    size_t roomlen = strlen(app->muc_jid);
    if (strncmp(from, app->muc_jid, roomlen) == 0 && from[roomlen] == '/' && type == NULL) {
        const char *nick = from + roomlen + 1;
        if (app->nick && strcmp(nick, app->nick) == 0) {
            app->muc_joined = true;
            if (!app->interactive && app->body && !app->one_shot_sent) {
                send_groupchat(conn, ctx, app->muc_jid, app->body);
                app->one_shot_sent = true;
                muc_leave(conn, ctx, app->muc_jid, app->nick);
                xmpp_disconnect(conn);
            }
        }
    }
    return 1;
}

/* ---------------------------- Connection ------------------------------- */
static void on_conn(xmpp_conn_t *const conn, const xmpp_conn_event_t status,
                    const int err, xmpp_stream_error_t *const stream_error,
                    void *const userdata)
{
    app_t *app = (app_t *)userdata;
    xmpp_ctx_t *ctx = xmpp_conn_get_context(conn);

    if (status == XMPP_CONN_CONNECT) {
        app->connected = true;  // track state

        if (app->pubsub_node) {
            if (!app->interactive && !app->mode_fifo && app->body && !app->one_shot_sent) {
                send_pubsub(conn, ctx, app, app->body);
                app->one_shot_sent = true;
                xmpp_disconnect(conn);
            }
        } else if (app->muc_jid) {
            xmpp_handler_add(conn, muc_presence_handler, NULL, "presence", NULL, app);
            muc_join(conn, ctx, app->muc_jid, app->nick ? app->nick : "bot");
        } else {
            if (!app->interactive && app->to && app->body && !app->one_shot_sent) {
                send_chat(conn, ctx, app->to, app->body);
                app->one_shot_sent = true;
                xmpp_disconnect(conn);
            }
        }
    } else if (status == XMPP_CONN_DISCONNECT) {
        app->connected = false; // track state
        xmpp_stop(ctx);
    } else {
        fprintf(stderr, "Connection failed (%d)\n", err);
        xmpp_stop(ctx);
    }
}

/* ------------------------------- CLI ----------------------------------- */
static void usage(const char *p) {
    fprintf(stderr,
        "Usage (config one-shot): %s [opts] \"message\" | -\n"
        "Usage (direct one-shot): %s [opts] <jid> <pass> <to> <message>\n"
        "Usage (muc one-shot):    %s [opts] --muc <room@conference> --nick <nick> <jid> <pass> <message>\n"
        "Usage (pubsub one-shot): %s [opts] --pubsub <node> \"xml|text\" | -\n"
        "Usage (REPL, direct):    %s -i [opts] <jid> <pass> <to>\n"
        "Usage (REPL, muc):       %s -i [opts] --muc <room@conference> --nick <nick> <jid> <pass>\n\n"
        "Usage (REPL, pubsub):    %s -i [opts] --pubsub <node> <jid> <pass>\n"
        "Usage (stream, pubsub):  %s [opts] --pubsub <node> --mode fifo\n\n"
        "Env keys (.env or ~/.sendxmpp.env): JID, PASS, TO, MUC, NICK, HOST, PORT, TLS, REQUIRE_TLS, INSECURE, INTERACTIVE,\n"
        "  PUBSUB_SERVICE, PUBSUB_NODE, PUBSUB_ITEM, PUBSUB_RAW, PUBSUB_APPEND, MODE,\n"
        "  TITLE, CATEGORY, LANGUAGE, TAGS, AUTHOR_NAME, AUTHOR_EMAIL, GENERATOR\n"
        "Options:\n"
        "  --config <file>        Load key=val from file (like .env). CLI overrides env.\n"
        "  -i, --interactive      Keep connection open; read lines from stdin and send\n"
        "  -H, --host <host>      Override/fallback host (SRV is default)\n"
        "  -P, --port <port>      Override/fallback port (5222 default; 5223 if --directtls)\n"
        "      --starttls | --directtls | --plaintext\n"
        "      --require-tls      Fail if TLS not negotiated\n"
        "      --insecure         Trust any TLS certificate (NOT for prod)\n"
        "      --muc <roomjid>    Send to MUC; joins room first\n"
        "      --nick <nickname>  Nickname to use in MUC\n"
        "      --pubsub <node>    Publish to PubSub node via IQ\n"
        "      --pubsub-service <jid>  PubSub service JID (PUBSUB_SERVICE)\n"
        "      --item <id>        PubSub item id (default: main)\n"
        "      --raw-xml          Treat input as raw XML payload (no wrapping)\n"
        "      --append           Append text into a single item (text mode only)\n"
        "      --title <text>     Atom entry title (TITLE)\n"
        "      --category <term>  Atom category term (CATEGORY)\n"
        "      --language <term>  Atom language term (LANGUAGE)\n"
        "      --tags <csv>       Atom tags (TAGS, comma-separated)\n"
        "      --author-name <n>  Atom author name (AUTHOR_NAME)\n"
        "      --author-email <e> Atom author email (AUTHOR_EMAIL)\n"
        "      --generator <text> Atom generator (GENERATOR)\n"
        "      --mode <fifo>      Stream stdin line-by-line (no prompt)\n"
        "  -d, --debug            Verbose logging\n", p,p,p,p,p,p,p,p);
}

static int streq(const char *a, const char *b){ return a && b && strcmp(a,b)==0; }

/* ---- stdin helper: read all, trim trailing newlines ---- */
static char *read_all_stdin_trimmed(void){
    char *buf=NULL; size_t cap=0,len=0; int c;
    while ((c=fgetc(stdin))!=EOF){
        if (len+1>=cap){
            cap=cap?cap*2:4096;
            char *nb=realloc(buf,cap);
            if(!nb){ free(buf); return NULL; }
            buf=nb;
        }
        buf[len++]=(char)c;
    }
    if (!buf){ return NULL; }
    while (len>0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) len--;
    buf[len]='\0';
    return buf;
}

int main(int argc, char **argv)
{
    try_load_default_envs();

    const char *cfgfile = NULL;
    app_t app; memset(&app, 0, sizeof(app));
    app.tls_mode = TLS_STARTTLS;

    // Preload from ENV (can be overridden by CLI)
    const char *e;
    if((e=getenv("HOST"))) app.host=e;
    if((e=getenv("PORT"))) app.port=(unsigned short)atoi(e);
    if((e=getenv("TLS"))) {
        if(!strcasecmp(e,"directtls")) app.tls_mode = TLS_DIRECTTLS;
        else if(!strcasecmp(e,"plaintext")) app.tls_mode = TLS_PLAINTEXT;
        else app.tls_mode = TLS_STARTTLS;
    }
    if((e=getenv("REQUIRE_TLS"))) app.require_tls = atoi(e)!=0;
    if((e=getenv("INSECURE")))    app.insecure    = atoi(e)!=0;
    if((e=getenv("INTERACTIVE"))) app.interactive = atoi(e)!=0;
    if((e=getenv("MUC")))  app.muc_jid=e;
    if((e=getenv("NICK"))) app.nick=e;
    if((e=getenv("TO")))   app.to=e;
    if((e=getenv("PUBSUB_SERVICE"))) app.pubsub_service=e;
    if((e=getenv("PUBSUB_NODE")))    app.pubsub_node=e;
    if((e=getenv("PUBSUB_ITEM")))    app.pubsub_item=e;
    if((e=getenv("PUBSUB_RAW")))     app.pubsub_raw = atoi(e)!=0;
    if((e=getenv("PUBSUB_APPEND")))  app.pubsub_append = atoi(e)!=0;
    if((e=getenv("MODE")) && !strcasecmp(e,"fifo")) app.mode_fifo = 1;
    if((e=getenv("TITLE")) || (e=getenv("ZINC_TITLE")))        app.zinc_title = e;
    if((e=getenv("CATEGORY")) || (e=getenv("ZINC_CATEGORY")))  app.zinc_category = e;
    if((e=getenv("LANGUAGE")) || (e=getenv("ZINC_LANGUAGE")))  app.zinc_language = e;
    if((e=getenv("TAGS")) || (e=getenv("ZINC_TAGS")))          app.zinc_tags = e;
    if((e=getenv("AUTHOR_NAME")) || (e=getenv("ZINC_AUTHOR_NAME")))  app.zinc_author_name = e;
    if((e=getenv("AUTHOR_EMAIL")) || (e=getenv("ZINC_AUTHOR_EMAIL"))) app.zinc_author_email = e;
    if((e=getenv("GENERATOR")) || (e=getenv("ZINC_GENERATOR"))) app.zinc_generator = e;
    if((e=getenv("JID")))  app.jid=e;
    if((e=getenv("PASS"))) app.pass=e;

    // Parse CLI (overrides env)
    int i=1;
    for(; i<argc; ++i){
        const char *a=argv[i];
        if(a[0] != '-') break;
        if(streq(a,"--config")) { if(++i>=argc){usage(argv[0]);return 2;} cfgfile=argv[i]; }
        else if(streq(a,"-i")||streq(a,"--interactive")) { app.interactive = 1; }
        else if(streq(a,"-H")||streq(a,"--host")) { if(++i>=argc){usage(argv[0]);return 2;} app.host=argv[i]; }
        else if(streq(a,"-P")||streq(a,"--port")) { if(++i>=argc){usage(argv[0]);return 2;} app.port=(unsigned short)atoi(argv[i]); }
        else if(streq(a,"--starttls"))   { app.tls_mode = TLS_STARTTLS; }
        else if(streq(a,"--directtls"))  { app.tls_mode = TLS_DIRECTTLS; }
        else if(streq(a,"--plaintext"))  { app.tls_mode = TLS_PLAINTEXT; }
        else if(streq(a,"--require-tls")){ app.require_tls = 1; }
        else if(streq(a,"--insecure"))   { app.insecure = 1; }
        else if(streq(a,"--muc"))        { if(++i>=argc){usage(argv[0]);return 2;} app.muc_jid = argv[i]; }
        else if(streq(a,"--nick"))       { if(++i>=argc){usage(argv[0]);return 2;} app.nick   = argv[i]; }
        else if(streq(a,"--pubsub"))     { if(++i>=argc){usage(argv[0]);return 2;} app.pubsub_node = argv[i]; }
        else if(streq(a,"--pubsub-service")) { if(++i>=argc){usage(argv[0]);return 2;} app.pubsub_service = argv[i]; }
        else if(streq(a,"--item"))       { if(++i>=argc){usage(argv[0]);return 2;} app.pubsub_item = argv[i]; }
        else if(streq(a,"--raw-xml")||streq(a,"--xml")) { app.pubsub_raw = 1; }
        else if(streq(a,"--append"))     { app.pubsub_append = 1; }
        else if(streq(a,"--title"))      { if(++i>=argc){usage(argv[0]);return 2;} app.zinc_title = argv[i]; }
        else if(streq(a,"--category"))   { if(++i>=argc){usage(argv[0]);return 2;} app.zinc_category = argv[i]; }
        else if(streq(a,"--language"))   { if(++i>=argc){usage(argv[0]);return 2;} app.zinc_language = argv[i]; }
        else if(streq(a,"--tags"))       { if(++i>=argc){usage(argv[0]);return 2;} app.zinc_tags = argv[i]; }
        else if(streq(a,"--author-name")){ if(++i>=argc){usage(argv[0]);return 2;} app.zinc_author_name = argv[i]; }
        else if(streq(a,"--author-email")){ if(++i>=argc){usage(argv[0]);return 2;} app.zinc_author_email = argv[i]; }
        else if(streq(a,"--generator"))  { if(++i>=argc){usage(argv[0]);return 2;} app.zinc_generator = argv[i]; }
        else if(streq(a,"--mode")) {
            if(++i>=argc){usage(argv[0]);return 2;}
            if(!strcasecmp(argv[i],"fifo")) app.mode_fifo = 1;
            else { fprintf(stderr,"Unknown mode: %s\n", argv[i]); return 2; }
        }
        else if(streq(a,"--fifo"))       { app.mode_fifo = 1; }
        else if(streq(a,"-d")||streq(a,"--debug")) { app.debug = 1; }
        else if(streq(a,"--")) { ++i; break; }
        else { usage(argv[0]); return 2; }
    }

    // If --config provided, load it (doesn't overwrite existing env), then re-pull env fallbacks for unset fields.
    if(cfgfile) {
        load_env_file(cfgfile);
        if(!app.host && (e=getenv("HOST"))) app.host=e;
        if(!app.port && (e=getenv("PORT"))) app.port=(unsigned short)atoi(e);
        if(!app.muc_jid && (e=getenv("MUC"))) app.muc_jid=e;
        if(!app.nick && (e=getenv("NICK"))) app.nick=e;
        if(!app.to && (e=getenv("TO"))) app.to=e;
        if(!app.pubsub_service && (e=getenv("PUBSUB_SERVICE"))) app.pubsub_service=e;
        if(!app.pubsub_node && (e=getenv("PUBSUB_NODE"))) app.pubsub_node=e;
        if(!app.pubsub_item && (e=getenv("PUBSUB_ITEM"))) app.pubsub_item=e;
        if(!app.zinc_title && ((e=getenv("TITLE")) || (e=getenv("ZINC_TITLE")))) app.zinc_title = e;
        if(!app.zinc_category && ((e=getenv("CATEGORY")) || (e=getenv("ZINC_CATEGORY")))) app.zinc_category = e;
        if(!app.zinc_language && ((e=getenv("LANGUAGE")) || (e=getenv("ZINC_LANGUAGE")))) app.zinc_language = e;
        if(!app.zinc_tags && ((e=getenv("TAGS")) || (e=getenv("ZINC_TAGS")))) app.zinc_tags = e;
        if(!app.zinc_author_name && ((e=getenv("AUTHOR_NAME")) || (e=getenv("ZINC_AUTHOR_NAME")))) app.zinc_author_name = e;
        if(!app.zinc_author_email && ((e=getenv("AUTHOR_EMAIL")) || (e=getenv("ZINC_AUTHOR_EMAIL")))) app.zinc_author_email = e;
        if(!app.zinc_generator && ((e=getenv("GENERATOR")) || (e=getenv("ZINC_GENERATOR")))) app.zinc_generator = e;
        if(!app.jid && (e=getenv("JID"))) app.jid=e;
        if(!app.pass && (e=getenv("PASS"))) app.pass=e;
        if((e=getenv("TLS"))){
            if(!strcasecmp(e,"directtls")) app.tls_mode = TLS_DIRECTTLS;
            else if(!strcasecmp(e,"plaintext")) app.tls_mode = TLS_PLAINTEXT;
            else app.tls_mode = TLS_STARTTLS;
        }
        if(!app.require_tls && (e=getenv("REQUIRE_TLS"))) app.require_tls = atoi(e)!=0;
        if(!app.insecure    && (e=getenv("INSECURE")))    app.insecure    = atoi(e)!=0;
        if(!app.interactive && (e=getenv("INTERACTIVE"))) app.interactive = atoi(e)!=0;
        if(!app.pubsub_raw && (e=getenv("PUBSUB_RAW"))) app.pubsub_raw = atoi(e)!=0;
        if(!app.pubsub_append && (e=getenv("PUBSUB_APPEND"))) app.pubsub_append = atoi(e)!=0;
        if(!app.mode_fifo && (e=getenv("MODE")) && !strcasecmp(e,"fifo")) app.mode_fifo = 1;
    }

    if (app.muc_jid && app.pubsub_node) {
        fprintf(stderr,"--muc and --pubsub are mutually exclusive.\n");
        return 2;
    }
    if (app.mode_fifo && app.interactive) {
        fprintf(stderr,"--mode fifo and --interactive cannot be used together.\n");
        return 2;
    }
    if (app.pubsub_node && !app.pubsub_item) app.pubsub_item = "main";
    if (app.pubsub_append && app.pubsub_raw) {
        fprintf(stderr,"--append is only supported for text payloads (disable --raw-xml).\n");
        return 2;
    }

    bool use_pubsub = app.pubsub_node != NULL;
    bool stream = app.interactive || app.mode_fifo;

    // Determine arg shape.
    // Priority:
    //   1) If exactly one positional left: it's the message (or "-" for stdin).
    //   2) Else if no message arg but stdin is piped: read stdin automatically.
    //   3) Else fall back to classic DM/MUC/PubSub argument shapes.
    if (!stream) {
        if (argc - i == 1) {
            if (strcmp(argv[i], "-") == 0) {
                app.body = read_all_stdin_trimmed();
                if (!app.body){ fprintf(stderr,"No stdin provided\n"); return 2; }
            } else {
                app.body = argv[i];
            }
        } else if ((argc - i) == 0 && !isatty(STDIN_FILENO)) {
            app.body = read_all_stdin_trimmed();
            if (!app.body){ fprintf(stderr,"No stdin provided\n"); return 2; }
        } else {
            if (use_pubsub) {
                if (!app.jid && i < argc) app.jid = argv[i++];
                if (!app.pass && i < argc) app.pass = argv[i++];
                if (!app.body && i < argc) app.body = argv[i++];
            } else if (app.muc_jid) {
                if (!app.jid && i < argc) app.jid = argv[i++];
                if (!app.pass && i < argc) app.pass = argv[i++];
                if (!app.body && i < argc) app.body = argv[i++];
            } else {
                if (!app.jid && i < argc) app.jid = argv[i++];
                if (!app.pass && i < argc) app.pass = argv[i++];
                if (!app.to && i < argc) app.to = argv[i++];
                if (!app.body && i < argc) app.body = argv[i++];
            }
        }
    } else {
        if (use_pubsub) {
            if (!app.jid && i < argc) app.jid = argv[i++];
            if (!app.pass && i < argc) app.pass = argv[i++];
        } else if (app.muc_jid) {
            if (!app.jid && i < argc) app.jid = argv[i++];
            if (!app.pass && i < argc) app.pass = argv[i++];
        } else {
            if (!app.jid && i < argc) app.jid = argv[i++];
            if (!app.pass && i < argc) app.pass = argv[i++];
            if (!app.to && i < argc) app.to = argv[i++];
        }
    }

    if (!app.jid || !app.pass) {
        fprintf(stderr,"JID/PASS must be set (env/config or CLI).\n");
        return 2;
    }
    if (use_pubsub) {
        if (!app.pubsub_service) { fprintf(stderr,"PUBSUB_SERVICE or --pubsub-service is required.\n"); return 2; }
        if (!app.pubsub_node)    { fprintf(stderr,"PUBSUB_NODE or --pubsub is required.\n"); return 2; }
        if (!stream && !app.body){ fprintf(stderr,"Provide a payload (arg or stdin) for PubSub.\n"); return 2; }
    } else if (app.muc_jid) {
        if (!app.nick) { fprintf(stderr,"--nick or NICK= required for MUC.\n"); return 2; }
        if (!stream && !app.body) { fprintf(stderr,"Provide a message for MUC.\n"); return 2; }
    } else {
        if (!app.to)   { fprintf(stderr,"Provide TO=… or <to> argument.\n"); return 2; }
        if (!stream && !app.body) { fprintf(stderr,"Provide a message.\n"); return 2; }
    }

    long flags = 0;
    switch (app.tls_mode) {
        case TLS_PLAINTEXT: flags |= XMPP_CONN_FLAG_DISABLE_TLS; break;
        case TLS_DIRECTTLS: flags |= XMPP_CONN_FLAG_LEGACY_SSL; if (app.port==0) app.port=5223; break;
        case TLS_STARTTLS: default: break;
    }
    if (app.require_tls) flags |= XMPP_CONN_FLAG_MANDATORY_TLS;
    if (app.insecure)    flags |= XMPP_CONN_FLAG_TRUST_TLS;
    if (app.port == 0) app.port = 5222;

    // Init & connect
    xmpp_initialize();
    xmpp_log_t *logger = xmpp_get_default_logger(app.debug ? XMPP_LEVEL_DEBUG : XMPP_LEVEL_ERROR);
    xmpp_ctx_t *ctx = xmpp_ctx_new(NULL, logger);
    if (!ctx) { fprintf(stderr,"ctx alloc failed\n"); return 1; }

    xmpp_conn_t *conn = xmpp_conn_new(ctx);
    if (!conn) { fprintf(stderr,"conn alloc failed\n"); xmpp_ctx_free(ctx); return 1; }
    if (xmpp_conn_set_flags(conn, flags) != XMPP_EOK) {
        fprintf(stderr,"Failed to set connection flags\n");
        xmpp_conn_release(conn); xmpp_ctx_free(ctx); return 1;
    }
    xmpp_conn_set_jid(conn, app.jid);
    xmpp_conn_set_pass(conn, app.pass);

    if (xmpp_connect_client(conn, app.host, app.port, on_conn, &app) != XMPP_EOK) {
        fprintf(stderr,"xmpp_connect_client failed to start\n");
        xmpp_conn_release(conn); xmpp_ctx_free(ctx); xmpp_shutdown(); return 1;
    }

    if (!app.interactive && !app.mode_fifo) {
        xmpp_run(ctx);
    } else if (app.mode_fifo) {
        struct pollfd fds[1]; fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN;
        char line[4096];
        while (1) {
            xmpp_run_once(ctx, 50);

            int r = poll(fds, 1, 1000);
            if (r > 0 && (fds[0].revents & POLLIN)) {
                if (!fgets(line, sizeof(line), stdin)) {
                    xmpp_disconnect(conn);
                    while (app.connected) xmpp_run_once(ctx, 50);
                    break;
                }
                size_t L = strlen(line);
                if (L && (line[L-1]=='\n'||line[L-1]=='\r')) line[L-1]=0;
                if (!app.connected) continue;
                send_line(conn, ctx, &app, line);
            }
        }
    } else {
        struct pollfd fds[1]; fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN;
        char line[4096];
        while (1) {
            xmpp_run_once(ctx, 50);

            fprintf(stdout, "> "); fflush(stdout);
            int r = poll(fds, 1, 10000);
            if (r > 0 && (fds[0].revents & POLLIN)) {
                if (!fgets(line, sizeof(line), stdin)) break;
                size_t L = strlen(line);
                if (L && (line[L-1]=='\n'||line[L-1]=='\r')) line[L-1]=0;
                if (!strcmp(line,"/quit")) {
                    if (app.muc_jid && app.nick) muc_leave(conn, ctx, app.muc_jid, app.nick);
                    xmpp_disconnect(conn);
                    while (app.connected) xmpp_run_once(ctx, 50);
                    break;
                }
                if (!app.connected) continue;
                send_line(conn, ctx, &app, line);
            }
        }
    }

    xmpp_conn_release(conn);
    xmpp_ctx_free(ctx);
    xmpp_shutdown();
    free(app.append_buf);
    return 0;
}
