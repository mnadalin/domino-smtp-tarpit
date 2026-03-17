#include <stdio.h>
#include <string.h>
#include <time.h>
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

static HEMREGISTRATION g_hReg = NULLHANDLE;

#define VERSION "1.0.0"

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

        /* Found a socket with this peer IP. There may be more than one
         * (e.g., a previous connection still in CLOSE_WAIT alongside the
         * current ESTABLISHED SMTP connection), so we scan them all and
         * return TRUE as soon as we find any that looks alive. */
        found_any = TRUE;

        pfd.fd      = fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 0);
        if (pr < 0)
            continue;       /* poll error on this fd; skip, keep scanning */
        if (pr == 0)
        {
            found_alive = TRUE;   /* Not readable: silently connected */
            break;
        }

        /* Readable — peek to distinguish data from EOF/error. */
        n = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n > 0)
        {
            found_alive = TRUE;   /* Client sent SMTP data; still alive */
            break;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            found_alive = TRUE;   /* Spurious wakeup; assume alive */
            break;
        }

        /* This socket appears dead (FIN or RST).  Don't give up yet —
         * continue scanning for other fds with the same peer IP. */
    }

    if (!found_any)
        return TRUE;    /* Socket not found; assume alive (conservative) */

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
    char        fmtbuf[768];
    const char *pct;
    va_list     ap;

    if (remoteHost && remoteHost[0] != '\0')
        snprintf(label, sizeof(label), "%s (%s)",
                 remoteHost, remoteIP ? remoteIP : "");
    else
        snprintf(label, sizeof(label), "%s",
                 remoteIP ? remoteIP : "unknown");

    /* Substitute the first %s placeholder with the client label. */
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
        AddInLogMessageText("SMTP Tarpit: extension version " VERSION " loaded successfully", NOERROR);
    else
        AddInLogMessageText("SMTP Tarpit: EMRegister failed", err);

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
    char           msgbuf[512];

    if (pRec == NULL || pRec->NotificationType != EM_BEFORE)
        return ERR_EM_CONTINUE;

    delay = (int)OSGetEnvironmentInt("SMTPTarpitDelay");
    if (delay <= 0)
        return ERR_EM_CONTINUE;

    VARARG_COPY(ap, pRec->Ap);
    sessionID         = VARARG_GET(ap, DWORD);
    remoteIP          = VARARG_GET(ap, char *);
    remoteHost        = VARARG_GET(ap, char *);
    possibleRelay     = VARARG_GET(ap, BOOL *);
    greeting          = VARARG_GET(ap, char *);
    maxGreetingLength = VARARG_GET(ap, DWORD);
    va_end(ap);

    SmtpFmtClientMsg(msgbuf, sizeof(msgbuf), remoteHost, remoteIP,
                     "SMTP Tarpit: incoming connection from %s, delaying %d seconds",
                     delay);
    AddInLogMessageText(msgbuf, NOERROR);

    tick.tv_sec  = 1;
    tick.tv_nsec = 0;
    for (elapsed = 0; elapsed < delay; elapsed++)
    {
        nanosleep(&tick, NULL);

        if (!SmtpClientIsAlive(remoteIP))
        {
            SmtpFmtClientMsg(msgbuf, sizeof(msgbuf), remoteHost, remoteIP,
                             "SMTP Tarpit: client %s disconnected after %d second(s), dropping connection",
                             elapsed);
            AddInLogMessageText(msgbuf, NOERROR);
            return ERR_NOACCESS;
        }
    }

    SmtpFmtClientMsg(msgbuf, sizeof(msgbuf), remoteHost, remoteIP,
                     "SMTP Tarpit: delay finished for %s, resuming connection");
    AddInLogMessageText(msgbuf, NOERROR);

    return ERR_EM_CONTINUE;
}

void LNPUBLIC TerminateLibrary(void)
{
    AddInLogMessageText("SMTP Tarpit: extension unloaded", NOERROR);
    if (g_hReg != NULLHANDLE)
    {
        EMDeregister(g_hReg);
        g_hReg = NULLHANDLE;
    }
}
