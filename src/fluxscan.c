/*
 * fluxscan.c  -  native scan kernel for the Gaia DR3 epoch-photometry challenge.
 *
 * Design intent (see README "Fastest design"):
 *   One OpenMP task per input .gz file. Each task mmaps the compressed bytes,
 *   inflates them once with libdeflate into an exact-sized buffer, then walks
 *   the CSV in place, touching only the three columns the challenge needs
 *   (source_id, bp_flux, rp_flux). Qualifying rows are formatted straight into
 *   a per-file byte buffer; the driver concatenates those buffers in the caller's
 *   file order after every task has finished, so the CSV is deterministic without
 *   any global lock or post-sort.
 *
 * The heavy numeric work never leaves C. Python's only job is to hand us the
 * file list and the output path.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <omp.h>

/*
 * libdeflate is resolved at run time with dlopen/dlsym rather than at link time.
 * That way the build depends on neither the -dev header nor a -l flag, and we
 * bind to whatever soname the IRIS image ships. Only three entry points are
 * needed; they are described here by our own function-pointer typedefs over the
 * opaque decompressor handle.
 */
typedef struct libdeflate_decompressor libdeflate_decompressor;
typedef libdeflate_decompressor *(*deflate_open_fn)(void);
typedef void (*deflate_close_fn)(libdeflate_decompressor *);
typedef int (*deflate_gunzip_fn)(libdeflate_decompressor *,
                                 const void *, size_t,
                                 void *, size_t, size_t *);

static deflate_open_fn   deflate_open;
static deflate_close_fn  deflate_close;
static deflate_gunzip_fn deflate_gunzip;

/* Bind the three libdeflate symbols once, before any worker thread starts.
   Returns 0 on success, -1 if the library or a symbol cannot be found. */
static int deflate_bind(void) {
    static const char *sonames[] = { "libdeflate.so.0", "libdeflate.so", NULL };
    void *handle = NULL;
    for (int i = 0; sonames[i] && !handle; i++) {
        handle = dlopen(sonames[i], RTLD_NOW | RTLD_GLOBAL);
    }
    if (!handle) return -1;
    deflate_open   = (deflate_open_fn)   dlsym(handle, "libdeflate_alloc_decompressor");
    deflate_close  = (deflate_close_fn)  dlsym(handle, "libdeflate_free_decompressor");
    deflate_gunzip = (deflate_gunzip_fn) dlsym(handle, "libdeflate_gzip_decompress");
    return (deflate_open && deflate_close && deflate_gunzip) ? 0 : -1;
}

/* Zero-based column positions inside each Gaia epoch-photometry record. */
enum {
    COL_SOURCE_ID = 1,
    COL_BP_FLUX   = 11,
    COL_RP_FLUX   = 16
};

/* Growable output buffer, one per input file (thread-private during the scan). */
typedef struct {
    char  *bytes;
    size_t used;
    size_t room;
    long   kept;   /* qualifying rows written into this buffer */
} outbuf;

/* Running min/max for one flux band while walking its array. */
typedef struct {
    double lo;
    double hi;
    int    valid;
} band;

/* Ensure the buffer can take `extra` more bytes, doubling as needed. */
static int reserve(outbuf *b, size_t extra) {
    size_t want = b->used + extra;
    if (want <= b->room) return 0;
    size_t room = b->room ? b->room : (size_t)(1u << 20); /* 1 MiB seed */
    while (room < want) room <<= 1;
    char *grown = realloc(b->bytes, room);
    if (!grown) return -1;
    b->bytes = grown;
    b->room  = room;
    return 0;
}

/*
 * Parse one strictly-positive finite decimal (with optional exponent) starting
 * at *cur, advancing *cur past whatever it consumed. Sets *good to 1 only when a
 * usable magnitude was read; otherwise skips to the next array separator so the
 * caller can keep scanning. Anything the caller later rejects (<=0, non-finite)
 * is handled by band_add, so here we just decode the token.
 */
