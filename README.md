# domino-smtp-tarpit

An HCL Domino Extension Manager add-in that delays incoming SMTP connections before the server sends its `220` greeting. This ties up spam bots and dictionary-attack clients for a configurable number of seconds, reducing their throughput at negligible cost to legitimate senders.

## How it works

The add-in registers a `EM_SMTPCONNECT` (before) hook. When an inbound SMTP connection arrives, the hook sleeps one second at a time — checking each second whether the remote client is still connected — until the configured delay expires, then lets Domino proceed normally. If the client disconnects early, the connection is dropped immediately.

`EXTMGR_ADDINS` makes every Domino process load the library, but the extension is only active in the `smtp` task: in all other processes (`http`, `router`, `amgr`, …) it returns immediately without registering any hook or logging anything.

Following the HCL C API rules for Extension Manager libraries:

- the entry point only calls `EMCreateRecursionID` / `EMRegister` (no other C API calls while Domino is still initialising); `notes.ini` is read and the start-up message is logged on the first SMTP connection;
- the hooks are deregistered when Domino unloads the library (library destructor, the Linux counterpart of `DLL_PROCESS_DETACH`), after the `EM_TERMINATENSF` notification — never from inside the `EM_TERMINATENSF` callback.

## Requirements

- HCL Domino 14+ (Linux, 64-bit)
- Domino C API Toolkit (`CAPI_INC` headers)
- `gcc` and `make`

## Building

```bash
make DOMINO_DIR=/opt/hcl/domino/notes/14050000/linux \
     CAPI_INC=/opt/hcl/notesapi/include
```

Both variables have defaults matching common installation paths; override only what differs on your system. The output is `libsmtp-tarpit.so`.

## Deploying

Copy the shared library into Domino's program directory:

```bash
cp libsmtp-tarpit.so /opt/hcl/domino/notes/14050000/linux/
```

## Configuration

Add the following to your Domino `notes.ini` (or via the server console with `set config`):

```ini
# Load the extension
EXTMGR_ADDINS=smtp-tarpit

# Tarpit duration in seconds (0 or missing = disabled)
SMTPTarpitDelay=5
```

If `EXTMGR_ADDINS` already has other entries, append with a comma:

```ini
EXTMGR_ADDINS=existing_addin,smtp-tarpit
```

### Exclusions

Hosts listed in the **anti-relay exclusion list** (`SMTPRlyExcpts` field of the Server Configuration document in `names.nsf`) are automatically bypassed — the tarpit delay is not applied to them. This honours the same list used by Domino's built-in anti-relay enforcement.

> **Note:** exclusions are only evaluated when anti-relay enforcement is active. If enforcement is set to *None*, the exclusion list is ignored and the tarpit applies to all connecting hosts.

The exclusion list is loaded once, on the first incoming connection (if `names.nsf` cannot be read, the load is retried every 60 seconds). To pick up changes, restart the SMTP task:

```
> restart task smtp
```

## Verifying

Reload the SMTP task:

```
> restart task smtp
```

On the first incoming SMTP connection you should see this in the Domino console / log:

```
SMTP Tarpit: extension version 1.2.4 active (delay 5 second(s))
```

The message is logged once per SMTP task start, by the `smtp` process only, and not at server start-up (see *How it works*).

To confirm it is working, connect with telnet and observe the delay before the `220` banner appears:

```bash
telnet your-domino-server 25
```

Disconnection events are also logged:

```
SMTP Tarpit: 192.0.2.1 disconnected after [n] second(s), dropping connection
```

The value of `SMTPTarpitDelay` is read once when the extension loads and cached for the lifetime of the SMTP task. To change it, restart the SMTP task:

```
> restart task smtp
```

## Unloading

Remove `smtp-tarpit` from `EXTMGR_ADDINS` in `notes.ini` and restart the SMTP task.

## Diagnostic logging

Set `DEBUG_SMTPTarpit` in `notes.ini` to enable additional log output:

| Value | Effect |
|-------|--------|
| `0` (default) | Normal operation — only standard connection messages are logged |
| `1` | **Debug** — also logs server name resolution, exclusion list loading, and which configuration document was matched |
| `2` | **Trace** — logs every internal step (API calls, return codes, VARARG reads). Very verbose; use only when diagnosing a specific problem |

```ini
DEBUG_SMTPTarpit=1
```

The value is read once, on the first SMTP connection, and cached for the lifetime of the SMTP task. To change it, restart the SMTP task.

With `DEBUG_SMTPTarpit` ≥ 1 the deregistration performed when the `smtp` task unloads the library is also traced to `/tmp/smtp-tarpit-unload.log` (plain file, since the Domino log may no longer be available at that point). Each `EMDeregister(...) ...` line should be followed by its `= 0x0000` result line.

# Disclaimer

The author is not responsible for any damage or malfunction caused by the extension and/or instructions provided.