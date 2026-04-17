#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include "global.h"
#include "extmgr.h"
#include "nsferr.h"
#include "osenv.h"
#include "addin.h"
#include "nsfdb.h"
#include "nsfnote.h"
#include "nif.h"
#include "osmem.h"

static HEMREGISTRATION g_hReg = NULLHANDLE;

#define VERSION "1.1.0"

/* -----------------------------------------------------------------------
 * Server Configuration document fields (names.nsf).
 *
 * SMTPRlyCheck  – "Perform Anti-Relay enforcement for these connecting
 *                  hosts".  Value "2" means None (no enforcement at all);
 *                  in that case the exclusion list has no meaning and the
 *                  tarpit bypass must NOT be applied.
 *
 * SMTPRlyExcpts – "Exclude these connecting hosts from anti-relay checks".
 *                  Multi-value text list; IPs are enclosed in [brackets],
 *                  hostnames are plain text.
 * ----------------------------------------------------------------------- */
#define SERVERCONFIG_VIEW          "($ServerConfig)"
#define SERVERCONFIG_RLYCK         "SMTPRlyCheck"
#define SERVERCONFIG_RLYCK_NONE    "2"
#define SERVERCONFIG_FIELD         "SMTPRlyExcpts"

/* Maximum exclusion entries we cache. */
#define MAX_EXCLUDE   256
#define EXCLUDE_LEN   256

static char g_excl[MAX_EXCLUDE][EXCLUDE_LEN];
static int  g_exclCount  = 0;
static BOOL g_exclLoaded = FALSE;

/* -----------------------------------------------------------------------
 * Unified log function.  Pass one of the LOG_* constants as level.
 *
 * LOG_DEBUG messages are emitted only when DEBUG_SMTPTarpit >= 1.
 * LOG_TRACE messages are emitted only when DEBUG_SMTPTarpit >= 2.
 * LOG_INFO  messages are always emitted.
 *
 * The DEBUG_SMTPTarpit value is read once from notes.ini and cached.
 * ----------------------------------------------------------------------- */
#define LOG_INFO  0
#define LOG_DEBUG 1
#define LOG_TRACE 2

static void SmtpTarpitLogMessageText(int level, const char *fmt, ...)
{
    static int  debugLevel = -1;
    static char buf[512];
    va_list     ap;

    if (debugLevel < 0)
        debugLevel = (int)OSGetEnvironmentInt("DEBUG_SMTPTarpit");

    if (level > debugLevel) return;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    AddInLogMessageText(buf, NOERROR);
}

/* -----------------------------------------------------------------------
 * Look up a Server Configuration document in the ($ServerConfig) view
 * by the given key string, using a case-insensitive exact match on the
 * first sorted column (ServerName).
 *
 * Returns TRUE and sets *retNoteID on success; FALSE if not found.
 * ----------------------------------------------------------------------- */
