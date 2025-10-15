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
#include <unistd.h>
#include <poll.h>
#include <ctype.h>
#include <sys/stat.h>
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

    // runtime
    bool connected;
    bool one_shot_sent;
    const char *body; // one-shot message body (or buffer from stdin)
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

        if (app->muc_jid) {
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
        "Usage (REPL, direct):    %s -i [opts] <jid> <pass> <to>\n"
        "Usage (REPL, muc):       %s -i [opts] --muc <room@conference> --nick <nick> <jid> <pass>\n\n"
        "Env keys (.env or ~/.sendxmpp.env): JID, PASS, TO, MUC, NICK, HOST, PORT, TLS, REQUIRE_TLS, INSECURE, INTERACTIVE\n"
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
        "  -d, --debug            Verbose logging\n", p,p,p,p,p);
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
    }

    // Determine arg shape.
    // Priority:
    //   1) If exactly one positional left: it's the message (or "-" for stdin).
    //   2) Else if no message arg but stdin is piped: read stdin automatically.
    //   3) Else fall back to classic DM/MUC argument shapes (or interactive).
    if (argc - i == 1) {
        if (strcmp(argv[i], "-") == 0) {
            app.body = read_all_stdin_trimmed();
            if (!app.body){ fprintf(stderr,"No stdin provided\n"); return 2; }
        } else {
            app.body = argv[i];
        }
        if(!app.jid || !app.pass){ fprintf(stderr,"JID/PASS must be set in env/config for message-only usage.\n"); return 2; }
        if(!app.to && !app.muc_jid){ fprintf(stderr,"Provide TO=… or MUC=… (+ NICK=…) in env/config.\n"); return 2; }
        if(app.muc_jid && !app.nick){ fprintf(stderr,"MUC is set; NICK is required in env/config.\n"); return 2; }

    } else if ((argc - i) == 0 && !isatty(STDIN_FILENO)) {
        app.body = read_all_stdin_trimmed();
        if (!app.body){ fprintf(stderr,"No stdin provided\n"); return 2; }
        if(!app.jid || !app.pass){ fprintf(stderr,"JID/PASS must be set in env/config for stdin usage.\n"); return 2; }
        if(!app.to && !app.muc_jid){ fprintf(stderr,"Provide TO=… or MUC=… (+ NICK=…) in env/config.\n"); return 2; }
        if(app.muc_jid && !app.nick){ fprintf(stderr,"MUC is set; NICK is required in env/config.\n"); return 2; }

    } else if (!app.interactive) {
        if (app.muc_jid) {
            if (argc - i < 3) { usage(argv[0]); return 2; }
            app.jid = app.jid ? app.jid : argv[i++];
            app.pass= app.pass? app.pass: argv[i++];
            app.body= argv[i++];
            if(!app.nick){ fprintf(stderr,"--nick or NICK= required for MUC.\n"); return 2; }
        } else {
            if (argc - i < 4) { usage(argv[0]); return 2; }
            app.jid = app.jid ? app.jid : argv[i++];
            app.pass= app.pass? app.pass: argv[i++];
            app.to  = app.to  ? app.to   : argv[i++];
            app.body= argv[i++];
        }
    } else {
        if (app.muc_jid) {
            if (!app.nick) { fprintf(stderr,"--nick or NICK= required with --muc\n"); return 2; }
            if (argc - i < 2) { usage(argv[0]); return 2; }
            app.jid = app.jid ? app.jid : argv[i++]; app.pass = app.pass ? app.pass : argv[i++];
        } else {
            if (argc - i < 3) { usage(argv[0]); return 2; }
            app.jid = app.jid ? app.jid : argv[i++]; app.pass = app.pass ? app.pass : argv[i++]; app.to = app.to ? app.to : argv[i++];
        }
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

    if (!app.interactive) {
        xmpp_run(ctx);
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
                if (app.muc_jid) send_groupchat(conn, ctx, app.muc_jid, line);
                else             send_chat(conn, ctx, app.to, line);
            }
        }
    }

    xmpp_conn_release(conn);
    xmpp_ctx_free(ctx);
    xmpp_shutdown();
    return 0;
}
