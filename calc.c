// Asadbek Rakhmatillaev 251adb073
// https://github.com/asadbekrakh2007-cpu/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic AI assistant), conversation history: <PASTE SHARE LINK HERE>; C standard library man pages (fread, stat, mkdir)
 
// Makes mkdir/stat visible even under strict -std=c17 (which hides POSIX names).
#define _POSIX_C_SOURCE 200809L
 
#include <ctype.h>
#include <dirent.h>
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
typedef enum { T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW, T_LPAREN, T_RPAREN, T_EOL, T_BAD } TokType;
 
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
    else if (c == '*') {
        // Longest match: "**" is one operator, a lone '*' is multiplication.
        if (g_p < g_end && g_buf[g_p] == '*') { g_p++; g_tok.type = T_POW; }
        else g_tok.type = T_STAR;
    }
    else if (c == '/') g_tok.type = T_SLASH;
    else if (c == '(') g_tok.type = T_LPAREN;
    else if (c == ')') g_tok.type = T_RPAREN;
    else               g_tok.type = T_BAD;   // parser will fail exactly at this character
}
 
// ---------------------------------------------------------------------------
// Parser (recursive descent), one function per precedence level:
//   expr    := term    { ('+' | '-') term }
//   term    := power { ('*' | '/') power }
//   power   := primary [ '**' power ]
//   primary := NUMBER | '(' expr ')'
// Precedence comes from nesting: expr calls term, so when '+' is looked at,
// every '*', '/' and '**' around it has already been grouped (3 + 2 * 5 == 3 + (2*5)),
// and '**' binds tighter still because term calls power (2 * 3 ** 2 == 2 * (3**2)).
// The loops make all four operators LEFT-associative: 8 / 4 / 2 == (8/4)/2.
//
// Every syntax error is reported at g_tok.pos: the token the parser is looking
// at when it cannot continue (invalid char, second operator in a row, or the
// '\n' where an operand is missing).
// ---------------------------------------------------------------------------
static int parse_expr(long long *out);   // forward declaration: primary and expr call each other
 
static int parse_primary(long long *out) {
    if (g_tok.type == T_NUM) {
        *out = g_tok.value;
        advance();
        return 1;
    }
    if (g_tok.type == T_LPAREN) {
        advance();
        // The parentheses restart the grammar at the lowest level (expr), so
        // everything inside is grouped first: (2 + 3) * 4 == 5 * 4.
        // "()" fails inside parse_expr, at the ')' where an operand was expected.
        if (!parse_expr(out)) return 0;
        // A missing ')' is noticed at whatever follows the expression: for
        // "(2 + 3" that is the '\n', so the error is reported at the end of the line.
        if (g_tok.type != T_RPAREN) return fail(g_tok.pos);
        advance();
        return 1;
    }
    // Operator, ')', end of line or invalid character where an operand must be.
    return fail(g_tok.pos);
}
 
// Integer power by repeated squaring: O(log exp), so "1 ** 9000000000000" is
// instant. Unsigned arithmetic avoids signed-overflow UB. result starts at 1,
// which gives x ** 0 == 1 for every x, including 0 ** 0.
static long long int_pow(long long base, long long exp) {
    unsigned long long result = 1, b = (unsigned long long)base, e = (unsigned long long)exp;
    while (e > 0) {
        if (e & 1ULL) result *= b;
        b *= b;
        e >>= 1;
    }
    return (long long)result;
}
 
// Operands of one "**" chain are kept on this explicit stack (shared by all
// nesting levels; each parse_power call only uses the part above its `base`).
typedef struct {
    long long v;
    size_t start;   // where this operand starts (= the exponent position for the '**' before it)
} PowItem;
 
static PowItem *g_pstack;
static size_t   g_ptop, g_pcap;
 
static void pstack_push(PowItem it) {
    if (g_ptop == g_pcap) {
        size_t ncap = g_pcap ? g_pcap * 2 : 64;   // doubling: amortised O(1) per push
        PowItem *t = realloc(g_pstack, ncap * sizeof *t);
        if (!t) { fprintf(stderr, "calc: out of memory\n"); exit(1); }
        g_pstack = t;
        g_pcap = ncap;
    }
    g_pstack[g_ptop++] = it;
}
 
