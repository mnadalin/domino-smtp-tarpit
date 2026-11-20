#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* POLLRDHUP */
#endif
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/resource.h>
#include "global.h"
#include "extmgr.h"
#include "nsferr.h"
#include "osenv.h"
#include "addin.h"
#include "nsfdb.h"
#include "nsfnote.h"
#include "nif.h"
#include "osmem.h"

/* Extension Manager registrations, only ever made in the smtp process.
 * g_hReg is released from the EM_TERMINATENSF handler (see below). */
static HEMREGISTRATION g_hReg     = NULLHANDLE;
static HEMREGISTRATION g_hRegTerm = NULLHANDLE;

/* Name of the only process the extension is active in. */
#define SMTP_PROCESS_NAME "smtp"

/* DEBUG_SMTPTarpit / SMTPTarpitDelay are read once from notes.ini in
 * MainEntryPoint and cached for the lifetime of the SMTP task; a
 * 'restart task smtp' is required to pick up a changed value. */
static int g_debugLevel  = 0; /* LOG_INFO */
static int g_tarpitDelay = 0;

#define VERSION "1.2.3"

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

/* Seconds to wait before retrying a failed exclusion-list load. */
#define EXCLUDE_RETRY_SECS 60

/* g_excl / g_exclCount / g_exclLoaded / g_exclLastTry are protected by
 * g_exclMutex.  The list is loaded once (on the first connection) and is
 * read-only afterwards; a failed load is retried after EXCLUDE_RETRY_SECS. */
static pthread_mutex_t g_exclMutex = PTHREAD_MUTEX_INITIALIZER;
static char   g_excl[MAX_EXCLUDE][EXCLUDE_LEN];
static int    g_exclCount   = 0;
static BOOL   g_exclLoaded  = FALSE;
static time_t g_exclLastTry = 0;

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
    char        buf[512];
    va_list     ap;

    if (level > g_debugLevel) return;

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
 * Configuration changes take effect after "restart task smtp".
 *
 * Must be called with g_exclMutex held (the static buffers below are
 * shared).  Returns TRUE when names.nsf was read successfully (even if no
 * Server Configuration document matched), FALSE on a hard error so the
 * caller can retry later.
 * ----------------------------------------------------------------------- */
