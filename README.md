# sendxmpp external component

`sendxmpp` is a small XEP-0114 external component written in C with
libstrophe. It connects to an XMPP server using a component domain and shared
secret instead of an XMPP user account and user password.

It supports:

- one-shot chat messages;
- one-shot input read from stdin;
- continuous line-by-line FIFO input;
- incoming message output;
- optional PubSub publishing.

## Security and transport

The shared secret authenticates the whole external component. It is not an
individual user password or per-user token. Keep it out of source control and
only provide it to trusted operators.

The XEP-0114 connection in this program does not use the normal XMPP client
STARTTLS flow. Protect the transport externally with a private network, VPN,
SSH tunnel, or another secured transport.

## Server configuration

The component needs a unique XMPP domain. It must not be a normal domain in the
server's global hosted-domain list and must not be the PubSub service domain.
It is an XMPP routing identity, not a URL path or an IP address.

Generic ejabberd listener:

```yaml
listen:
  -
    port: <component-port>
    ip: "<server-private-address>"
    access: all
    module: ejabberd_service
    check_from: true
    hosts:
      "<component-domain>":
        password: "<shared-secret>"
```

The program connects to `<server-private-address>`, but identifies itself as
`<component-domain>`. A DNS record for the component domain is not required
when the program connects directly to the configured host.

After editing ejabberd, validate and restart it using the commands appropriate
for that installation.

## Dependencies and build

Install a C compiler, `make`, pkg-config, and the libstrophe development
package. On Debian-family systems the package names are commonly:

```sh
sudo apt install build-essential pkg-config libstrophe-dev zlib1g-dev
```

Confirm that component support is available:

```sh
pkg-config --modversion libstrophe
grep -n 'xmpp_connect_component' /usr/include/strophe.h
```

Build:

```sh
make
```

Equivalent direct compilation:

```sh
cc sendxmpp.c $(pkg-config --cflags --libs libstrophe) \
  -O2 -Wall -Wextra -o sendxmpp
```

### libstrophe Stream Management compatibility

Some libstrophe builds dereference client Stream Management state during
component receive, send, and event-queue paths, although XEP-0114 component
connections may not have that state. The symptom is a segmentation fault
immediately after a successful component handshake or while sending the first
stanza.

The repository includes
`patches/libstrophe-xep0114-sm-null.patch`, which adds the three required null
guards. Apply it to the matching libstrophe source tree before building that
library:

```sh
cd <libstrophe-source-directory>
patch -p1 < <sendxmpp-source-directory>/patches/libstrophe-xep0114-sm-null.patch
make
make install
```

When libstrophe is installed under a custom prefix, make its pkg-config file
and shared library discoverable before rebuilding `sendxmpp`:

```sh
export PKG_CONFIG_PATH="<local-prefix>/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="<local-prefix>/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
make clean
make
```

The Makefile embeds the libstrophe library directory reported by `pkg-config`
as a runtime search path in `sendxmpp`. This keeps a locally patched
libstrophe selected after reboot without requiring `LD_LIBRARY_PATH` whenever
the program runs. Keep `PKG_CONFIG_PATH` configured when rebuilding so
`pkg-config` selects the intended installation:

```sh
printf '%s\n' \
  'export PKG_CONFIG_PATH="<local-prefix>/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"' \
  >> ~/.profile
```

Log out and back in after changing `~/.profile`, or load it in the current
shell with `. ~/.profile`. Verify the resulting executable with:

```sh
readelf -d ./sendxmpp | grep -E 'RPATH|RUNPATH'
ldd ./sendxmpp | grep libstrophe
```

Confirm which library will be loaded:

```sh
ldd ./sendxmpp | grep strophe
```

## Environment configuration

Copy the safe template and restrict its permissions:

```sh
cp .env.example .env
chmod 600 .env
```

Set component credentials and addresses:

```env
COMPONENT_HOST=<server-private-address>
COMPONENT_PORT=<component-port>
COMPONENT_DOMAIN=<component-domain>
COMPONENT_SECRET=<shared-secret>
COMPONENT_FROM=<component-domain>
COMPONENT_TO=<recipient-jid>
```

The program automatically loads `.env` from its current directory. `.env` is
ignored by Git.

The fields have distinct purposes:

| Setting | Meaning |
| --- | --- |
| `COMPONENT_HOST` | Network hostname or address used for the TCP connection |
| `COMPONENT_PORT` | XEP-0114 listener port, from 1 through 65535 |
| `COMPONENT_DOMAIN` | Unique XMPP domain owned by the external component |
| `COMPONENT_SECRET` | Shared secret configured identically on the server |
| `COMPONENT_FROM` | Explicit outgoing sender; defaults to the component domain |
| `COMPONENT_TO` | Complete recipient JID used for direct messages |

With server-side `check_from: true`, the domain portion of `COMPONENT_FROM`
must exactly match `COMPONENT_DOMAIN`. The safest initial value is the component
domain itself.

Shell environment variables take precedence over `.env`. If a changed `.env`
appears to be ignored, inspect and clear older exported values:

```sh
env | grep '^COMPONENT_'
unset COMPONENT_DOMAIN COMPONENT_FROM
```

