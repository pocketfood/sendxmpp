#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <strophe.h>

/*
 * XEP-0114 external component transport is not the normal XMPP client
 * STARTTLS flow. Protect this connection externally, for example with a
 * private network, VPN, SSH tunnel, or another secured transport.
 */

typedef struct {
    const char *host;
    unsigned short port;
    const char *domain;
    const char *secret;
    const char *from;
    const char *to;
    int fifo;
    int debug;

    /* Optional PubSub settings retained from the client version. */
    const char *pubsub_node;
    const char *pubsub_service;
    const char *pubsub_item;
    int pubsub_raw;
    int pubsub_append;
    const char *entry_title;
    const char *entry_category;
    const char *entry_language;
    const char *entry_tags;
    const char *entry_author_name;
    const char *entry_author_email;
    const char *entry_generator;

    bool connected;
    bool ever_connected;
    bool one_shot_sent;
    bool connect_failed;
    const char *body;
    bool body_allocated;
    char *append_buf;
    size_t append_len;
    size_t append_cap;
} app_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strbuf_t;

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static char *trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static void unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 &&
        ((s[0] == '"' && s[n - 1] == '"') ||
         (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        memmove(s, s + 1, n - 1);
    }
}

static int load_env_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096];

    if (!f) {
        fprintf(stderr, "Cannot open config '%s': %s\n", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        char *eq;
        char *hash;
        char *key;
        char *value;

        if (!*p || *p == '#') continue;
        hash = strchr(p, '#');
        if (hash) *hash = '\0';
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        key = trim(p);
        value = trim(eq + 1);
        unquote(value);
        if (*key && *value) setenv(key, value, 0);
    }
    fclose(f);
    return 0;
}

static int sb_reserve(strbuf_t *sb, size_t add)
{
    size_t cap;
    char *next;

    if (sb->len + add + 1 <= sb->cap) return 0;
    cap = sb->cap ? sb->cap * 2 : 256;
    while (cap < sb->len + add + 1) cap *= 2;
    next = realloc(sb->buf, cap);
    if (!next) return -1;
    sb->buf = next;
    sb->cap = cap;
    return 0;
}

static int sb_appendn(strbuf_t *sb, const char *s, size_t n)
{
    if (!s || !n) return 0;
    if (sb_reserve(sb, n) != 0) return -1;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

static int sb_append(strbuf_t *sb, const char *s)
{
    return sb_appendn(sb, s, s ? strlen(s) : 0);
}

static int sb_appendf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    int needed;

    va_start(ap, fmt);
    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0 || sb_reserve(sb, (size_t)needed) != 0) return -1;
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)needed;
    return 0;
}

static void sb_free(strbuf_t *sb)
{
    free(sb->buf);
    memset(sb, 0, sizeof(*sb));
}

static char *xml_escape(const char *s)
{
    strbuf_t out = {0};
    const char *p;

    if (!s) return strdup("");
    for (p = s; *p; ++p) {
        const char *replacement = NULL;
        switch (*p) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&apos;"; break;
        default:
            if (sb_appendn(&out, p, 1) != 0) goto fail;
            continue;
        }
        if (sb_append(&out, replacement) != 0) goto fail;
    }
    if (!out.buf) return strdup("");
    return out.buf;
fail:
    sb_free(&out);
    return NULL;
}

