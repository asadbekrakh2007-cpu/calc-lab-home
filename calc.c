// Asadbek Rakhmatillaev 251ADB073
// https://github.com/asadbekrakh2007-cpu/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic AI assistant), conversation history: <PASTE SHARE LINK HERE>; C standard library man pages (fread, stat, mkdir)
 
// Makes mkdir/stat visible even under strict -std=c17 (which hides POSIX names).
#define _POSIX_C_SOURCE 200809L
 
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
 
// Used in output file names, lowercase (e.g. task1_anna_berzina_221234.txt).
// CHANGE THESE THREE LINES.
#define STUDENT_NAME     "asadbek"
#define STUDENT_LASTNAME "rakhmatillaev"
#define STUDENT_ID       "251adb073"
 
// ---------------------------------------------------------------------------
// Tokenizer. The whole file lives in ONE buffer (g_buf) and is never copied.
// A token only remembers its index in that buffer, so a position is just
// "index + 1". Newlines are part of the buffer, so they are counted for free.
// ---------------------------------------------------------------------------
typedef enum { T_NUM, T_PLUS, T_MINUS, T_EOL, T_BAD } TokType;
 
typedef struct {
    TokType type;
    size_t pos;       // 0-based index of the token's first character in the file
    long long value;  // only for T_NUM
} Token;
 
static char  *g_buf;      // the whole input file
static size_t g_p;        // lexer cursor
static size_t g_end;      // end index (exclusive) of the line being parsed
static Token  g_tok;      // the token the parser is currently looking at
static size_t g_err_pos;  // 0-based position of the first error
 
// Remember an error position and return 0, so callers can `return fail(...)`.
static int fail(size_t pos) {
    g_err_pos = pos;
    return 0;
}
 
// Produces the next token on demand (one at a time, no token array).
static void advance(void) {
    while (g_p < g_end && (g_buf[g_p] == ' ' || g_buf[g_p] == '\t')) g_p++;
    g_tok.pos = g_p;
    if (g_p >= g_end) { g_tok.type = T_EOL; return; }
 
    char c = g_buf[g_p];
    if (isdigit((unsigned char)c)) {
        // Unsigned accumulation: wrap-around is defined, signed overflow is not.
        unsigned long long acc = 0;
        while (g_p < g_end && isdigit((unsigned char)g_buf[g_p])) {
            acc = acc * 10ULL + (unsigned long long)(g_buf[g_p] - '0');
            g_p++;
        }
        g_tok.type = T_NUM;
        g_tok.value = (long long)acc;
        return;
    }
    g_p++;
    if (c == '+')      g_tok.type = T_PLUS;
    else if (c == '-') g_tok.type = T_MINUS;
    else               g_tok.type = T_BAD;   // parser will fail exactly at this character
}
 
// ---------------------------------------------------------------------------
// Parser (recursive descent). Grade 4 only needs:
//   expr    := primary { ('+' | '-') primary }
//   primary := NUMBER
// The loop makes + and - LEFT-associative: 10 - 3 + 1 == (10 - 3) + 1.
// Later grades add more levels between expr and primary.
// ---------------------------------------------------------------------------
static int parse_primary(long long *out) {
    if (g_tok.type != T_NUM) return fail(g_tok.pos);   // error = token we are looking at
    *out = g_tok.value;
    advance();
    return 1;
}
 
static int parse_expr(long long *out) {
    long long left;
    if (!parse_primary(&left)) return 0;
    while (g_tok.type == T_PLUS || g_tok.type == T_MINUS) {
        int plus = (g_tok.type == T_PLUS);
        advance();
        long long right;
        if (!parse_primary(&right)) return 0;
        // Unsigned arithmetic so overflow wraps instead of being undefined behaviour.
        unsigned long long x = (unsigned long long)left, y = (unsigned long long)right;
        left = (long long)(plus ? x + y : x - y);
    }
    *out = left;
    return 1;
}
 
// Evaluates one line buf[start, end). Everything must be consumed.
static int eval_line(size_t start, size_t end, long long *out) {
    g_p = start;
    g_end = end;
    advance();
    if (!parse_expr(out)) return 0;
    if (g_tok.type != T_EOL) return fail(g_tok.pos);
    return 1;
}
 
// ---------------------------------------------------------------------------
// Output buffer. Results are kept in memory and written once at the end,
// because if any line fails the file must contain ONLY the error.
// Doubling growth keeps total copying linear.
// ---------------------------------------------------------------------------
typedef struct { char *data; size_t len, cap; } OutBuf;
 
static void ob_append(OutBuf *o, const char *s, size_t n) {
    if (o->len + n > o->cap) {
        size_t ncap = o->cap ? o->cap : 4096;
        while (ncap < o->len + n) ncap *= 2;
        char *t = realloc(o->data, ncap);
        if (!t) { fprintf(stderr, "calc: out of memory\n"); exit(1); }
        o->data = t;
        o->cap = ncap;
    }
    memcpy(o->data + o->len, s, n);
    o->len += n;
}
 