static BOOL LoadExcludeList(void)
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
    BOOL        ok = TRUE;

    /* Build both forms of the server name.
     * AbbrevToCanonical converts "DominoS1/Devangarde" → "CN=DominoS1/O=Devangarde".
     * The abbreviated form is also tried as a fallback lookup key. */
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
        SmtpTarpitLogMessageText(LOG_INFO,
                 "SMTP Tarpit: cannot open names.nsf (error 0x%04X), exclusion list not loaded",
                 (unsigned)err);
        return FALSE;
    }

    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: step 4 - finding ($ServerConfig) view");
    err = NIFFindView(hDb, SERVERCONFIG_VIEW, &viewNoteID);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: NIFFindView err=0x%04X viewNoteID=0x%08lX",
             (unsigned)err, (unsigned long)viewNoteID);
    if (err != NOERROR)
    {
        SmtpTarpitLogMessageText(LOG_INFO,
                 "SMTP Tarpit: view %s not found in names.nsf (error 0x%04X), exclusion list not loaded",
                 SERVERCONFIG_VIEW, (unsigned)err);
        NSFDbClose(hDb);
        return FALSE;
    }

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: step 5 - opening collection");
    err = NIFOpenCollection(hDb, hDb, viewNoteID, 0, NULLHANDLE,
                            &hCol, NULL, NULL, NULL, NULL);
    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: NIFOpenCollection err=0x%04X", (unsigned)err);
    if (err != NOERROR)
    {
        SmtpTarpitLogMessageText(LOG_INFO,
                 "SMTP Tarpit: cannot open view %s (error 0x%04X), exclusion list not loaded",
                 SERVERCONFIG_VIEW, (unsigned)err);
        NSFDbClose(hDb);
        return FALSE;
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
                /* A full buffer means the item text was truncated: the last
                 * entry may be cut in half, so drop it rather than match
                 * against a partial pattern. */
                if (textlen >= sizeof(textbuf) - 1)
                {
                    char *last = strrchr(textbuf, '\n');
                    if (last) *last = '\0';
                    else      textbuf[0] = '\0';
                    SmtpTarpitLogMessageText(LOG_INFO,
                             "SMTP Tarpit: %s exceeds %u bytes, list truncated",
                             SERVERCONFIG_FIELD, (unsigned)(sizeof(textbuf) - 1));
                }
                if (textlen > 0)
                {
                    p = textbuf;
                    while (p && *p)
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
                        if (*p && newCount >= MAX_EXCLUDE)
                        {
                            SmtpTarpitLogMessageText(LOG_INFO,
                                     "SMTP Tarpit: %s has more than %d entries, extra entries ignored",
                                     SERVERCONFIG_FIELD, MAX_EXCLUDE);
                            break;
                        }
                        if (*p)
                        {
                            size_t len = strlen(p);
                            if (len >= EXCLUDE_LEN)
                            {
                                SmtpTarpitLogMessageText(LOG_INFO,
                                         "SMTP Tarpit: %s entry longer than %d chars truncated",
                                         SERVERCONFIG_FIELD, EXCLUDE_LEN - 1);
                                len = EXCLUDE_LEN - 1;
                            }
                            memcpy(newExcl[newCount], p, len);
                            newExcl[newCount][len] = '\0';
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
        else
        {
            SmtpTarpitLogMessageText(LOG_INFO,
                     "SMTP Tarpit: cannot open Server Configuration note %08lX (error 0x%04X)",
                     (unsigned long)noteID, (unsigned)err);
            ok = FALSE;
        }
    }

    SmtpTarpitLogMessageText(LOG_TRACE,
             "SMTP Tarpit [trace]: LoadExcludeList: step 12 - closing names.nsf");
    NSFDbClose(hDb);
    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: LoadExcludeList: names.nsf closed");

    if (!ok)
        return FALSE;

    /* Publish the entries before the count (caller holds g_exclMutex). */
    memcpy(g_excl, newExcl, (size_t)newCount * sizeof(g_excl[0]));
    g_exclCount = newCount;
    return TRUE;
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
    char        pat[EXCLUDE_LEN];
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
    int    i;
    BOOL   excluded = FALSE;
    time_t now;

    /* Hold the mutex for load + match: the match loop is cheap (at most
     * MAX_EXCLUDE short strings) and this guarantees a connection arriving
     * during the initial load waits for the list instead of seeing it empty. */
    pthread_mutex_lock(&g_exclMutex);

    if (!g_exclLoaded)
    {
        now = time(NULL);
        if (g_exclLastTry == 0 || now - g_exclLastTry >= EXCLUDE_RETRY_SECS)
        {
            g_exclLastTry = now;
            g_exclLoaded  = LoadExcludeList();
            if (!g_exclLoaded)
                SmtpTarpitLogMessageText(LOG_INFO,
                         "SMTP Tarpit: exclusion list load failed, retrying in %d seconds",
                         EXCLUDE_RETRY_SECS);
        }
    }

    for (i = 0; i < g_exclCount; i++)
    {
        if (MatchExcludeEntry(g_excl[i], remoteIP, remoteHost))
        {
            SmtpTarpitLogMessageText(LOG_DEBUG,
                     "SMTP Tarpit [debug]: %s (%s) matched exclusion entry \"%s\"",
                     remoteHost && remoteHost[0] ? remoteHost : "unknown",
                     remoteIP   && remoteIP[0]   ? remoteIP   : "unknown",
                     g_excl[i]);
            excluded = TRUE;
            break;
        }
    }

    pthread_mutex_unlock(&g_exclMutex);
    return excluded;
}

/*
 * Normalise an IP address string for comparison: an IPv4-mapped IPv6
 * address ("::ffff:1.2.3.4", as seen on dual-stack listeners) is reduced
 * to its dotted-quad form ("1.2.3.4").  Anything else is copied as-is.
 */
static void NormalizeIP(const char *in, char *out, size_t outsz)
{
    if (!in) { out[0] = '\0'; return; }
    if (strncasecmp(in, "::ffff:", 7) == 0 && strchr(in + 7, '.') != NULL)
        in += 7;
    snprintf(out, outsz, "%s", in);
}

/* Upper bound for the fallback fd scan when /proc/self/fd is unavailable. */
#define FD_SCAN_MAX 65536

/*
 * Check whether fd is a socket connected to remoteIP (already normalised).
 * Returns -1 if it is not, 1 if it is and the peer looks connected,
 * 0 if it is and the peer has closed the connection (FIN or RST).
 */
static int ProbeClientFd(int fd, const char *remoteIP)
{
    struct sockaddr_storage peer;
    socklen_t               peerlen = sizeof(peer);
    char                    ipbuf[INET6_ADDRSTRLEN];
    struct pollfd           pfd;
    char                    probe;
    ssize_t                 n;
    int                     pr;

    if (getpeername(fd, (struct sockaddr *)&peer, &peerlen) != 0)
        return -1;

    ipbuf[0] = '\0';
    if (peer.ss_family == AF_INET)
    {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr,
                  ipbuf, sizeof(ipbuf));
    }
    else if (peer.ss_family == AF_INET6)
    {
        const struct in6_addr *a6 = &((struct sockaddr_in6 *)&peer)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a6))
            inet_ntop(AF_INET, &a6->s6_addr[12], ipbuf, sizeof(ipbuf));
        else
            inet_ntop(AF_INET6, a6, ipbuf, sizeof(ipbuf));
    }
    else
        return -1;

    if (strcmp(ipbuf, remoteIP) != 0)
        return -1;

    pfd.fd      = fd;
    pfd.events  = POLLIN;
