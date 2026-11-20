/*
 * rastertozjs: CUPS raster (1-bit K, 600 dpi) to ZjStream for the HP LaserJet 1020.
 * Protocol details follow foo2zjs (GPL-2.0-or-later) for the HP 1018/1020/1022.
 */
#include <cups/raster.h>
#include <cups/sidechannel.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "jbig.h"

enum { ZJT_START_DOC, ZJT_END_DOC, ZJT_START_PAGE, ZJT_END_PAGE, ZJT_JBIG_BIH, ZJT_JBIG_BID, ZJT_END_JBIG };
enum { ZJI_PAGECOUNT = 0, ZJI_DMCOLLATE = 1, ZJI_DMDUPLEX = 2, ZJI_DMPAPER = 3, ZJI_DMCOPIES = 4,
       ZJI_DMDEFAULTSOURCE = 5, ZJI_DMMEDIATYPE = 6, ZJI_NBIE = 7, ZJI_RESOLUTION_X = 8,
       ZJI_RESOLUTION_Y = 9, ZJI_RASTER_X = 12, ZJI_RASTER_Y = 13, ZJI_VIDEO_BPP = 16,
       ZJI_VIDEO_X = 17, ZJI_VIDEO_Y = 18, ZJI_ECONOMODE = 23 };

/* Page size in points and its ZjStream paper code. CUPS rasterizes only the PPD imageable area. */
typedef struct { int w, h, code; } Paper;

static const Paper papers[] = {
    {612, 792, 1}, {612, 1008, 5}, {522, 756, 7}, {595, 842, 9}, {420, 595, 11}, {516, 729, 13},
    {297, 420, 262}, {297, 684, 20}, {312, 624, 27}, {459, 649, 28}, {279, 540, 37},
};

static const char *ERRORS = "media-empty-error,media-jam-error,door-open-error,toner-empty-error,other-error";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int job_ended, error_active, paper_out, heard;
static time_t last_heard;

static unsigned char *jbuf;
static size_t jlen, jcap;

static void die(const char *msg)
{
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(1);
}

static void put32(uint32_t v)
{
    unsigned char b[4] = {v >> 24, v >> 16, v >> 8, v};
    fwrite(b, 1, 4, stdout);
}

static void chunk(uint32_t type, uint16_t rsvd, uint32_t items)
{
    put32(16 + items * 12);
    put32(type);
    put32(items);
    unsigned char t[4] = {rsvd >> 8, rsvd, 'Z', 'Z'};
    fwrite(t, 1, 4, stdout);
}

static void chunk_raw(uint32_t type, uint32_t len)
{
    put32(16 + len);
    put32(type);
    put32(0);
    fwrite("\0\0ZZ", 1, 4, stdout);
}

static void item(uint16_t id, uint32_t v)
{
    unsigned char h[8] = {0, 0, 0, 12, id >> 8, id, 1, 0};
    fwrite(h, 1, 8, stdout);
    put32(v);
}

static void collect(unsigned char *p, size_t n, void *unused)
{
    (void)unused;
    if (jlen + n > jcap) {
        jcap = (jlen + n) * 2;
        if (!(jbuf = realloc(jbuf, jcap)))
            die("out of memory");
    }
    memcpy(jbuf + jlen, p, n);
    jlen += n;
}

static const Paper *find_paper(const cups_page_header2_t *h)
{
    for (size_t i = 0; i < sizeof papers / sizeof *papers; i++)
        if (abs((int)h->PageSize[0] - papers[i].w) <= 2 && abs((int)h->PageSize[1] - papers[i].h) <= 2)
            return &papers[i];
    fprintf(stderr, "WARNING: unknown page size %ux%u pt, using A4 settings\n", h->PageSize[0], h->PageSize[1]);
    return &papers[3];
}