// ** is RIGHT-associative: 2 ** 3 ** 2 must be 2 ** (3 ** 2) == 512. The
// textbook rule `power := primary ['**' power]` gets this by recursing on the
// right side (power calls itself for the exponent). We give the same grouping
// iteratively: collect all operands of the chain, then fold from the right.
// Reason: a line like "1 ** 1 ** 1 ** ..." with thousands of terms would
// otherwise recurse that deep and could overflow the C stack, while the task
// only promises deep nesting for parentheses.
// All operands are parsed before any power is computed, so a syntax error later
// in the line is reported before an arithmetic error earlier in it.
static int parse_power(long long *out) {
    size_t base = g_ptop;
    for (;;) {
        PowItem it;
        it.start = g_tok.pos;
        if (!parse_primary(&it.v)) { g_ptop = base; return 0; }
        pstack_push(it);
        if (g_tok.type != T_POW) break;
        advance();   // consume '**'
    }
    long long acc = g_pstack[g_ptop - 1].v;   // right-most operand
    for (size_t i = g_ptop - 1; i > base; i--) {
        // A negative integer exponent is reported at the START OF THE EXPONENT:
        // that operand is what makes the power impossible with integers.
        if (acc < 0) { g_ptop = base; return fail(g_pstack[i].start); }
        acc = int_pow(g_pstack[i - 1].v, acc);
    }
    g_ptop = base;
    *out = acc;
    return 1;
}
 
static int parse_power(long long *out);
 
static int parse_term(long long *out) {
    long long left;
    if (!parse_power(&left)) return 0;
    while (g_tok.type == T_STAR || g_tok.type == T_SLASH) {
        int is_mul = (g_tok.type == T_STAR);
        advance();
        size_t right_pos = g_tok.pos;   // remembered: division by zero is reported HERE,
                                        // at the start of the right operand
        long long right;
        if (!parse_power(&right)) return 0;
        if (is_mul) {
            // Unsigned multiply: overflow wraps instead of being undefined behaviour.
            left = (long long)((unsigned long long)left * (unsigned long long)right);
        } else {
            if (right == 0) return fail(right_pos);
            // x / -1 is done as negation, because LLONG_MIN / -1 is undefined in C.
            if (right == -1) left = (long long)(0ULL - (unsigned long long)left);
            else             left = left / right;   // C truncates toward zero, as required
        }
    }
    *out = left;
    return 1;
}
 
static int parse_expr(long long *out) {
    long long left;
    if (!parse_term(&left)) return 0;
    while (g_tok.type == T_PLUS || g_tok.type == T_MINUS) {
        int plus = (g_tok.type == T_PLUS);
        advance();
        long long right;
        if (!parse_term(&right)) return 0;
        // Unsigned arithmetic so overflow wraps instead of being undefined behaviour.
        unsigned long long x = (unsigned long long)left, y = (unsigned long long)right;
        left = (long long)(plus ? x + y : x - y);
    }
    *out = left;
    return 1;
}
 