static BOOL LookupSvrCfgByKey(HCOLLECTION hCol, const char *key, NOTEID *retNoteID)
{
    COLLECTIONPOSITION pos;
    DWORD              numMatches;
    HANDLE             hBuf;
    DWORD              nRead;
    WORD               signal;
    NOTEID            *pIDs;
    STATUS             err;

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LookupSvrCfgByKey: key=\"%s\"", key);

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LookupSvrCfgByKey: calling NIFFindByName");
    err = NIFFindByName(hCol, (char *)key,
                        FIND_CASE_INSENSITIVE | FIND_EQUAL,
                        &pos, &numMatches);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LookupSvrCfgByKey: NIFFindByName err=0x%04X numMatches=%lu",
             (unsigned)err, (unsigned long)numMatches);
    if (err != NOERROR || numMatches == 0)
        return FALSE;

    hBuf = NULLHANDLE;
    err  = NIFReadEntries(hCol, &pos,
                          NAVIGATE_CURRENT, 0L,
                          NAVIGATE_NEXT,    1L,
                          READ_MASK_NOTEID,
                          &hBuf, NULL, NULL, &nRead, &signal);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LookupSvrCfgByKey: NIFReadEntries err=0x%04X nRead=%lu hBuf=%s",
             (unsigned)err, (unsigned long)nRead, hBuf != NULLHANDLE ? "valid" : "NULL");
    if (err != NOERROR || hBuf == NULLHANDLE || nRead == 0)
    {
        if (hBuf != NULLHANDLE) OSMemFree(hBuf);
        return FALSE;
    }

    pIDs       = (NOTEID *)OSLockObject(hBuf);
    *retNoteID = *pIDs;
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LookupSvrCfgByKey: noteID=0x%08lX",
             (unsigned long)*retNoteID);
    OSUnlockObject(hBuf);
    OSMemFree(hBuf);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Convert an abbreviated Domino name to canonical form without calling
 * DNCanonicalize (which is unsafe in SMTP extension thread context).
 *
 * "DominoS1/Devangarde"              → "CN=DominoS1/O=Devangarde"
 * "Server/OU1/Org"                   → "CN=Server/OU=OU1/O=Org"
 * "CN=Server/OU=Sales/O=ACME/C=DE"   → same (idempotent: strips and re-applies)
 *
 * Each component is stripped of any leading [A-Z]+= prefix before the
 * canonical prefix is assigned, making the function safe for inputs that
 * are already fully or partially canonical.
 *
 * Component-to-prefix mapping: first→CN, last→O, everything else→OU.
 * Country codes (C=) are not emitted; if the caller needs them the
 * full DNCanonicalize API should be used in an appropriate context.
 * ----------------------------------------------------------------------- */
