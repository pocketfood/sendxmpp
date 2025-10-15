# sendxmpp — tiny XMPP CLI (C + libstrophe)

A tiny, fast, scriptable XMPP client that:
- sends **direct messages** and **groupchat (MUC)** messages
- supports **interactive REPL** mode
- reads **.env** or `~/.sendxmpp.env` for creds/targets
- accepts **piped stdin** (so any command can notify you)
- STARTTLS (default) / direct TLS / plaintext (testing)

Binary is ~20–60 KB dynamically linked on Debian/Ubuntu.

---

## Quick start

```bash
# deps (Debian/Ubuntu/WSL)
sudo apt update
sudo apt install -y build-essential pkg-config libstrophe-dev libexpat1-dev libmbedtls-dev

# build
make

# GCC Build
gcc sendxmpp.c $(pkg-config --cflags --libs libstrophe) -Os -ffunction-sections -fdata-sections -Wl,--gc-sections -s -o sendxmpp

# configure
cat > .env <<'EOF'
JID=user@example.com/res
PASS=supersecret
TO=buddy@example.com
# MUC=room@conference.example.com
# NICK=MyNick
HOST=xmpp.example.com
PORT=5222
TLS=starttls
REQUIRE_TLS=1
INSECURE=0
EOF

# send a one-liner from .env
./sendxmpp "hello from env"

# pipe command output (dash optional)
echo "deploy complete @ $(date)" | ./sendxmpp
lscpu | ./sendxmpp