#ifdef POLLRDHUP
    /* Reports the peer's FIN even when unread data is still queued
     * (a client that sent something and then hung up). */
    pfd.events |= POLLRDHUP;
#endif
    pfd.revents = 0;
    pr = poll(&pfd, 1, 0);
    if (pr < 0)
        return 1;           /* cannot tell: assume alive */
    if (pr == 0)
        return 1;           /* nothing pending: idle but connected */
    if (pfd.revents & (POLLHUP | POLLERR))
        return 0;
#ifdef POLLRDHUP
    if (pfd.revents & POLLRDHUP)
        return 0;
#endif

    n = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0)
        return 1;           /* client already sent data (early talker) */
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 1;
    return 0;               /* n == 0 (FIN) or hard error (RST) */
}

/*
 * Scan the process's open file descriptors for a TCP socket whose remote
 * peer matches remoteIP, then do a non-blocking peek to decide whether the
 * client is still connected.
 *
 * The EM_SMTPCONNECT record does not expose the socket, so the match is by
 * peer IP only: when several connections from the same IP are open, the
 * client is considered alive as long as any of them is.
 *
 * Returns TRUE  if the client appears connected (or if we cannot determine
 *               its state — conservative default).
 * Returns FALSE if we find the socket(s) and the peer has closed them.
 */