/* Map a PJL status code (HP PJL reference, appendix D; foo2zjs hplj10xx_gui) to IPP printer-state-reasons. */
static void report(int code, const char *display)
{
    const char *reason = NULL, *text = display;
    if (paper_out && code / 1000 == 10)
        return; /* the printer briefly says "printing" each time it retries the paper pickup */
    if (code / 1000 == 41)
        reason = "media-empty-error", text = "Out of paper";
    else if (code == 40022 || code == 30119 || code / 1000 == 42 || code / 1000 == 44)
        reason = "media-jam-error", text = "Paper jam";
    else if (code == 40021)
        reason = "door-open-error", text = "Printer door open";
    else if (code == 40600)
        reason = "toner-empty-error", text = "No toner cartridge";
    else if (code / 1000 == 40 && code != 40038)
        reason = "other-error";
    if (reason)
        fprintf(stderr, "STATE: -%s\nSTATE: +%s\nINFO: %s\n", ERRORS, reason, *text ? text : "Printer needs attention");
    else if (error_active)
        fprintf(stderr, "STATE: -%s\nINFO: Printing\n", ERRORS);
    if (code == 40038 || code == 10006)
        fputs("STATE: +toner-low-report\n", stderr);
    error_active = reason != NULL;
    paper_out = code / 1000 == 41;
}

static void handle_message(char *msg)
{
    char *code = strstr(msg, "CODE="), *disp = strstr(msg, "DISPLAY="), text[64] = "";
    for (char *c = msg; *c; c++)
        if (*c == '\r' || *c == '\n')
            *c = ' ';
    fprintf(stderr, "DEBUG: printer status: %s\n", msg);
    if (disp && sscanf(disp, "DISPLAY=\"%63[^\"]", text) != 1)
        text[0] = 0;
    pthread_mutex_lock(&lock);
    heard = 1;
    last_heard = time(NULL);
    if (strstr(msg, "USTATUS JOB") && strstr(msg, " END "))
        job_ended = 1;
    if ((job_ended || strstr(msg, "USTATUS PAGE")) && error_active) {
        fprintf(stderr, "STATE: -%s\nINFO: Printing\n", ERRORS);
        error_active = paper_out = 0;
    }
    if (code)
        report(atoi(code + 5), text);
    pthread_mutex_unlock(&lock);
}

/* The 1020 stays paused after paper is reloaded until the host reads its device ID (see foo2zjs hplj10xx_gui). */
static void reactivate_if_paper_out(void)
{
    static time_t last;
    char id[1024];
    int len = sizeof id;
    pthread_mutex_lock(&lock);
    int poll = paper_out && time(NULL) - last >= 10;
    pthread_mutex_unlock(&lock);
    if (poll) {
        last = time(NULL);
        cupsSideChannelDoRequest(CUPS_SC_CMD_GET_DEVICE_ID, id, &len, 5.0);
    }
}

/* PJL replies arrive on the CUPS back channel, each terminated by a form feed. */
static void *status_thread(void *unused)
{
    char buf[512], msg[2048];
    size_t len = 0;
    (void)unused;
    for (;;) {
        reactivate_if_paper_out();
        ssize_t n = cupsBackChannelRead(buf, sizeof buf, 1.0);
        if (n == 0 || (n < 0 && errno == EBADF))
            return NULL;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\f' || len == sizeof msg - 1) {
                msg[len] = 0;
                if (len)
                    handle_message(msg);
                len = 0;
            } else if (buf[i])
                msg[len++] = buf[i];
        }
    }
}

/* Keep the job open until the printer reports it finished, so errors on the last page still show. */
static void wait_for_printer(void)
{
    time_t start = time(NULL);
    for (;;) {
        pthread_mutex_lock(&lock);
        time_t now = time(NULL);
        int done = job_ended || (!heard && now - start > 15) || (heard && !error_active && now - last_heard > 90);
        pthread_mutex_unlock(&lock);
        if (done)
            break;
        usleep(250000);
    }
    fprintf(stderr, "STATE: -%s\n", ERRORS);
}

static void start_doc(int econo)
{
    char stamp[15];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", localtime(&now));
    printf("\033%%-12345X@PJL JOB\n@PJL SET JAMRECOVERY=OFF\n@PJL SET DENSITY=3\n"
           "@PJL SET ECONOMODE=%s\n@PJL SET RET=MEDIUM\n@PJL INFO STATUS\n"
           "@PJL USTATUS DEVICE = ON\n@PJL USTATUS JOB = ON\n@PJL USTATUS PAGE = ON\n"
           "@PJL USTATUS TIMED = 30\n@PJL SET JOBATTR=\"JobAttr4=%s\"",
           econo ? "ON" : "OFF", stamp);
    putchar(0);
    printf("\033%%-12345XJZJZ");
    chunk(ZJT_START_DOC, 0x24, 3);
    item(ZJI_DMCOLLATE, 0);
    item(ZJI_DMDUPLEX, 1);
    item(ZJI_PAGECOUNT, 0);
}