static void AbbrevToCanonical(const char *abbrev, char *canonical, size_t canonSize)
{
    char        tmp[MAXUSERNAME + 1];
    const char *parts[8];
    int         nparts = 0;
    char       *p;
    size_t      written = 0;
    int         i;

    strncpy(tmp, abbrev, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    p = tmp;
    while (*p && nparts < 8)
    {
        char *component = p;
        char *slash     = strchr(p, '/');
        if (slash) { *slash = '\0'; p = slash + 1; }
        else         p += strlen(p);

        /* Strip any leading [A-Z]+= prefix (e.g. "CN=", "OU=", "O=", "C="). */
        {
            char *eq = component;
            while (*eq >= 'A' && *eq <= 'Z') eq++;
            if (*eq == '=' && eq > component) component = eq + 1;
        }

        parts[nparts++] = component;
    }

    canonical[0] = '\0';
    if (nparts == 0) return;

    for (i = 0; i < nparts; i++)
    {
        const char *prefix;
        int         n;

        if (i == 0)                              prefix = "CN";
        else if (i == nparts - 1 && nparts > 1) prefix = "O";
        else                                     prefix = "OU";

        if (written > 0 && written < canonSize - 1)
            canonical[written++] = '/';

        n = snprintf(canonical + written, canonSize - written, "%s=%s", prefix, parts[i]);
        if (n > 0) written += (size_t)n;
        if (written >= canonSize) { canonical[canonSize - 1] = '\0'; break; }
    }
}

/* -----------------------------------------------------------------------
 * Load the exclusion list from names.nsf once per extension lifetime.
 *
 * Algorithm:
 *   1. Build both forms of the server name from notes.ini.
 *   2. Open ($ServerConfig) view in names.nsf.
 *   3. Look up by canonical name, then abbreviated name, then "*".
 *   4. If SMTPRlyCheck == "2" (None), treat the list as empty.
 *      Otherwise read SMTPRlyExcpts into the cache.
 *
 * Configuration changes take effect after "tell smtp restart".
 * ----------------------------------------------------------------------- */
static void LoadExcludeList(void)
{
    static char abbrev[MAXUSERNAME + 1];
    static char canonical[MAXUSERNAME + 1];
    DBHANDLE    hDb;
    NOTEID      viewNoteID;
    HCOLLECTION hCol;
    NOTEID      noteID;
    NOTEHANDLE  hNote;
    char        rlyCheck[8];
    BOOL        isNone;
    static char textbuf[8192];
    WORD        textlen;
    char       *p, *sep;
    int         newCount;
    static char newExcl[MAX_EXCLUDE][EXCLUDE_LEN];
    STATUS      err;

    /* Load once per extension lifetime.  Mark as loaded first so that
     * concurrent connections don't all race to load simultaneously;
     * worst case two threads load in parallel and write identical data. */
    if (g_exclLoaded) return;
    g_exclLoaded = TRUE;

    /* Build both forms of the server name.
     * DNCanonicalize converts "DominoS1/Devangarde" → "CN=DominoS1/O=Devangarde".
     * On failure (unexpected format) fall back to the abbreviated form only. */
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: step 1 - reading ServerName");
    abbrev[0] = canonical[0] = '\0';
    OSGetEnvironmentString("ServerName", abbrev, sizeof(abbrev) - 1);

    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: ServerName (abbrev) = \"%s\"", abbrev);

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: step 2 - building canonical name");
    if (abbrev[0] == '\0')
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: LoadExcludeList: abbrev is empty, skipping canonicalization");
    }
    else
    {
        AbbrevToCanonical(abbrev, canonical, sizeof(canonical));
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: LoadExcludeList: canonical = \"%s\"", canonical);
    }
    SmtpTarpitLogMessageText(LOG_DEBUG, "SMTP Tarpit [debug]: server name abbreviated: \"%s\"", abbrev);
    SmtpTarpitLogMessageText(LOG_DEBUG, "SMTP Tarpit [debug]: server name canonical:   \"%s\"", canonical);

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: step 3 - opening names.nsf");
    err = NSFDbOpen("names.nsf", &hDb);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: NSFDbOpen err=0x%04X", (unsigned)err);
    if (err != NOERROR)
    {
        g_exclCount = 0;
        return;
    }

    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: step 4 - finding ($ServerConfig) view");
    err = NIFFindView(hDb, SERVERCONFIG_VIEW, &viewNoteID);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: NIFFindView err=0x%04X viewNoteID=0x%08lX",
             (unsigned)err, (unsigned long)viewNoteID);
    if (err != NOERROR)
    {
        SmtpTarpitLogMessageText(LOG_DEBUG,
                 "SMTP Tarpit [debug]: view %s not found in names.nsf", SERVERCONFIG_VIEW);
        NSFDbClose(hDb);
        g_exclCount = 0;
        return;
    }

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: step 5 - opening collection");
    err = NIFOpenCollection(hDb, hDb, viewNoteID, 0, NULLHANDLE,
                            &hCol, NULL, NULL, NULL, NULL);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: NIFOpenCollection err=0x%04X", (unsigned)err);
    if (err != NOERROR)
    {
        NSFDbClose(hDb);
        g_exclCount = 0;
        return;
    }

    /* Try canonical name → abbreviated name → wildcard document.
     * Skip the abbreviated lookup when it is identical to canonical
     * (single-segment names with no org component). */
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: step 6 - looking up server config document");
    noteID = 0;
    if (canonical[0] != '\0' && LookupSvrCfgByKey(hCol, canonical, &noteID))
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: LoadExcludeList: matched by canonical name");
        SmtpTarpitLogMessageText(LOG_DEBUG,
                 "SMTP Tarpit [debug]: matched Server Configuration by canonical name");
    }
    else if (strcmp(canonical, abbrev) != 0 && LookupSvrCfgByKey(hCol, abbrev, &noteID))
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: LoadExcludeList: matched by abbreviated name");
        SmtpTarpitLogMessageText(LOG_DEBUG,
                 "SMTP Tarpit [debug]: matched Server Configuration by abbreviated name");
    }
    else if (LookupSvrCfgByKey(hCol, "*", &noteID))
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: LoadExcludeList: matched by wildcard (*)");
        SmtpTarpitLogMessageText(LOG_DEBUG,
                 "SMTP Tarpit [debug]: matched Server Configuration by wildcard (*)");
    }
    else
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: LoadExcludeList: no matching document found");
        SmtpTarpitLogMessageText(LOG_DEBUG,
                 "SMTP Tarpit [debug]: no matching Server Configuration document found");
    }

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: step 7 - closing collection");
    NIFCloseCollection(hCol);
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: collection closed");

    newCount = 0;

    if (noteID != 0)
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: LoadExcludeList: step 8 - opening note 0x%08lX",
                 (unsigned long)noteID);
        SmtpTarpitLogMessageText(LOG_DEBUG,
                 "SMTP Tarpit [debug]: using Server Configuration NoteID %08lX",
                 (unsigned long)noteID);

        err = NSFNoteOpen(hDb, noteID, 0, &hNote);
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: LoadExcludeList: NSFNoteOpen err=0x%04X", (unsigned)err);
        if (err == NOERROR)
        {
            /* If anti-relay enforcement is None, the exclusion list is
             * irrelevant — leave newCount = 0 so the tarpit runs for all. */
            SmtpTarpitLogMessageText(LOG_TRACE,
                     "SMTP Tarpit [trace]: LoadExcludeList: step 9 - reading SMTPRlyCheck");
            rlyCheck[0] = '\0';
            NSFItemGetText(hNote, SERVERCONFIG_RLYCK,
                           rlyCheck, sizeof(rlyCheck) - 1);
            isNone = (strcmp(rlyCheck, SERVERCONFIG_RLYCK_NONE) == 0);
            SmtpTarpitLogMessageText(LOG_TRACE,
                     "SMTP Tarpit [trace]: LoadExcludeList: %s = \"%s\"%s",
                     SERVERCONFIG_RLYCK, rlyCheck, isNone ? " (None)" : "");
            SmtpTarpitLogMessageText(LOG_DEBUG,
                     "SMTP Tarpit [debug]: %s = \"%s\"%s",
                     SERVERCONFIG_RLYCK, rlyCheck,
                     isNone ? " (None — exclusion list ignored)" : "");

            if (!isNone)
            {
                SmtpTarpitLogMessageText(LOG_TRACE,
                         "SMTP Tarpit [trace]: LoadExcludeList: step 10 - reading SMTPRlyExcpts");
                memset(textbuf, 0, sizeof(textbuf));
                textlen = NSFItemConvertToText(hNote, SERVERCONFIG_FIELD,
                                               textbuf,
                                               (WORD)(sizeof(textbuf) - 1),
                                               '\n');
                SmtpTarpitLogMessageText(LOG_TRACE,
                         "SMTP Tarpit [trace]: LoadExcludeList: NSFItemConvertToText returned %u bytes",
                         (unsigned)textlen);
                if (textlen > 0)
                {
                    p = textbuf;
                    while (p && *p && newCount < MAX_EXCLUDE)
                    {
                        sep = strchr(p, '\n');
                        if (sep) *sep = '\0';

                        while (isspace((unsigned char)*p)) p++;
                        if (*p)
                        {
                            char *e = p + strlen(p) - 1;
                            while (e > p && isspace((unsigned char)*e))
                                *e-- = '\0';
                        }
                        if (*p)
                        {
                            strncpy(newExcl[newCount], p, EXCLUDE_LEN - 1);
                            newExcl[newCount][EXCLUDE_LEN - 1] = '\0';
                            newCount++;
                        }
                        p = sep ? sep + 1 : NULL;
                    }
                }
            }

            SmtpTarpitLogMessageText(LOG_TRACE,
                     "SMTP Tarpit [trace]: LoadExcludeList: %s: %d value(s) parsed",
                     SERVERCONFIG_FIELD, newCount);
            SmtpTarpitLogMessageText(LOG_DEBUG,
                     "SMTP Tarpit [debug]: %s: %d value(s) loaded",
                     SERVERCONFIG_FIELD, newCount);

            SmtpTarpitLogMessageText(LOG_TRACE,
                     "SMTP Tarpit [trace]: LoadExcludeList: step 11 - closing note");
            NSFNoteClose(hNote);
            SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: note closed");
        }
    }

    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: step 12 - closing names.nsf");
    NSFDbClose(hDb);
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: names.nsf closed");

    g_exclCount = newCount;
    memcpy(g_excl, newExcl, (size_t)newCount * sizeof(g_excl[0]));
}