static BOOL SmtpClientIsAlive(const char *remoteIP)
{
    DIR            *dir;
    struct dirent  *de;
    struct rlimit   rl;
    int             fd, maxfd, r;
    BOOL            found_any   = FALSE;
    BOOL            found_alive = FALSE;

    if (!remoteIP || !remoteIP[0])
        return TRUE;

    /* Preferred: enumerate only the fds that actually exist. */
    dir = opendir("/proc/self/fd");
    if (dir != NULL)
    {
        int dfd = dirfd(dir);
        while (!found_alive && (de = readdir(dir)) != NULL)
        {
            char *end;
            long  v = strtol(de->d_name, &end, 10);
            if (*end != '\0' || end == de->d_name || v < 3 || v == dfd)
                continue;
            r = ProbeClientFd((int)v, remoteIP);
            if (r < 0) continue;
            found_any = TRUE;
            if (r > 0) found_alive = TRUE;
        }
        closedir(dir);
    }
    else
    {
        /* Fallback: brute-force scan up to the process fd limit. */
        maxfd = 4096;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
            maxfd = rl.rlim_cur > FD_SCAN_MAX ? FD_SCAN_MAX : (int)rl.rlim_cur;
        for (fd = 3; fd < maxfd && !found_alive; fd++)
        {
            r = ProbeClientFd(fd, remoteIP);
            if (r < 0) continue;
            found_any = TRUE;
            if (r > 0) found_alive = TRUE;
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
    char        label[256];
    char        labelEsc[512];
    char        fmtbuf[768];
    const char *pct;
    size_t      li, ei;
    va_list     ap;

    if (remoteHost && remoteHost[0] != '\0')
        snprintf(label, sizeof(label), "%s (%s)",
                 remoteHost, remoteIP ? remoteIP : "");
    else
        snprintf(label, sizeof(label), "%s",
                 remoteIP ? remoteIP : "unknown");

    /* remoteHost/remoteIP come from the connecting client (reverse DNS,
     * TCP peer address) and are spliced into the format string below.
     * Double up '%' so the client cannot inject conversion specifiers
     * (e.g. a PTR record containing "%n") into vsnprintf's format. */
    for (li = 0, ei = 0; label[li] != '\0' && ei + 2 < sizeof(labelEsc); li++)
    {
        if (label[li] == '%')
            labelEsc[ei++] = '%';
        labelEsc[ei++] = label[li];
    }
    labelEsc[ei] = '\0';

    pct = strstr(fmt, "%s");
    if (pct)
        snprintf(fmtbuf, sizeof(fmtbuf), "%.*s%s%s",
                 (int)(pct - fmt), fmt, labelEsc, pct + 2);
    else
        snprintf(fmtbuf, sizeof(fmtbuf), "%s", fmt);

    va_start(ap, fmt);
    vsnprintf(buf, bufsz, fmtbuf, ap);
    va_end(ap);
}

STATUS LNCALLBACK SmtpTarpitCallback(EMRECORD *pRec);
STATUS LNCALLBACK SmtpTarpitTerminateCallback(EMRECORD *pRec);

/* -----------------------------------------------------------------------
 * Return TRUE when running inside the Domino SMTP task.
 *
 * EXTMGR_ADDINS makes every Domino process (server, http, router, amgr,
 * traveler, ...) load this library, but EM_SMTPCONNECT only fires in the
 * smtp process.  Registering a handler everywhere else is pointless and
 * leaves an Extension Manager registration pointing into this library in
 * processes such as http, whose shutdown (JVM teardown) then crashed.
 *
 * The executable name is taken from /proc/self/exe (symlinks such as
 * notes/latest resolve to the real binary, whose basename is still
 * "smtp"); program_invocation_short_name is the fallback.
 * ----------------------------------------------------------------------- */
static BOOL IsSmtpProcess(void)
{
    char        exe[512];
    ssize_t     n;
    const char *base;

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
    {
        exe[n] = '\0';
        base = strrchr(exe, '/');
        base = base ? base + 1 : exe;
    }
    else
        base = program_invocation_short_name;

    return (base != NULL && strcmp(base, SMTP_PROCESS_NAME) == 0);
}

STATUS LNPUBLIC MainEntryPoint(void)
{
    STATUS err;
    WORD   recursionID = 0;

    /* Stay completely inert outside the SMTP task: no registration, no
     * notes.ini reads, no log messages. */
    if (!IsSmtpProcess())
        return NOERROR;

    if (g_hReg != NULLHANDLE)   /* already initialised in this process */
        return NOERROR;

    g_debugLevel  = (int)OSGetEnvironmentInt("DEBUG_SMTPTarpit");
    g_tarpitDelay = (int)OSGetEnvironmentInt("SMTPTarpitDelay");

    err = EMCreateRecursionID(&recursionID);
    if (err != NOERROR)
        return err;

    err = EMRegister(EM_SMTPCONNECT,
                     EM_REG_BEFORE,
                     (EMHANDLER)SmtpTarpitCallback,
                     recursionID,
                     &g_hReg);
    if (err != NOERROR)
    {
        g_hReg = NULLHANDLE;
        SmtpTarpitLogMessageText(LOG_INFO,
                 "SMTP Tarpit: EMRegister failed (error 0x%04X)", err);
        return err;
    }

    /* Release the EM_SMTPCONNECT registration while NSF services are still
     * up, instead of leaving it dangling until the library is unloaded.
     * A failure here is not fatal: the tarpit itself works without it. */
    err = EMRegister(EM_TERMINATENSF,
                     EM_REG_BEFORE,
                     (EMHANDLER)SmtpTarpitTerminateCallback,
                     recursionID,
                     &g_hRegTerm);
    if (err != NOERROR)
    {
        g_hRegTerm = NULLHANDLE;
        SmtpTarpitLogMessageText(LOG_INFO,
                 "SMTP Tarpit: EMRegister(EM_TERMINATENSF) failed (error 0x%04X)", err);
    }

    SmtpTarpitLogMessageText(LOG_INFO,
             "SMTP Tarpit: extension version " VERSION " loaded successfully");
    return NOERROR;
}

/* -----------------------------------------------------------------------
 * EM_TERMINATENSF: NSF services are shutting down for the smtp process.
 * Deregister the EM_SMTPCONNECT handler here, while the Notes runtime is
 * still fully available.  The EM_TERMINATENSF registration itself is left
 * alone: deregistering the handler that is currently executing is not
 * documented as safe, and the Extension Manager discards it at shutdown.
 *
 * This replaces the former TerminateLibrary(), which is not a C API hook
 * (Domino never calls it) and would have logged / called EMDeregister
 * after Notes had already terminated had anything invoked it.
 * ----------------------------------------------------------------------- */
STATUS LNCALLBACK SmtpTarpitTerminateCallback(EMRECORD *pRec)
{
    (void)pRec;

    if (g_hReg != NULLHANDLE)
    {
        SmtpTarpitLogMessageText(LOG_DEBUG,
                 "SMTP Tarpit [debug]: NSF terminating, deregistering EM_SMTPCONNECT");
        EMDeregister(g_hReg);
        g_hReg = NULLHANDLE;
    }
    return ERR_EM_CONTINUE;
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
    char           msgbuf[512];
    char           ipNorm[64];

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: callback entered");

    if (pRec == NULL || pRec->NotificationType != EM_BEFORE)
    {
        SmtpTarpitLogMessageText(LOG_TRACE,
                 "SMTP Tarpit [trace]: callback exit - not EM_BEFORE or NULL pRec");
        return ERR_EM_CONTINUE;
    }

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 1 - reading SMTPTarpitDelay");
    delay = g_tarpitDelay;
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

    /* "::ffff:1.2.3.4" → "1.2.3.4" so that [1.2.3.4] exclusions and the
     * socket scan match on dual-stack listeners. */
    NormalizeIP(remoteIP, ipNorm, sizeof(ipNorm));

    SmtpTarpitLogMessageText(LOG_TRACE, "SMTP Tarpit [trace]: step 9 - calling SmtpTarpitIsExcluded");
    /* Check whether this host is in the Server Configuration anti-relay
     * exclusion list (SMTPRlyExcpts).  The list is only honoured when
     * SMTPRlyCheck != "2" (i.e. anti-relay enforcement is active). */
    if (SmtpTarpitIsExcluded(ipNorm, remoteHost))
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
        if (!SmtpClientIsAlive(ipNorm))
        {
            SmtpFmtClientMsg(msgbuf, sizeof(msgbuf), remoteHost, remoteIP,
                             "SMTP Tarpit: %s disconnected after %d second(s), dropping connection",
                             elapsed + 1);
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