static double read_float(const char **cur, const char *stop, int *good) {
    const char *p = *cur;
    int sign = 1;

    if (p < stop && (*p == '+' || *p == '-')) {
        if (*p == '-') sign = -1;
        p++;
    }

    double mant = 0.0;
    int digits = 0;
    while (p < stop && *p >= '0' && *p <= '9') {
        mant = mant * 10.0 + (*p - '0');
        p++;
        digits++;
    }
    if (p < stop && *p == '.') {
        p++;
        double frac = 0.1;
        while (p < stop && *p >= '0' && *p <= '9') {
            mant += (*p - '0') * frac;
            frac *= 0.1;
            p++;
            digits++;
        }
    }
    if (!digits) {
        /* Not a number (e.g. the "NaN" token): swallow up to the next comma. */
        while (p < stop && *p != ',' && *p != ']') p++;
        *cur = p;
        *good = 0;
        return 0.0;
    }
    if (p < stop && (*p == 'e' || *p == 'E')) {
        p++;
        int esign = 1, e = 0;
        if (p < stop && (*p == '+' || *p == '-')) {
            if (*p == '-') esign = -1;
            p++;
        }
        while (p < stop && *p >= '0' && *p <= '9') {
            e = e * 10 + (*p - '0');
            p++;
        }
        mant *= pow(10.0, esign * e);
    }

    *cur = p;
    *good = 1;
    return sign * mant;
}

/* Fold one value into a band, keeping only strictly-positive finite numbers. */
static inline void band_add(band *b, double v) {
    if (v > 0.0 && isfinite(v)) {
        if (!b->valid) {
            b->lo = b->hi = v;
            b->valid = 1;
        } else {
            if (v < b->lo) b->lo = v;
            if (v > b->hi) b->hi = v;
        }
    }
}

/* Reduce a "[...]" flux array in [from, to) to its valid min/max. */
static band reduce_array(const char *from, const char *to) {
    band b = { 0.0, 0.0, 0 };
    const char *p = from;
    while (p < to && (*p == '"' || *p == '[')) p++;
    while (p < to && *p != ']') {
        char c = *p;
        if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
            int good = 0;
            double v = read_float(&p, to, &good);
            if (good) band_add(&b, v);
        } else {
            p++;
        }
    }
    return b;
}

/* Percentage swing of a band, or a sentinel below the 100 cutoff if empty. */
static inline double band_pct(const band *b) {
    return b->valid ? (b->hi - b->lo) / b->lo * 100.0 : -1.0;
}

/* Format one qualifying record into the file buffer. */
static int emit(outbuf *b, long long sid, const band *bp, const band *rp, double pct) {
    char row[288];
    char bmn[32] = "", bmx[32] = "", rmn[32] = "", rmx[32] = "";
    if (bp->valid) {
        snprintf(bmn, sizeof bmn, "%.17g", bp->lo);
        snprintf(bmx, sizeof bmx, "%.17g", bp->hi);
    }
    if (rp->valid) {
        snprintf(rmn, sizeof rmn, "%.17g", rp->lo);
        snprintf(rmx, sizeof rmx, "%.17g", rp->hi);
    }
    int n = snprintf(row, sizeof row, "%lld,%s,%s,%s,%s,%.17g\n",
                     sid, bmn, bmx, rmn, rmx, pct);
    if (n <= 0 || (size_t)n >= sizeof row) return -1;
    if (reserve(b, (size_t)n) != 0) return -1;
    memcpy(b->bytes + b->used, row, (size_t)n);
    b->used += (size_t)n;
    b->kept++;
    return 0;
}

/*
 * Walk a single record delimited by [line, line_end). Fields may be bare or
 * double-quoted (the flux arrays are quoted because they contain commas). We
 * advance field by field, decoding only the columns we care about, and stop
 * once the last needed column (rp_flux) has been consumed.
 */
static int process_record(const char *line, const char *line_end, outbuf *b) {
    long long sid = 0;
    band bp = { 0.0, 0.0, 0 };
    band rp = { 0.0, 0.0, 0 };
    int col = 0;
    const char *f = line;

    while (f <= line_end) {
        const char *f_end;
        const char *sep;
        if (f < line_end && *f == '"') {
            const char *close = memchr(f + 1, '"', (size_t)(line_end - f - 1));
            f_end = close ? close + 1 : line_end;
            sep = (close && close + 1 < line_end)
                      ? memchr(close + 1, ',', (size_t)(line_end - close - 1))
                      : NULL;
        } else {
            sep = memchr(f, ',', (size_t)(line_end - f));
            f_end = sep ? sep : line_end;
        }

        if (col == COL_SOURCE_ID) {
            sid = strtoll(f, NULL, 10);
        } else if (col == COL_BP_FLUX) {
            bp = reduce_array(f, f_end);
        } else if (col == COL_RP_FLUX) {
            rp = reduce_array(f, f_end);
            break; /* nothing further matters */
        }

        if (!sep) break;
        f = sep + 1;
        col++;
    }

    if (!bp.valid && !rp.valid) return 0;
    double pb = band_pct(&bp);
    double pr = band_pct(&rp);
    double pct = pb > pr ? pb : pr;
    if (pct > 100.0) return emit(b, sid, &bp, &rp, pct);
    return 0;
}

