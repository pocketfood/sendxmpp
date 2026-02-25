# sendxmpp — tiny XMPP CLI (C + libstrophe)

A tiny, fast, scriptable XMPP client that:
- sends **direct messages** and **groupchat (MUC)** messages
- supports **interactive REPL** mode
- reads **.env** or `~/.sendxmpp.env` for creds/targets
- accepts **piped stdin** (so any command can notify you)
- publishes **PubSub items via IQ** (`--pubsub <node>`) using Atom-style entries
- optional **debug logging** (`-d`) to inspect raw stanzas
- fully configurable TLS: STARTTLS (default), legacy direct TLS, or plaintext
- STARTTLS (default) / direct TLS / plaintext (testing)

Binary is ~20–60 KB dynamically linked on Debian/Ubuntu.

---

## Quick start

```bash
# deps (Debian/Ubuntu/WSL)
sudo apt update
sudo apt install -y build-essential pkg-config libstrophe-dev libexpat1-dev libmbedtls-dev

# deps (Arch)
sudo pacman -S --needed base-devel pkgconf libstrophe expat mbedtls

# build
make

# Build 2

gcc sendxmpp.c $(pkg-config --cflags --libs libstrophe) -Os -s -o sendxmpp

# configure
cat > .env <<'EOF'
JID=user@example.com/res
PASS=supersecret
TO=buddy@example.com
#MUC=room@conference.example.com
#NICK=MyNick
HOST=xmpp.example.com
PORT=5222
TLS=starttls
REQUIRE_TLS=1
INSECURE=0
PUBSUB_SERVICE=pubsub.example.com
#PUBSUB_NODE=syslog
#PUBSUB_ITEM=main
#PUBSUB_RAW=0
#PUBSUB_APPEND=1
#MODE=fifo
#TITLE=Apache access
#CATEGORY=access
#LANGUAGE=text
#TAGS=apache,access,log
#AUTHOR_NAME=user@example.com
#GENERATOR=sendxmpp
EOF

# send a one-liner from .env
./sendxmpp "hello from env"

# pipe command output (dash optional)
echo "deploy complete @ $(date)" | ./sendxmpp
lscpu | ./sendxmpp

# stream logs to PubSub
tail -f /var/log/syslog | ./sendxmpp --pubsub syslog --mode fifo --append
```

## PubSub IQ nodes

Publish XML or text to a PubSub node using IQ. By default, items are published with the item id `main`,
so repeated publishes update a single main entry. Override with `--item`.
Text publishes are wrapped into an Atom-style `<entry>` compatible with Zinc’s indexing fields.
Note: `--append` accumulates locally for the current run; it does not fetch existing item content.

```bash
# publish raw XML payload
./sendxmpp --pubsub syslog --raw-xml '<log><line>hello</line></log>'

# publish plain text (wrapped as Atom entry)
echo "hello" | ./sendxmpp --pubsub notes

# append to a single item while streaming (text mode)
tail -f /var/log/syslog | ./sendxmpp --pubsub syslog --mode fifo --append
```

```bash
# Access.log raw input data for testing
(while true; do printf '127.0.0.1 - - [%s] "GET /health HTTP/1.1" 200 123 "-" "curl/8.6.0"\n' "$(date +'%d/%b/%Y:%H:%M:%S %z')"; sleep 1; done) | ./sendxmpp --pubsub access.log --mode fifo --pubsub-service pubsub.domain --append
 

```

Atom entry metadata (optional):
```bash
./sendxmpp --pubsub access \
  --title "Apache access log" \
  --category access \
  --language text \
  --tags "apache,access,log" \
  --author-name "user@example.com" \
  --generator "sendxmpp"
```