static void iso8601_now(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static char *first_line_trunc(const char *s, size_t maxlen)
{
    size_t n = 0;
    char *out;
    if (!s) s = "";
    while (s[n] && s[n] != '\n' && s[n] != '\r' && n < maxlen) n++;
    out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static int append_text(app_t *app, const char *line)
{
    size_t line_len = strlen(line);
    size_t add = line_len + (app->append_len ? 1 : 0);
    char *next;
    size_t cap;

    if (app->append_len + add + 1 > app->append_cap) {
        cap = app->append_cap ? app->append_cap * 2 : 4096;
        while (cap < app->append_len + add + 1) cap *= 2;
        next = realloc(app->append_buf, cap);
        if (!next) return -1;
        app->append_buf = next;
        app->append_cap = cap;
    }
    if (app->append_len) app->append_buf[app->append_len++] = '\n';
    memcpy(app->append_buf + app->append_len, line, line_len + 1);
    app->append_len += line_len;
    return 0;
}

static void append_tags(strbuf_t *categories, const char *csv)
{
    const char *p = csv;
    if (!p) return;
    while (*p) {
        const char *start;
        const char *end;
        char *raw;
        char *escaped;
        size_t n;

        while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
        start = p;
        while (*p && *p != ',') p++;
        end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        n = (size_t)(end - start);
        if (!n) continue;
        raw = malloc(n + 1);
        if (!raw) return;
        memcpy(raw, start, n);
        raw[n] = '\0';
        escaped = xml_escape(raw);
        free(raw);
        if (!escaped) return;
        sb_appendf(categories, "<category term=\"%s\"/>", escaped);
        free(escaped);
    }
}

static char *build_pubsub_payload_entry(const app_t *app, const char *text)
{
    char timestamp[32];
    char *title_raw = NULL;
    char *title = NULL;
    char *content = NULL;
    char *author = NULL;
    char *email = NULL;
    char *category = NULL;
    char *language = NULL;
    char *generator = NULL;
    strbuf_t categories = {0};
    strbuf_t out = {0};

    iso8601_now(timestamp, sizeof(timestamp));
    title_raw = app->entry_title ? strdup(app->entry_title)
                                 : first_line_trunc(text, 120);
    title = xml_escape(title_raw);
    content = xml_escape(text);
    author = xml_escape(app->entry_author_name ? app->entry_author_name
                                               : app->from);
    email = xml_escape(app->entry_author_email ? app->entry_author_email : "");
    category = app->entry_category ? xml_escape(app->entry_category) : NULL;
    language = xml_escape(app->entry_language ? app->entry_language : "en");
    generator = xml_escape(app->entry_generator ? app->entry_generator
                                                 : "sendxmpp-component");
    free(title_raw);
    if (!title || !content || !author || !email || !language || !generator)
        goto fail;

    if (category)
        sb_appendf(&categories, "<category term=\"%s\"/>", category);
    append_tags(&categories, app->entry_tags);

    if (sb_append(&out, "<entry xmlns=\"http://www.w3.org/2005/Atom\">") ||
        sb_appendf(&out, "<title>%s</title>", title) ||
        sb_appendf(&out, "<updated>%s</updated>", timestamp) ||
        sb_appendf(&out, "<published>%s</published>", timestamp) ||
        sb_appendf(&out, "<author><name>%s</name><email>%s</email></author>",
                   author, email) ||
        (categories.len && sb_append(&out, categories.buf)) ||
        sb_appendf(&out, "<content type=\"text\" xml:lang=\"%s\">%s</content>",
                   language, content) ||
        sb_appendf(&out, "<generator>%s</generator>", generator) ||
        sb_append(&out, "</entry>"))
        goto fail;

    free(title); free(content); free(author); free(email);
    free(category); free(language); free(generator);
    sb_free(&categories);
    return out.buf;
fail:
    free(title); free(content); free(author); free(email);
    free(category); free(language); free(generator);
    sb_free(&categories);
    sb_free(&out);
    return NULL;
}

static void make_iq_id(char *buf, size_t size)
{
    static unsigned long counter;
    snprintf(buf, size, "component-%lu-%lu",
             (unsigned long)getpid(), ++counter);
}

static char *build_pubsub_publish_iq(const app_t *app, const char *payload)
{
    char id[64];
    char *from = xml_escape(app->from);
    char *to = xml_escape(app->pubsub_service);
    char *node = xml_escape(app->pubsub_node);
    char *item = xml_escape(app->pubsub_item);
    strbuf_t out = {0};

    make_iq_id(id, sizeof(id));
    if (!from || !to || !node || !item ||
        sb_appendf(&out,
            "<iq type='set' from='%s' to='%s' id='%s'>"
            "<pubsub xmlns='http://jabber.org/protocol/pubsub'>"
            "<publish node='%s'><item id='%s'>%s</item></publish>"
            "</pubsub></iq>",
            from, to, id, node, item, payload ? payload : "") != 0) {
        sb_free(&out);
    }
    free(from); free(to); free(node); free(item);
    return out.buf;
}

static int send_pubsub(xmpp_conn_t *conn, app_t *app, const char *text)
{
    const char *source = text ? text : "";
    const char *payload;
    char *owned_payload = NULL;
    char *iq;

    if (app->pubsub_append) {
        if (append_text(app, source) != 0) return -1;
        source = app->append_buf;
    }
    if (app->pubsub_raw) {
        payload = source;
    } else {
        owned_payload = build_pubsub_payload_entry(app, source);
        if (!owned_payload) return -1;
        payload = owned_payload;
    }
    iq = build_pubsub_publish_iq(app, payload);
    free(owned_payload);
    if (!iq) return -1;
    xmpp_send_raw_string(conn, "%s", iq);
    free(iq);
    return 0;
}

static int send_message(xmpp_conn_t *conn, xmpp_ctx_t *ctx,
                        const app_t *app, const char *text)
{
    xmpp_stanza_t *message = NULL;
    xmpp_stanza_t *body = NULL;
    xmpp_stanza_t *body_text = NULL;
    int rc = -1;

    message = xmpp_stanza_new(ctx);
    body = xmpp_stanza_new(ctx);
    body_text = xmpp_stanza_new(ctx);
    if (!message || !body || !body_text) goto done;
    if (xmpp_stanza_set_name(message, "message") != XMPP_EOK ||
        xmpp_stanza_set_type(message, "chat") != XMPP_EOK ||
        xmpp_stanza_set_attribute(message, "from", app->from) != XMPP_EOK ||
        xmpp_stanza_set_attribute(message, "to", app->to) != XMPP_EOK ||
        xmpp_stanza_set_name(body, "body") != XMPP_EOK ||
        xmpp_stanza_set_text(body_text, text ? text : "") != XMPP_EOK ||
        xmpp_stanza_add_child(body, body_text) != XMPP_EOK ||
        xmpp_stanza_add_child(message, body) != XMPP_EOK)
        goto done;
    xmpp_send(conn, message);
    rc = 0;
done:
    if (body_text) xmpp_stanza_release(body_text);
    if (body) xmpp_stanza_release(body);
    if (message) xmpp_stanza_release(message);
    return rc;
}

static int send_line(xmpp_conn_t *conn, xmpp_ctx_t *ctx,
                     app_t *app, const char *line)
{
    if (app->pubsub_node) return send_pubsub(conn, app, line);
    return send_message(conn, ctx, app, line);
}

static void print_field(const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    for (; *p; ++p) {
        switch (*p) {
        case '\\': fputs("\\\\", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        default: fputc(*p, stdout); break;
        }
    }
}

static int message_handler(xmpp_conn_t *const conn,
                           xmpp_stanza_t *const stanza,
                           void *const userdata)
{
    xmpp_ctx_t *ctx = xmpp_conn_get_context(conn);
    xmpp_stanza_t *body_stanza = xmpp_stanza_get_child_by_name(stanza, "body");
    char *body = body_stanza ? xmpp_stanza_get_text(body_stanza) : NULL;
    const char *from = xmpp_stanza_get_attribute(stanza, "from");
    const char *to = xmpp_stanza_get_attribute(stanza, "to");
    const char *type = xmpp_stanza_get_type(stanza);
    (void)userdata;

    fputs("IN\t", stdout); print_field(from);
    fputc('\t', stdout); print_field(to);
    fputc('\t', stdout); print_field(type);
    fputc('\t', stdout); print_field(body);
    fputc('\n', stdout);
    fflush(stdout);
    if (body) xmpp_free(ctx, body);
    return 1;
}

static void connection_handler(xmpp_conn_t *const conn,
                               const xmpp_conn_event_t status,
                               const int error,
                               xmpp_stream_error_t *const stream_error,
                               void *const userdata)
{
    app_t *app = userdata;
    xmpp_ctx_t *ctx = xmpp_conn_get_context(conn);

    if (status == XMPP_CONN_CONNECT) {
        app->connected = true;
        app->ever_connected = true;
        fprintf(stderr, "Component authenticated as %s\n", app->domain);
        xmpp_handler_add(conn, message_handler, NULL, "message", NULL, app);
        if (!app->fifo && app->body && !app->one_shot_sent) {
            if (send_line(conn, ctx, app, app->body) != 0) {
                fprintf(stderr, "Failed to build outgoing stanza\n");
                app->connect_failed = true;
            }
            app->one_shot_sent = true;
            xmpp_disconnect(conn);
        }
    } else {
        app->connected = false;
        if (!app->ever_connected) {
            fprintf(stderr,
                    "Component connection or XEP-0114 handshake failed"
                    " (error=%d%s)\n",
                    error, stream_error ? ", stream error received" : "");
            app->connect_failed = true;
        } else if (error != 0 || stream_error) {
            fprintf(stderr, "Component disconnected with error=%d%s\n",
                    error, stream_error ? " (stream error)" : "");
            app->connect_failed = true;
        }
        xmpp_stop(ctx);
    }
}

static bool from_matches_domain(const char *from, const char *domain)
{
    const char *at;
    const char *start;
    const char *slash;
    size_t length;

    if (!from || !domain || !*from || !*domain) return false;
    at = strrchr(from, '@');
    start = at ? at + 1 : from;
    slash = strchr(start, '/');
    length = slash ? (size_t)(slash - start) : strlen(start);
    return strlen(domain) == length && strncmp(start, domain, length) == 0;
}

static int parse_port(const char *text, unsigned short *port)
{
    char *end;
    unsigned long value;
    if (!text || !*text || *text == '-') return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value < 1 || value > 65535) return -1;
    *port = (unsigned short)value;
    return 0;
}

static char *read_all_stdin(void)
{
    strbuf_t out = {0};
    char block[4096];
    size_t count;
    while ((count = fread(block, 1, sizeof(block), stdin)) > 0) {
        if (sb_appendn(&out, block, count) != 0) {
            sb_free(&out);
            return NULL;
        }
    }
    while (out.len && (out.buf[out.len - 1] == '\n' ||
                       out.buf[out.len - 1] == '\r'))
        out.buf[--out.len] = '\0';
    return out.buf;
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s [options] [message|-]\n\n"
        "XEP-0114 external component options:\n"
        "  --host <host>       Component listener host (COMPONENT_HOST)\n"
        "  --port <port>       Component listener port (COMPONENT_PORT)\n"
        "  --component <name>  Component domain (COMPONENT_DOMAIN)\n"
        "  --secret <secret>   Shared component secret (COMPONENT_SECRET)\n"
        "  --from <jid>        Component-owned sender (COMPONENT_FROM)\n"
        "  --to <jid>          Destination address (COMPONENT_TO)\n"
        "  --fifo              Read and send stdin one line at a time\n"
        "  --config <file>     Load KEY=VALUE settings\n"
        "  --debug             Enable libstrophe debug logging\n"
        "  --help              Show this help\n\n"
        "Optional retained PubSub options:\n"
        "  --pubsub <node> --pubsub-service <jid> [--item <id>]\n"
        "  [--raw-xml] [--append]\n\n"
        "Apache stream example (placeholders):\n"
        "  tail -f <apache-access-log> | %s --host <server-vpn-address>\n"
        "    --port 5347 --component <component-domain>\n"
        "    --secret <shared-secret> --pubsub-service <pubsub-service-jid>\n"
        "    --pubsub <access-node> --fifo\n",
        program,
        program);
}

static const char *env_value(const char *name)
{
    const char *value = getenv(name);
    return value && *value ? value : NULL;
}

int main(int argc, char **argv)
{
    app_t app;
    const char *config = NULL;
    const char *port_text;
    xmpp_log_t *logger;
    xmpp_ctx_t *ctx = NULL;
    xmpp_conn_t *conn = NULL;
    int i;
    int rc = 1;
    bool disconnect_started = false;

    memset(&app, 0, sizeof(app));

    /* Find config first. Process environment retains precedence over the file. */
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
    }
    if (config) {
        if (load_env_file(config) != 0) return 2;
    } else {
        struct stat config_stat;
        if (stat(".env", &config_stat) == 0 && S_ISREG(config_stat.st_mode) &&
            load_env_file(".env") != 0)
            return 2;
    }

    app.host = env_value("COMPONENT_HOST");
    port_text = env_value("COMPONENT_PORT");
    app.domain = env_value("COMPONENT_DOMAIN");
    app.secret = env_value("COMPONENT_SECRET");
    app.from = env_value("COMPONENT_FROM");
    app.to = env_value("COMPONENT_TO");
    app.pubsub_service = env_value("PUBSUB_SERVICE");
    app.pubsub_node = env_value("PUBSUB_NODE");
    app.pubsub_item = env_value("PUBSUB_ITEM");
    app.entry_title = env_value("TITLE");
    app.entry_category = env_value("CATEGORY");
    app.entry_language = env_value("LANGUAGE");
    app.entry_tags = env_value("TAGS");
    app.entry_author_name = env_value("AUTHOR_NAME");
    app.entry_author_email = env_value("AUTHOR_EMAIL");
    app.entry_generator = env_value("GENERATOR");
    app.pubsub_raw = env_value("PUBSUB_RAW") ?
                     atoi(env_value("PUBSUB_RAW")) != 0 : 0;
    app.pubsub_append = env_value("PUBSUB_APPEND") ?
                        atoi(env_value("PUBSUB_APPEND")) != 0 : 0;
    if (port_text && parse_port(port_text, &app.port) != 0) {
        fprintf(stderr, "Invalid COMPONENT_PORT: expected 1..65535\n");
        return 2;
    }

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = NULL;
#define OPTION_VALUE() \
        do { if (++i >= argc) { usage(argv[0]); return 2; } value = argv[i]; } while (0)
        if (!strcmp(arg, "--config")) { OPTION_VALUE(); }
        else if (!strcmp(arg, "--host")) { OPTION_VALUE(); app.host = value; }
        else if (!strcmp(arg, "--port")) {
            OPTION_VALUE();
            if (parse_port(value, &app.port) != 0) {
                fprintf(stderr, "Invalid --port: expected 1..65535\n");
                return 2;
            }
        }
        else if (!strcmp(arg, "--component")) { OPTION_VALUE(); app.domain = value; }
        else if (!strcmp(arg, "--secret")) { OPTION_VALUE(); app.secret = value; }
        else if (!strcmp(arg, "--from")) { OPTION_VALUE(); app.from = value; }
        else if (!strcmp(arg, "--to")) { OPTION_VALUE(); app.to = value; }
        else if (!strcmp(arg, "--fifo")) app.fifo = 1;
        else if (!strcmp(arg, "--debug")) app.debug = 1;
        else if (!strcmp(arg, "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(arg, "--pubsub")) { OPTION_VALUE(); app.pubsub_node = value; }
        else if (!strcmp(arg, "--pubsub-service")) { OPTION_VALUE(); app.pubsub_service = value; }
        else if (!strcmp(arg, "--item")) { OPTION_VALUE(); app.pubsub_item = value; }
        else if (!strcmp(arg, "--raw-xml")) app.pubsub_raw = 1;
        else if (!strcmp(arg, "--append")) app.pubsub_append = 1;
        else if (arg[0] == '-') {
            if (!strcmp(arg, "-") && !app.body) {
                app.body = read_all_stdin();
                app.body_allocated = true;
            } else {
                fprintf(stderr, "Unknown option: %s\n", arg);
                usage(argv[0]);
                goto cleanup;
            }
        } else if (!app.body) {
            app.body = arg;
        } else {
            fprintf(stderr, "Only one message argument is allowed\n");
            goto cleanup;
        }
#undef OPTION_VALUE
    }

    if (!app.host || !app.port || !app.domain || !app.secret) {
        fprintf(stderr,
                "COMPONENT_HOST, COMPONENT_PORT, COMPONENT_DOMAIN, and "
                "COMPONENT_SECRET are required\n");
        rc = 2;
        goto cleanup;
    }
    if (!app.from) app.from = app.domain;
    if (!from_matches_domain(app.from, app.domain)) {
        fprintf(stderr,
                "COMPONENT_FROM must be an address owned by COMPONENT_DOMAIN\n");
        rc = 2;
        goto cleanup;
    }
    if (!app.fifo && !app.body && !isatty(STDIN_FILENO)) {
        app.body = read_all_stdin();
        app.body_allocated = true;
    }
    if (app.pubsub_node) {
        if (!app.pubsub_service) {
            fprintf(stderr, "--pubsub requires --pubsub-service\n");
            rc = 2;
            goto cleanup;
        }
        if (!app.pubsub_item) app.pubsub_item = "main";
    } else if ((app.fifo || app.body) && !app.to) {
        fprintf(stderr, "COMPONENT_TO or --to is required when sending messages\n");
        rc = 2;
        goto cleanup;
    }
    if (app.pubsub_append && app.pubsub_raw) {
        fprintf(stderr, "--append cannot be combined with --raw-xml\n");
        rc = 2;
        goto cleanup;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    xmpp_initialize();
    logger = xmpp_get_default_logger(app.debug ? XMPP_LEVEL_DEBUG
                                               : XMPP_LEVEL_ERROR);
    ctx = xmpp_ctx_new(NULL, logger);
    if (!ctx) {
        fprintf(stderr, "Failed to allocate libstrophe context\n");
        goto shutdown;
    }
    conn = xmpp_conn_new(ctx);
    if (!conn) {
        fprintf(stderr, "Failed to allocate libstrophe connection\n");
        goto shutdown;
    }

    xmpp_conn_set_jid(conn, app.domain);
    xmpp_conn_set_pass(conn, app.secret);
    if (xmpp_connect_component(conn, app.host, app.port,
                               connection_handler, &app) != XMPP_EOK) {
        fprintf(stderr, "xmpp_connect_component() failed to start\n");
        goto shutdown;
    }

    while (!stop_requested) {
        xmpp_run_once(ctx, 50);
        if (app.connect_failed || (app.one_shot_sent && !app.connected)) break;
        if (app.fifo && app.connected) {
            struct pollfd input = { STDIN_FILENO, POLLIN, 0 };
            int ready = poll(&input, 1, 0);
            if (ready > 0 && (input.revents & POLLIN)) {
                char line[4096];
                size_t length;
                if (!fgets(line, sizeof(line), stdin)) {
                    stop_requested = 1;
                    continue;
                }
                length = strlen(line);
                while (length && (line[length - 1] == '\n' ||
                                  line[length - 1] == '\r'))
                    line[--length] = '\0';
                if (send_line(conn, ctx, &app, line) != 0) {
                    fprintf(stderr, "Failed to build outgoing stanza\n");
                    app.connect_failed = true;
                }
            }
        }
    }

    if (app.connected && !disconnect_started) {
        disconnect_started = true;
        xmpp_disconnect(conn);
        while (app.connected) xmpp_run_once(ctx, 50);
    }
    rc = app.connect_failed ? 1 : 0;

shutdown:
    if (conn) xmpp_conn_release(conn);
    if (ctx) xmpp_ctx_free(ctx);
    xmpp_shutdown();
cleanup:
    if (app.body_allocated) free((char *)app.body);
    free(app.append_buf);
    return rc;
}