/* -----------------------------------------------------------------------
 * Wildcard string match where '*' matches any sequence of characters
 * (including dots, so [192.168.*] covers 192.168.x.y and [208.*.0.0]
 * covers 208.x.0.0 for any x).  Case-insensitive.
 *
 * Uses the standard iterative two-pointer algorithm: no recursion, no
 * heap allocation, O(n) on typical inputs.
 * ----------------------------------------------------------------------- */
static BOOL WildcardMatch(const char *pat, const char *str)
{
    const char *starPat = NULL;
    const char *starStr = str;

    while (*str)
    {
        if (*pat == '*')
        {
            starPat = pat++;
            starStr = str;
        }
        else if (tolower((unsigned char)*pat) == tolower((unsigned char)*str))
        {
            pat++;
            str++;
        }
        else if (starPat)
        {
            pat = starPat + 1;
            str = ++starStr;
        }
        else
            return FALSE;
    }

    while (*pat == '*') pat++;
    return (*pat == '\0');
}

/* -----------------------------------------------------------------------
 * Match one exclusion entry against the connecting client.
 *
 * Entries enclosed in [brackets] are compared against remoteIP; plain
 * entries are compared against remoteHost (case-insensitive).
 * '*' wildcards are allowed anywhere in an IP pattern
 * (e.g. [192.168.1.*] or [208.*.0.0]).
 * ----------------------------------------------------------------------- */
