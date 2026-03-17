# domino-smtp-tarpit

An HCL Domino Extension Manager add-in that delays incoming SMTP connections before the server sends its `220` greeting. This ties up spam bots and dictionary-attack clients for a configurable number of seconds, reducing their throughput at negligible cost to legitimate senders.

## How it works

The add-in registers a `EM_SMTPCONNECT` (before) hook. When an inbound SMTP connection arrives, the hook sleeps one second at a time — checking each second whether the remote client is still connected — until the configured delay expires, then lets Domino proceed normally. If the client disconnects early, the connection is dropped immediately.

## Requirements

- HCL Domino 14+ (Linux, 64-bit)
- Domino C API Toolkit (`CAPI_INC` headers)
- `gcc` and `make`

## Building

```bash
make DOMINO_DIR=/opt/hcl/domino/notes/14050000/linux \
     CAPI_INC=/opt/hcl/notesapi/include
```

Both variables have defaults matching common installation paths; override only what differs on your system. The output is `libsmtp_tarpit.so`.

## Deploying

Copy the shared library into Domino's program directory:

```bash
cp libsmtp_tarpit.so /opt/hcl/domino/notes/14050000/linux/
```

## Configuration

Add the following to your Domino `notes.ini` (or via the server console with `set config`):

```ini
# Load the extension
EXTMGR_ADDINS=smtp_tarpit

# Tarpit duration in seconds (0 or missing = disabled)
SMTPTarpitDelay=5
```

If `EXTMGR_ADDINS` already has other entries, append with a comma:

```ini
EXTMGR_ADDINS=existing_addin,smtp_tarpit
```

## Verifying

Reload the SMTP task:

```
> restart task smtp
```

You should see this in the Domino console / log:

```
SMTP Tarpit: extension loaded successfully
```

To confirm it is working, connect with telnet and observe the delay before the `220` banner appears:

```bash
telnet your-domino-server 25
```

Disconnection events are also logged:

```
SMTP Tarpit: client 192.0.2.1 disconnected during delay, dropping connection
```

Changes to the `SMTPTarpitDelay` parameter take effect immediately without requiring an SMTP task restart.

## Unloading

Remove `smtp_tarpit` from `EXTMGR_ADDINS` in `notes.ini` and restart the SMTP task.

# Disclaimer

The author is not responsible for any damage or malfunction caused by the extension and/or instructions provided.