static void end_doc(void)
{
    chunk(ZJT_END_DOC, 0, 0);
    printf("\033%%-12345X@PJL EOJ\n\033%%-12345X");
}

static void write_page(cups_raster_t *ras, const cups_page_header2_t *h, int page)
{
    const Paper *p = find_paper(h);
    int w = h->cupsWidth, ht = h->cupsHeight;
    int wpad = (w + 127) & ~127, bpl = wpad / 8, copy = (w + 7) / 8;
    unsigned char *line = malloc(h->cupsBytesPerLine), *bitmap = calloc((size_t)bpl, ht);
    if (!line || !bitmap)
        die("out of memory");

    for (int y = 0; y < ht; y++) {
        if (cupsRasterReadPixels(ras, line, h->cupsBytesPerLine) != h->cupsBytesPerLine)
            die("premature end of raster data");
        unsigned char *dst = bitmap + (size_t)y * bpl;
        memcpy(dst, line, copy);
        if (w & 7)
            dst[copy - 1] &= (unsigned char)(0xff << (8 - (w & 7)));
    }

    struct jbg_enc_state se;
    unsigned char *planes[1] = {bitmap};
    jlen = 0;
    jbg_enc_init(&se, wpad, ht, 1, planes, collect, NULL);
    jbg_enc_options(&se, JBG_ILEAVE | JBG_SMID, JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON, 128, 0, 0);
    jbg_enc_out(&se);
    jbg_enc_free(&se);
    if (jlen <= 20)
        die("JBIG encoder produced no data");

    chunk(ZJT_START_PAGE, 0x9c, 13);
    item(ZJI_ECONOMODE, h->cupsInteger[0] ? 1 : 0);
    item(ZJI_VIDEO_X, w);
    item(ZJI_VIDEO_Y, ht);
    item(ZJI_VIDEO_BPP, 1);
    item(ZJI_RASTER_X, w);
    item(ZJI_RASTER_Y, ht);
    item(ZJI_NBIE, 1);
    item(ZJI_RESOLUTION_X, 600);
    item(ZJI_RESOLUTION_Y, 600);
    item(ZJI_DMDEFAULTSOURCE, 7);
    item(ZJI_DMCOPIES, 1);
    item(ZJI_DMPAPER, p->code);
    item(ZJI_DMMEDIATYPE, 1);

    chunk_raw(ZJT_JBIG_BIH, 20);
    fwrite(jbuf, 1, 20, stdout);
    for (size_t off = 20; off < jlen; off += 65536) {
        size_t n = jlen - off < 65536 ? jlen - off : 65536;
        size_t pad = off + n == jlen ? 16 + ((n + 3) & ~(size_t)3) - n : 0;
        chunk_raw(ZJT_JBIG_BID, (uint32_t)(n + pad));
        fwrite(jbuf + off, 1, n, stdout);
        for (size_t i = 0; i < pad; i++)
            putchar(0);
    }
    chunk(ZJT_END_JBIG, 0, 0);
    chunk(ZJT_END_PAGE, 0, 0);
    fprintf(stderr, "PAGE: %d 1\n", page);
    free(line);
    free(bitmap);
}

int main(int argc, char *argv[])
{
    if (argc < 6 || argc > 7) {
        fputs("Usage: rastertozjs job user title copies options [file]\n", stderr);
        return 1;
    }
    int fd = argc == 7 ? open(argv[6], O_RDONLY) : 0;
    if (fd < 0)
        die("cannot open input file");
    struct stat bc;
    pthread_t status;
    int listening = fd != 3 && fstat(3, &bc) == 0 && (S_ISFIFO(bc.st_mode) || S_ISSOCK(bc.st_mode)) &&
                    pthread_create(&status, NULL, status_thread, NULL) == 0;
    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t h;
    int page = 0;
    while (cupsRasterReadHeader2(ras, &h)) {
        if (h.cupsBitsPerColor != 1 || h.cupsColorSpace != CUPS_CSPACE_K || h.HWResolution[0] != 600 || h.HWResolution[1] != 600)
            die("expected 1-bit K raster at 600x600 dpi");
        if (page++ == 0)
            start_doc(h.cupsInteger[0]);
        write_page(ras, &h, page);
    }
    cupsRasterClose(ras);
    if (page) {
        end_doc();
        fflush(stdout);
        if (listening)
            wait_for_printer();
    }
    return ferror(stdout) ? 1 : 0;
}