static BOOL MatchExcludeEntry(const char *entry,
                               const char *remoteIP,
                               const char *remoteHost)
{
    static char pat[EXCLUDE_LEN];
    const char *p = entry;

    if (*p == '[')
    {
        size_t len;
        p++;
        len = strlen(p);
        if (len > 0 && p[len - 1] == ']') len--;
        if (len >= EXCLUDE_LEN) len = EXCLUDE_LEN - 1;
        memcpy(pat, p, len);
        pat[len] = '\0';

        if (!remoteIP || !remoteIP[0]) return FALSE;
        return WildcardMatch(pat, remoteIP);
    }
    else
    {
        if (!remoteHost || !remoteHost[0]) return FALSE;
        return (strcasecmp(entry, remoteHost) == 0);
    }
}

/* -----------------------------------------------------------------------
 * Return TRUE when the connecting host/IP should bypass the tarpit because
 * it is listed in the Server Configuration anti-relay exclusion list.
 * ----------------------------------------------------------------------- */
static BOOL SmtpTarpitIsExcluded(const char *remoteIP, const char *remoteHost)
{
    int i;

    if (!g_exclLoaded)
        LoadExcludeList();

    for (i = 0; i < g_exclCount; i++)
    {
        if (MatchExcludeEntry(g_excl[i], remoteIP, remoteHost))
        {
            SmtpTarpitLogMessageText(LOG_DEBUG,
                     "SMTP Tarpit [debug]: %s (%s) matched exclusion entry \"%s\"",
                     remoteHost && remoteHost[0] ? remoteHost : "unknown",
                     remoteIP   && remoteIP[0]   ? remoteIP   : "unknown",
                     g_excl[i]);
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Scan the process's open file descriptors for a TCP socket whose remote
 * peer matches remoteIP, then do a non-blocking peek to decide whether the
 * client is still connected.
 *
 * Returns TRUE  if the client appears connected (or if we cannot determine
 *               its state — conservative default).
 * Returns FALSE if we find the socket and the peer has closed the connection
 *               (recv returns 0 for FIN, or -1 for RST / other hard error).
 */
static BOOL SmtpClientIsAlive(const char *remoteIP)
{
    int                     fd;
    struct sockaddr_storage peer;
    socklen_t               peerlen;
    char                    ipbuf[INET6_ADDRSTRLEN];
    struct pollfd           pfd;
    char                    probe;
    ssize_t                 n;
    int                     pr;
    BOOL                    found_any   = FALSE;
    BOOL                    found_alive = FALSE;

    if (!remoteIP || !remoteIP[0])
        return TRUE;

    for (fd = 3; fd < 4096; fd++)
    {
        peerlen = sizeof(peer);
        if (getpeername(fd, (struct sockaddr *)&peer, &peerlen) != 0)
            continue;

        ipbuf[0] = '\0';
        if (peer.ss_family == AF_INET)
            inet_ntop(AF_INET,
                      &((struct sockaddr_in *)&peer)->sin_addr,
                      ipbuf, sizeof(ipbuf));
        else if (peer.ss_family == AF_INET6)
            inet_ntop(AF_INET6,
                      &((struct sockaddr_in6 *)&peer)->sin6_addr,
                      ipbuf, sizeof(ipbuf));
        else
            continue;

        if (strcmp(ipbuf, remoteIP) != 0)
            continue;

        found_any = TRUE;

        pfd.fd      = fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 0);
        if (pr < 0)
            continue;
        if (pr == 0)
        {
            found_alive = TRUE;
            break;
        }

        n = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n > 0)
        {
            found_alive = TRUE;
            break;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            found_alive = TRUE;
            break;
        }
    }

    if (!found_any)
        return TRUE;

    return found_alive;
}

/*
 * Format a log message where the first %s is the client identifier.
 * The identifier expands to "host (ip)" when remoteHost is non-empty,
 * or to "ip" (falling back to "unknown") otherwise.
 */
static void SmtpFmtClientMsg(char *buf, size_t bufsz,
                              const char *remoteHost, const char *remoteIP,
                              const char *fmt, ...)
{
    static char label[256];
    static char fmtbuf[768];
    const char *pct;
    va_list     ap;

    if (remoteHost && remoteHost[0] != '\0')
        snprintf(label, sizeof(label), "%s (%s)",
                 remoteHost, remoteIP ? remoteIP : "");
    else
        snprintf(label, sizeof(label), "%s",
                 remoteIP ? remoteIP : "unknown");

    pct = strstr(fmt, "%s");
    if (pct)
        snprintf(fmtbuf, sizeof(fmtbuf), "%.*s%s%s",
                 (int)(pct - fmt), fmt, label, pct + 2);
    else
        snprintf(fmtbuf, sizeof(fmtbuf), "%s", fmt);

    va_start(ap, fmt);
    vsnprintf(buf, bufsz, fmtbuf, ap);
    va_end(ap);
}

STATUS LNCALLBACK SmtpTarpitCallback(EMRECORD *pRec);

STATUS LNPUBLIC MainEntryPoint(void)
{
    STATUS err;
    WORD   recursionID = 0;

    err = EMCreateRecursionID(&recursionID);
    if (err != NOERROR)
        return err;

    err = EMRegister(EM_SMTPCONNECT,
                     EM_REG_BEFORE,
                     (EMHANDLER)SmtpTarpitCallback,
                     recursionID,
                     &g_hReg);

    if (err == NOERROR)
        SmtpTarpitLogMessageText(LOG_INFO,
                 "SMTP Tarpit: extension version " VERSION " loaded successfully");
    else
        SmtpTarpitLogMessageText(LOG_INFO, "SMTP Tarpit: EMRegister failed");

    return err;
}

STATUS LNCALLBACK SmtpTarpitCallback(EMRECORD *pRec)
{
    int            delay;
    int            elapsed;
    VARARG_PTR     ap;
    DWORD          sessionID;
    char          *remoteIP;
    char          *remoteHost;
    BOOL          *possibleRelay;
    char          *greeting;
    DWORD          maxGreetingLength;
    struct timespec tick;
    static char    msgbuf[512];

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: callback entered");

    if (pRec == NULL || pRec->NotificationType != EM_BEFORE)
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: callback exit - not EM_BEFORE or NULL pRec");
        return ERR_EM_CONTINUE;
    }

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 1 - reading SMTPTarpitDelay");
    delay = (int)OSGetEnvironmentInt("SMTPTarpitDelay");
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 1 done - delay=%d", delay);
    if (delay <= 0)
        return ERR_EM_CONTINUE;

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 2 - VARARG_COPY");
    VARARG_COPY(ap, pRec->Ap);
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 3 - VARARG_GET sessionID");
    sessionID         = VARARG_GET(ap, DWORD);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: step 3 done - sessionID=%lu", (unsigned long)sessionID);
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 4 - VARARG_GET remoteIP");
    remoteIP          = VARARG_GET(ap, char *);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: step 4 done - remoteIP=%s", remoteIP ? remoteIP : "(null)");
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 5 - VARARG_GET remoteHost");
    remoteHost        = VARARG_GET(ap, char *);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: step 5 done - remoteHost=%s", remoteHost ? remoteHost : "(null)");
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 6 - VARARG_GET possibleRelay");
    possibleRelay     = VARARG_GET(ap, BOOL *);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: step 6 done - possibleRelay ptr=%s",
             possibleRelay ? "valid" : "null");
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 7 - VARARG_GET greeting");
    greeting          = VARARG_GET(ap, char *);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: step 7 done - greeting ptr=%s", greeting ? "valid" : "null");
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 8 - VARARG_GET maxGreetingLength");
    maxGreetingLength = VARARG_GET(ap, DWORD);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: step 8 done - maxGreetingLength=%lu",
             (unsigned long)maxGreetingLength);
    va_end(ap);

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 9 - calling SmtpTarpitIsExcluded");
    /* Check whether this host is in the Server Configuration anti-relay
     * exclusion list (SMTPRlyExcpts).  The list is only honoured when
     * SMTPRlyCheck != "2" (i.e. anti-relay enforcement is active). */
    if (SmtpTarpitIsExcluded(remoteIP, remoteHost))
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: step 9 - host is excluded, bypassing");
        SmtpFmtClientMsg(msgbuf, sizeof(msgbuf), remoteHost, remoteIP,
                         "SMTP Tarpit: %s bypassing tarpit delay: "
                         "host is excluded in your configuration.");
        SmtpTarpitLogMessageText(LOG_INFO, "%s", msgbuf);
        return ERR_EM_CONTINUE;
    }

    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: step 10 - formatting incoming connection message");
    SmtpFmtClientMsg(msgbuf, sizeof(msgbuf), remoteHost, remoteIP,
                     "SMTP Tarpit: %s incoming connection, delaying %d second(s)...",
                     delay);
    SmtpTarpitLogMessageText(LOG_INFO, "%s", msgbuf);

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 11 - entering delay loop");
    tick.tv_sec  = 1;
    tick.tv_nsec = 0;
    for (elapsed = 0; elapsed < delay; elapsed++)
    {
        nanosleep(&tick, NULL);

        SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 11 - checking client alive");
        if (!SmtpClientIsAlive(remoteIP))
        {
            SmtpFmtClientMsg(msgbuf, sizeof(msgbuf), remoteHost, remoteIP,
                             "SMTP Tarpit: %s disconnected after %d second(s), dropping connection",
                             elapsed);
            SmtpTarpitLogMessageText(LOG_INFO, "%s", msgbuf);
            return ERR_NOACCESS;
        }
    }

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 12 - delay finished");
    SmtpFmtClientMsg(msgbuf, sizeof(msgbuf), remoteHost, remoteIP,
                     "SMTP Tarpit: %s delay finished, resuming connection");
    SmtpTarpitLogMessageText(LOG_INFO, "%s", msgbuf);

    return ERR_EM_CONTINUE;
}

void LNPUBLIC TerminateLibrary(void)
{
    SmtpTarpitLogMessageText(LOG_INFO, "SMTP Tarpit: extension unloaded");
    if (g_hReg != NULLHANDLE)
    {
        EMDeregister(g_hReg);
        g_hReg = NULLHANDLE;
    }
}