/* Inflate and scan one file, filling its output buffer. Returns 0 on success. */
static int scan_one(const char *path, outbuf *b) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat info;
    if (fstat(fd, &info) != 0) { close(fd); return -1; }
    size_t comp_len = (size_t)info.st_size;
    if (comp_len < 18) { close(fd); return -1; } /* smaller than a gzip frame */

    unsigned char *comp = mmap(NULL, comp_len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (comp == MAP_FAILED) return -1;

    /* The gzip trailer's ISIZE (mod 2^32) gives the exact plain size for a
       single-member stream, so we allocate once with no guessing. */
    uint32_t isize;
    memcpy(&isize, comp + comp_len - 4, 4);
    size_t plain_cap = isize ? (size_t)isize : comp_len * 20;

    char *plain = malloc(plain_cap + 1);
    if (!plain) { munmap(comp, comp_len); return -1; }

    libdeflate_decompressor *dec = deflate_open();
    size_t plain_len = 0;
    int rc = dec ? deflate_gunzip(dec, comp, comp_len,
                                  plain, plain_cap, &plain_len)
                 : -1;
    if (dec) deflate_close(dec);
    munmap(comp, comp_len);
    if (rc != 0) { free(plain); return -1; }
    plain[plain_len] = '\0';

    const char *p   = plain;
    const char *end = plain + plain_len;

    /* Drop the ECSV "#" comment block, then the single column-header line. */
    while (p < end && *p == '#') {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        p = nl ? nl + 1 : end;
    }
    if (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        p = nl ? nl + 1 : end;
    }

    int failed = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        if (line_end > p && *p >= '0' && *p <= '9') {
            if (process_record(p, line_end, b) != 0) { failed = 1; break; }
        }
        p = nl ? nl + 1 : end;
    }

    free(plain);
    return failed ? -1 : 0;
}

static long byte_size(const char *path) {
    struct stat s;
    return stat(path, &s) == 0 ? (long)s.st_size : 0;
}

/*
 * Entry point called from ctypes. `paths` is a caller-ordered vector of length
 * `count`; the CSV is written in that order regardless of scan order. Returns
 * the number of qualifying rows, or a negative value on failure.
 */
long flux_scan(const char **paths, int count, int threads, const char *out_path) {
    if (count <= 0) return -1;
    if (deflate_bind() != 0) return -1;
    if (threads > 0) omp_set_num_threads(threads);

    outbuf *bufs = calloc((size_t)count, sizeof *bufs);
    int    *sched = malloc((size_t)count * sizeof *sched);
    if (!bufs || !sched) { free(bufs); free(sched); return -1; }

    /* Schedule biggest compressed streams first so the longest inflate jobs are
       already running when the shorter ones start; output order is untouched. */
    long *sz = malloc((size_t)count * sizeof *sz);
    if (!sz) { free(bufs); free(sched); return -1; }
    for (int i = 0; i < count; i++) { sched[i] = i; sz[i] = byte_size(paths[i]); }
    for (int i = 1; i < count; i++) {
        int key = sched[i];
        long ks = sz[key];
        int j = i - 1;
        while (j >= 0 && sz[sched[j]] < ks) { sched[j + 1] = sched[j]; j--; }
        sched[j + 1] = key;
    }
    free(sz);

    int trouble = 0;
#pragma omp parallel for schedule(dynamic, 1)
    for (int s = 0; s < count; s++) {
        int i = sched[s];
        if (scan_one(paths[i], &bufs[i]) != 0) {
#pragma omp atomic write
            trouble = 1;
        }
    }
    free(sched);

    if (trouble) {
        for (int i = 0; i < count; i++) free(bufs[i].bytes);
        free(bufs);
        return -1;
    }

    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        for (int i = 0; i < count; i++) free(bufs[i].bytes);
        free(bufs);
        return -1;
    }

    static const char header[] =
        "source_id,bp_min_flux,bp_max_flux,rp_min_flux,rp_max_flux,percentage_change\n";
    fwrite(header, 1, sizeof header - 1, fp);

    long kept = 0;
    for (int i = 0; i < count; i++) {
        if (bufs[i].used) fwrite(bufs[i].bytes, 1, bufs[i].used, fp);
        kept += bufs[i].kept;
        free(bufs[i].bytes);
    }
    fclose(fp);
    free(bufs);
    return kept;
}