Use `--config <file>` to select a different environment-style configuration
file. Every setting can also be supplied as a CLI option.

## Send a one-shot message

Set `COMPONENT_TO` in `.env`, then run:

```sh
./sendxmpp --debug 'Hello from the external component'
```

Or specify the complete recipient JID directly:

```sh
./sendxmpp --debug --to '<recipient-jid>' 'Hello from the external component'
```

The final quoted argument is the message body. A successful debug trace
contains a stanza resembling:

```xml
<message from="<component-domain>" to="<recipient-jid>" type="chat">
  <body>Hello from the external component</body>
</message>
```

For shell commands split across lines, a continuation backslash must be the
last character on its line. Trailing spaces after `\` end the command early
and can result in an empty message. A single-line command avoids that problem.

## Read a one-shot message from stdin

```sh
printf '%s\n' 'Hello from stdin' | ./sendxmpp --to '<recipient-jid>'
```

The program connects, sends the message, disconnects, and exits.

## Receive messages

Running without a message or `--fifo` keeps the authenticated component online:

```sh
./sendxmpp --debug
```

This is expected to remain running until interrupted with `Ctrl+C` or
`SIGTERM`. Incoming messages are printed as tab-separated records:

```text
IN	from	to	type	body
```

Tabs, newlines, carriage returns, and backslashes inside fields are escaped.

## FIFO/stream mode

Use `--fifo` to send each input line as a separate message:

```sh
tail -f <input-file> |
  ./sendxmpp --to '<recipient-jid>' --fifo
```

Stop the producer or press `Ctrl+C` to terminate the component.

## Conference messages

A component must join an XEP-0045 conference as an occupant before it can send
messages to the room. Use `--muc` and provide the room plus a nickname in
`--to`:

```sh
./sendxmpp --muc \
  --to '<room-name>@<conference-service>/<nickname>' \
  'Hello from the external component'
```

The program sends join presence, waits for the room to confirm the occupant,
and then sends a `groupchat` message to the bare room JID. The component-owned
`from` address must still match `COMPONENT_DOMAIN`. A room may independently
require membership, a password, or permission to speak.

## Optional PubSub publishing

PubSub configuration is separate from direct messaging:

```env
PUBSUB_SERVICE=<pubsub-service-jid>
PUBSUB_NODE=<pubsub-node>
PUBSUB_ITEM=main
PUBSUB_RAW=0
PUBSUB_APPEND=0
```

One-shot publish:

```sh
printf '%s\n' 'example record' |
  ./sendxmpp --pubsub-service '<pubsub-service-jid>' \
    --pubsub '<pubsub-node>'
```

For a one-shot publish, the program waits up to 15 seconds for the matching IQ
response. Success is reported as:

```text
PUBSUB	result	<iq-id>
```

An IQ error is reported as `PUBSUB error` followed by the returned error
stanza. A timeout exits unsuccessfully instead of assuming that queuing the IQ
meant the server accepted it.

Continuous publishing:

```sh
tail -f <input-file> |
  ./sendxmpp --pubsub-service '<pubsub-service-jid>' \
    --pubsub '<pubsub-node>' --fifo
```

By default, publishes use item ID `main`, so each publish replaces that item.
`--append` accumulates text received during the current process and republishes
the accumulated value. It does not fetch existing node content. The PubSub
service must grant the component identity permission to publish.

### Publish CPU load every five seconds

The following loop reads Linux load averages from `/proc/loadavg` and updates
one PubSub item every five seconds:

```sh
while true; do
  read -r load1 load5 load15 _ < /proc/loadavg

  ./sendxmpp \
    --pubsub-service '<pubsub-service-jid>' \
    --pubsub '<pubsub-node>' \
    --item 'cpu-load' \
    "host=$(hostname) load1=${load1} load5=${load5} load15=${load15}"

  sleep 5
done
```

The fixed item ID `cpu-load` updates the same entry on every publish. Stop the
loop with `Ctrl+C`. Each accepted update prints
`PUBSUB result <iq-id>` as tab-separated fields. The PubSub node must grant the
configured component domain publisher permission.

## CLI summary

```text
--host <host>       Component listener host
--port <port>       Component listener port
--component <name>  Component domain
--secret <secret>   Shared component secret
--from <jid>        Component-owned sender
--to <jid>          Destination address
--muc               Join --to as a conference occupant before sending
--fifo              Read stdin one line at a time
--config <file>     Load KEY=VALUE settings
--debug             Enable libstrophe debug logging
```

Avoid placing the secret on the command line on shared systems because command
arguments may be visible to other processes. Prefer a protected `.env` or
configuration file.

## Architecture

The component domain and shared secret are copied into the libstrophe
connection before calling `xmpp_connect_component()`. After the XEP-0114
handshake completes and libstrophe returns from parsing it, the program
registers the incoming message handler and performs any pending one-shot send.
Every outgoing message and PubSub IQ has explicit `from` and `to` addresses.

SIGINT and SIGTERM trigger a clean disconnect. All libstrophe stanza,
connection, and context objects and application-owned buffers are released
before exit.