// Evaluates one line buf[start, end). Everything must be consumed: a token left
// over after the expression (typically an extra ')') is an error at that token.
static int eval_line(size_t start, size_t end, long long *out) {
    g_p = start;
    g_end = end;
    g_ptop = 0;   // a failed line may have left operands on the power stack
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
 
        // A blank line (only spaces/tabs) produces no output. A line whose FIRST
        // non-blank character is '#' is a comment and is skipped. We only skip by
        // index, so the comment's characters (and its '\n') still count for
        // positions. A '#' anywhere else reaches the lexer as an invalid character.
        if (j < le && buf[j] != '#') {
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
 
// Returns a malloc'ed copy of the last path component. Trailing '/' are ignored
// ("labs/" -> "labs"). With strip_txt, a final ".txt" is removed ("task1.txt" -> "task1").
static char *base_name(const char *path, int strip_txt) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    size_t m = n - s;
    if (strip_txt && m > 4 && memcmp(path + s + m - 4, ".txt", 4) == 0) m -= 4;
    char *r = malloc(m + 1);
    if (!r) { fprintf(stderr, "calc: out of memory\n"); exit(1); }
    memcpy(r, path + s, m);
    r[m] = '\0';
    return r;
}
 
// Creates `path` if missing (its parent is guaranteed to exist by the task).
// If it already exists we just write into it.
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
 
// Evaluates one input file and writes <out_dir>/<base>_<name>_<lastname>_<id>.txt.
// Returns 0 on success (including an ERROR:<pos> result, which is a normal
// outcome), 1 if the file cannot be read or the output cannot be written.
static int process_file(const char *in_path, const char *out_dir) {
    size_t len = 0;
    char *buf = read_file(in_path, &len);
    if (!buf) {
        fprintf(stderr, "calc: cannot read '%s': %s\n", in_path, strerror(errno));
        return 1;
    }
 
    OutBuf out = { NULL, 0, 0 };
    if (!evaluate(buf, len, &out)) {
        // No partial results: throw away what was collected, keep only the error.
        out.len = 0;
        char tmp[64];
        int n = snprintf(tmp, sizeof tmp, "ERROR:%zu\n", g_err_pos + 1);   // +1: positions are 1-based
        ob_append(&out, tmp, (size_t)n);
    }
    free(buf);
 
    int rc = 1;
    char *base = base_name(in_path, 1);
    size_t plen = strlen(out_dir) + strlen(base) + sizeof STUDENT_NAME + sizeof STUDENT_LASTNAME + sizeof STUDENT_ID + 16;
    char *out_path = malloc(plen);
    if (!out_path) { fprintf(stderr, "calc: out of memory\n"); exit(1); }
    snprintf(out_path, plen, "%s/%s_%s_%s_%s.txt", out_dir, base, STUDENT_NAME, STUDENT_LASTNAME, STUDENT_ID);
 
    if (ensure_dir(out_dir) == 0) {
        FILE *f = fopen(out_path, "wb");
        if (!f) {
            fprintf(stderr, "calc: cannot write '%s': %s\n", out_path, strerror(errno));
        } else {
            // fwrite with a NULL pointer is undefined even for 0 bytes; an all-comment file has no output.
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
 
static int usage(const char *msg) {
    fprintf(stderr, "calc: %s\n", msg);
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
    return 2;
}
 
// Evaluates every regular *.txt file directly inside `dir` (no recursion).
static int process_dir(const char *dir, const char *out_dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
        return 1;
    }
    if (ensure_dir(out_dir) != 0) { closedir(d); return 1; }
 
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl <= 4 || strcmp(e->d_name + nl - 4, ".txt") != 0) continue;
 
        size_t plen = strlen(dir) + nl + 2;
        char *path = malloc(plen);
        if (!path) { fprintf(stderr, "calc: out of memory\n"); exit(1); }
        snprintf(path, plen, "%s/%s", dir, e->d_name);
 
        // stat instead of d_type: d_type is not available in strict -std=c17 mode.
        // A subfolder named "x.txt" must be ignored, so we require a regular file.
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (process_file(path, out_dir) != 0) rc = 1;   // keep going, but remember the failure
        }
        free(path);
    }
    closedir(d);
    return rc;
}
 
int main(int argc, char **argv) {
    const char *dir = NULL, *out_dir = NULL, *input = NULL;
 
    for (int a = 1; a < argc; a++) {
        const char *s = argv[a];
        if (strcmp(s, "-d") == 0 || strcmp(s, "--dir") == 0) {
            if (++a >= argc) return usage("option needs a value: -d DIR");
            dir = argv[a];
        } else if (strcmp(s, "-o") == 0 || strcmp(s, "--output-dir") == 0) {
            if (++a >= argc) return usage("option needs a value: -o OUTDIR");
            out_dir = argv[a];
        } else if (s[0] == '-' && s[1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", s);
            return usage("unknown option");
        } else {
            if (input) return usage("only one input file is allowed");
            input = s;
        }
    }
    if (!dir && !input) return usage("no input file or folder given");
    if (dir && input)   return usage("give either an input file or -d DIR, not both");
 
    // Default output folder: <input_base>_<name>_<studentid> in the current folder.
    char *default_out = NULL;
    if (!out_dir) {
        char *base = base_name(dir ? dir : input, dir ? 0 : 1);
        size_t n = strlen(base) + sizeof STUDENT_NAME + sizeof STUDENT_ID + 4;
        default_out = malloc(n);
        if (!default_out) { fprintf(stderr, "calc: out of memory\n"); exit(1); }
        snprintf(default_out, n, "%s_%s_%s", base, STUDENT_NAME, STUDENT_ID);
        free(base);
        out_dir = default_out;
    }
 
    int rc = dir ? process_dir(dir, out_dir) : process_file(input, out_dir);
 
    free(default_out);
    free(g_pstack);   // no leaks, so the AddressSanitizer build stays quiet
    return rc;
}
 