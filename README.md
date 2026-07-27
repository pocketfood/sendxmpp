# sendxmpp external component

A small XEP-0114 external component written in C with libstrophe. Its primary
use is publishing streamed data, such as Apache access records, to an XMPP
PubSub node without using an XMPP user account. It can also send one-shot
messages, consume stdin, stream lines in FIFO mode, and print incoming
messages.

The component transport in this program does not use the normal XMPP client
STARTTLS flow. Protect it externally with a private network, VPN, SSH tunnel,
or another secured transport.

## libstrophe requirement

The program requires a libstrophe release that declares:

```c
int xmpp_connect_component(
    xmpp_conn_t *conn,
    const char *server,
    unsigned short port,
    xmpp_conn_handler callback,
    void *userdata);
```

This API is present in current libstrophe releases. If your installed
`strophe.h` lacks it, upgrade libstrophe rather than substituting
`xmpp_connect_client()`.

## Build

Install a C compiler, pkg-config, and the libstrophe development package. Then:

```sh
make
```

Or compile directly:

```sh
cc sendxmpp.c $(pkg-config --cflags --libs libstrophe) \
  -O2 -Wall -Wextra -o sendxmpp
```

## Configuration

Copy `.env.example` to `.env`, replace the placeholders, and keep the shared
secret out of source control. The program loads `.env` from its current
directory automatically:

```sh
cp .env.example .env
./sendxmpp "hello from the component"
```

The environment file contains component credentials only. No XMPP user JID or
user password is needed. Using `.env` remains optional: every setting can also
be supplied on the command line, and `--config <file>` selects a different
configuration file.

Required settings:

```text
COMPONENT_HOST=<server-vpn-address>
COMPONENT_PORT=5347
COMPONENT_DOMAIN=<component-domain>
COMPONENT_SECRET=<shared-secret>
COMPONENT_FROM=<component-domain>
COMPONENT_TO=<destination-jid>
```

`COMPONENT_FROM` defaults to `COMPONENT_DOMAIN`. If specified, its domain must
exactly match `COMPONENT_DOMAIN`, which is compatible with ejabberd
`check_from: true`.

## Examples

Stream Apache access data directly to a PubSub node without an environment
file:

```sh
tail -f <apache-access-log> |
  ./sendxmpp \
    --host <server-vpn-address> \
    --port 5347 \
    --component <component-domain> \
    --secret <shared-secret> \
    --pubsub-service <pubsub-service-jid> \
    --pubsub <access-node> \
    --fifo
```

In this form, `from` defaults to `<component-domain>`. Every published IQ has
that explicit `from` address. Each input line is published to the default item
ID `main`, replacing that item. Add `--append` to accumulate all lines received
during the current process and republish the accumulated text on each update.

The XEP-0114 shared secret authenticates the component itself; it is not an
individual XMPP-user password or a per-user access token. Anyone who knows the
secret can authenticate as the component, so only trusted operators should
receive it. The PubSub service must also permit the component domain to publish
to the selected node.

One-shot message:

```sh
./sendxmpp --config <component-config-file> \
  --to <destination-jid> "hello from the component"
```

Piped one-shot input:

```sh
printf '%s\n' "hello from stdin" |
  ./sendxmpp --config <component-config-file> --to <destination-jid>
```

FIFO/stream mode:

```sh
tail -f <input-file> |
  ./sendxmpp --config <component-config-file> \
    --from <component-domain> --to <destination-jid> --fifo
```

Fully specified CLI invocation:

```sh
./sendxmpp \
  --host <server-vpn-address> \
  --port 5347 \
  --component <component-domain> \
  --secret <shared-secret> \
  --from <component-domain> \
  --to <destination-jid> \
  "hello"
```

Incoming message records are written to stdout as tab-separated fields:

```text
IN	from	to	type	body
```

Tabs, newlines, carriage returns, and backslashes inside fields are escaped.

## Architecture

The former user-account client authentication and client TLS flags have been
removed. The component domain and shared secret are assigned to the
libstrophe connection, which is opened with `xmpp_connect_component()`. Once
the XEP-0114 handshake succeeds, the program registers its incoming message
handler. Every outgoing message and retained PubSub IQ includes an explicit
component-owned `from` address.