// Walks the file line by line IN PLACE (memchr finds each '\n'; no per-line copy).
// Lines are processed in order, so the first failure is the first error.
static int evaluate(char *buf, size_t len, OutBuf *out) {
    g_buf = buf;
    size_t i = 0;
    while (i < len) {
        const char *nl = memchr(buf + i, '\n', len - i);
        size_t le = nl ? (size_t)(nl - buf) : len;   // index of '\n' (or file length)
 
        size_t j = i;
        while (j < le && (buf[j] == ' ' || buf[j] == '\t')) j++;
 
        if (j < le) {   // blank lines (only spaces/tabs) produce no output
            long long v;
            if (!eval_line(j, le, &v)) return 0;
            char tmp[64];
            int n = snprintf(tmp, sizeof tmp, "%lld\n", v);
            ob_append(out, tmp, (size_t)n);
        }
        i = le + 1;
    }
    return 1;
}
 
// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
 
// Reads the whole file into one buffer. Size is unknown in advance, so the
// buffer doubles when full: every byte is copied O(1) times on average.
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        size_t got = fread(buf + len, 1, cap - len - 1, f);
        if (got == 0) break;
        len += got;
        if (len + 1 >= cap) {
            cap *= 2;
            char *t = realloc(buf, cap);
            if (!t) { free(buf); fclose(f); return NULL; }
            buf = t;
        }
    }
    if (ferror(f)) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}
 
// Returns a malloc'ed copy of the file name without folders and without ".txt".
static char *base_name(const char *path) {
    size_t n = strlen(path), s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    size_t m = n - s;
    if (m > 4 && memcmp(path + s + m - 4, ".txt", 4) == 0) m -= 4;
    char *r = malloc(m + 1);
    if (!r) { fprintf(stderr, "calc: out of memory\n"); exit(1); }
    memcpy(r, path + s, m);
    r[m] = '\0';
    return r;
}
 
// Creates the folder if missing (its parent exists); an existing one is reused.
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        fprintf(stderr, "calc: '%s' exists but is not a folder\n", path);
        return 1;
    }
    if (mkdir(path, 0777) != 0) {
        fprintf(stderr, "calc: cannot create folder '%s': %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}
 
// Evaluates one file and writes <out_dir>/<base>_<name>_<lastname>_<id>.txt
static int process_file(const char *in_path, const char *out_dir) {
    size_t len = 0;
    char *buf = read_file(in_path, &len);
    if (!buf) {
        fprintf(stderr, "calc: cannot read '%s': %s\n", in_path, strerror(errno));
        return 1;
    }
 
    OutBuf out = { NULL, 0, 0 };
    if (!evaluate(buf, len, &out)) {
        out.len = 0;   // no partial results: keep only the error
        char tmp[64];
        int n = snprintf(tmp, sizeof tmp, "ERROR:%zu\n", g_err_pos + 1);   // positions are 1-based
        ob_append(&out, tmp, (size_t)n);
    }
    free(buf);
 
    int rc = 1;
    char *base = base_name(in_path);
    size_t plen = strlen(out_dir) + strlen(base) + sizeof STUDENT_NAME + sizeof STUDENT_LASTNAME + sizeof STUDENT_ID + 16;
    char *out_path = malloc(plen);
    if (!out_path) { fprintf(stderr, "calc: out of memory\n"); exit(1); }
    snprintf(out_path, plen, "%s/%s_%s_%s_%s.txt", out_dir, base, STUDENT_NAME, STUDENT_LASTNAME, STUDENT_ID);
 
    if (ensure_dir(out_dir) == 0) {
        FILE *f = fopen(out_path, "wb");
        if (!f) {
            fprintf(stderr, "calc: cannot write '%s': %s\n", out_path, strerror(errno));
        } else {
            // fwrite with a NULL pointer is undefined even for 0 bytes (empty output).
            size_t w = out.len ? fwrite(out.data, 1, out.len, f) : 0;
            int cl = fclose(f);
            if (w != out.len || cl != 0) fprintf(stderr, "calc: error while writing '%s'\n", out_path);
            else rc = 0;
        }
    }
    free(out_path);
    free(base);
    free(out.data);
    return rc;
}
 
// Grade 4 command line: calc -o OUTDIR input.txt  (full CLI comes in grade 9)
int main(int argc, char **argv) {
    const char *out_dir = argc == 4 ? argv[2] : NULL;
    const char *input   = argc == 4 ? argv[3] : NULL;
    if (!out_dir || !input || strcmp(argv[1], "-o") != 0) {
        fprintf(stderr, "usage: calc -o OUTDIR input.txt\n");
        return 2;
    }
    return process_file(input, out_dir);
}
 