// Asadbek Rakhmatillaev 251adb073
// https://github.com/asadbekrakh2007-cpu/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic AI assistant), conversation history: <https://claude.ai/chat/7d2a080b-8264-4c2a-acf8-d09aac79ce64>; C standard library man pages (fread, strtod, stat, mkdir, opendir)
 
// AI usage (Claude, Anthropic): I used it step by step, one grade level at a time.
// Prompts (full history is linked in line 4):
//   "read the lab task and tell me step by step what to do, give code with comments for each task"
//   "give ready code for task 1 (grade 4), then task 2, ... up to grade 10"
// I tested every step with my own tests in mytests/ and with calc-tests, and I can explain the code.
 
// Exposes mkdir/stat/opendir declarations even under -std=c17 (strict ISO mode hides POSIX names).
#define _POSIX_C_SOURCE 200809L
 
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
 
// ---------------------------------------------------------------------------
// Student identity. Used for output file / folder names (lowercase, as in the
// task examples: task1_anna_berzina_221234.txt).
// ---------------------------------------------------------------------------
#define STUDENT_NAME     "asadbek"
#define STUDENT_LASTNAME "rakhmatillaev"
#define STUDENT_ID       "251adb073"
 
// ---------------------------------------------------------------------------
// Values: a number is either an integer or a double. We keep a flag instead of
// always using double so that 64-bit integers stay exact (a double only has
// 53 bits of mantissa) and so that "7 / 2" can truncate like C.
// ---------------------------------------------------------------------------
typedef struct {
    int is_float;     // 0 = integer arithmetic, 1 = double arithmetic
    long long i;      // valid when is_float == 0
    double d;         // valid when is_float == 1
} Value;
 
static Value make_int(long long x)  { Value v; v.is_float = 0; v.i = x; v.d = 0.0; return v; }
static Value make_flt(double x)     { Value v; v.is_float = 1; v.i = 0; v.d = x;   return v; }
static double as_double(Value v)    { return v.is_float ? v.d : (double)v.i; }
 
// ---------------------------------------------------------------------------
// Tokenizer state.
// The whole input file is read into ONE buffer (g_buf) and never copied again.
// Tokens only remember an index into that buffer, so a position is simply
// "index in the file + 1". Because every character of the file (newlines and
// comment lines included) has an index, positions are automatically correct
// without separate line/column counters.
// ---------------------------------------------------------------------------
typedef enum {
    T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
    T_LPAREN, T_RPAREN,
    T_EOL,   // end of the current line (pos = index of its '\n', or file length)
    T_BAD    // a character that cannot start any token
} TokType;
 
typedef struct {
    TokType type;
    size_t pos;   // 0-based index of the token's first character in the file
    Value val;    // only for T_NUM
} Token;
 
static char  *g_buf;      // whole file, NUL-terminated (the NUL is needed by strtod trick below)
static size_t g_p;        // lexer cursor
static size_t g_end;      // end index (exclusive) of the line being parsed
static Token  g_tok;      // the one token of lookahead the parser is "looking at"
static size_t g_err_pos;  // 0-based position of the first error found
 
// Records an error at `pos` and returns 0 so callers can write `return fail(...)`.
// We use return codes instead of setjmp/longjmp because setjmp.h is not in the
// list of allowed headers.
static int fail(size_t pos) {
    g_err_pos = pos;
    return 0;
}
 
// AI (Claude): prompt "how to call strtod on a number inside a big buffer without copying it".
//   I used the idea of temporarily writing a NUL after the number (see below).
// Reads a number starting at g_p. Integers are accumulated by hand (no strtoll)
// so that a number is consumed exactly up to its last digit. A '.' is only part
// of the number when digits follow it: for "5." or ".5" the '.' is left behind
// and becomes a T_BAD token, which puts the error exactly at the '.', as the
// rules demand. The same happens for the 'e' in "1.5e3".
static void lex_number(void) {
    size_t start = g_p;
    unsigned long long acc = 0;  // unsigned: wrap-around is defined, signed overflow is UB
    while (g_p < g_end && isdigit((unsigned char)g_buf[g_p])) {
        acc = acc * 10ULL + (unsigned long long)(g_buf[g_p] - '0');
        g_p++;
    }
    g_tok.type = T_NUM;
    if (g_p + 1 < g_end && g_buf[g_p] == '.' && isdigit((unsigned char)g_buf[g_p + 1])) {
        g_p++;
        while (g_p < g_end && isdigit((unsigned char)g_buf[g_p])) g_p++;
        // strtod needs a NUL-terminated string. Instead of copying the number to a
        // scratch buffer, we temporarily put a NUL right after it (the buffer is
        // ours and has room for it) and restore the character: no allocation, no copy.
        char saved = g_buf[g_p];
        g_buf[g_p] = '\0';
        double d = strtod(g_buf + start, NULL);
        g_buf[g_p] = saved;
        g_tok.val = make_flt(d);
    } else {
        g_tok.val = make_int((long long)acc);
    }
}
 
// Produces the next token into g_tok. Tokens are produced on demand (one at a
// time) so memory use does not grow with the line length.
static void advance(void) {
    while (g_p < g_end && (g_buf[g_p] == ' ' || g_buf[g_p] == '\t')) g_p++;
    g_tok.pos = g_p;
    if (g_p >= g_end) { g_tok.type = T_EOL; return; }
 
    char c = g_buf[g_p];
    if (isdigit((unsigned char)c)) { lex_number(); return; }
 
    g_p++;  // every remaining token consumes at least this one character
    switch (c) {
        case '+': g_tok.type = T_PLUS;   break;
        case '-': g_tok.type = T_MINUS;  break;
        case '/': g_tok.type = T_SLASH;  break;
        case '(': g_tok.type = T_LPAREN; break;
        case ')': g_tok.type = T_RPAREN; break;
        case '*':
            // Longest match: "**" is one operator, a lone '*' is multiplication.
            if (g_p < g_end && g_buf[g_p] == '*') { g_p++; g_tok.type = T_POW; }
            else g_tok.type = T_STAR;
            break;
        // Anything else ('#' not at line start, letters, '&', '\r', ...) is invalid.
        // The parser will fail on it at g_tok.pos, i.e. at that very character.
        default:  g_tok.type = T_BAD;    break;
    }
}
 
// ---------------------------------------------------------------------------
// Arithmetic. Integer vs float decision: the result is an integer only if
// BOTH operands are integers; as soon as one is a float, both are converted to
// double. That is the rule from the task and it is checked in one place per op.
//
// Integer + - * use unsigned arithmetic and convert back, so overflow wraps
// instead of being undefined behaviour (the sanitizer build would complain).
// Float results are checked with isfinite(): inf/NaN is an error reported at
// the operator, because the operator is what "produced" the bad value.
// ---------------------------------------------------------------------------
static int apply_addsub(char op, Value a, Value b, size_t op_pos, Value *res) {
    if (!a.is_float && !b.is_float) {
        unsigned long long x = (unsigned long long)a.i, y = (unsigned long long)b.i;
        *res = make_int((long long)(op == '+' ? x + y : x - y));
        return 1;
    }
    double x = as_double(a), y = as_double(b);
    double r = (op == '+') ? x + y : x - y;
    if (!isfinite(r)) return fail(op_pos);
    *res = make_flt(r);
    return 1;
}
 
static int apply_mul(Value a, Value b, size_t op_pos, Value *res) {
    if (!a.is_float && !b.is_float) {
        *res = make_int((long long)((unsigned long long)a.i * (unsigned long long)b.i));
        return 1;
    }
    double r = as_double(a) * as_double(b);
    if (!isfinite(r)) return fail(op_pos);
    *res = make_flt(r);
    return 1;
}
 
// Division by zero is reported at the START OF THE RIGHT OPERAND (r_pos): that
// is the token that makes the division impossible. Non-finite float results
// (e.g. overflow) are reported at the operator like other float errors.
static int apply_div(Value a, Value b, size_t op_pos, size_t r_pos, Value *res) {
    if (!a.is_float && !b.is_float) {
        if (b.i == 0) return fail(r_pos);
        // x / -1 is computed as negation: LLONG_MIN / -1 would be undefined in C.
        if (b.i == -1) *res = make_int((long long)(0ULL - (unsigned long long)a.i));
        else           *res = make_int(a.i / b.i);   // C truncates toward zero, as required
        return 1;
    }
    double y = as_double(b);
    if (y == 0.0) return fail(r_pos);     // float division by zero is also an error
    double r = as_double(a) / y;
    if (!isfinite(r)) return fail(op_pos);
    *res = make_flt(r);
    return 1;
}
 
// Integer power by repeated squaring: O(log exp), so "1 ** 9000000000000"
// is instant. Unsigned arithmetic again avoids signed-overflow UB.
// result starts at 1, which also gives x ** 0 == 1 and 0 ** 0 == 1.
static long long int_pow(long long base, long long exp) {
    unsigned long long result = 1, b = (unsigned long long)base, e = (unsigned long long)exp;
    while (e > 0) {
        if (e & 1ULL) result *= b;
        b *= b;
        e >>= 1;
    }
    return (long long)result;
}
 
// A negative integer exponent is reported at the start of the exponent (e_pos).
// With a float operand we use pow(), where negative exponents are fine
// (0.5 ** (0-1) == 2); NaN/inf (e.g. (0-8) ** 0.5) is reported at the operator.
static int apply_pow(Value a, Value b, size_t op_pos, size_t e_pos, Value *res) {
    if (!a.is_float && !b.is_float) {
        if (b.i < 0) return fail(e_pos);
        *res = make_int(int_pow(a.i, b.i));
        return 1;
    }
    double r = pow(as_double(a), as_double(b));
    if (!isfinite(r)) return fail(op_pos);
    *res = make_flt(r);
    return 1;
}
 
// ---------------------------------------------------------------------------
// Parser: recursive descent, one function per precedence level.
//
//   expr    := term  { ('+' | '-') term }
//   term    := power { ('*' | '/') power }
//   power   := primary [ '**' power ]
//   primary := NUMBER | '(' expr ')'
//
// Precedence comes from nesting: expr calls term, term calls power, so by the
// time '+' is looked at, every '*' and '**' to its sides has already been
// grouped. Loops in expr/term give LEFT associativity (a - b - c == (a-b)-c).
//
// Error position rule: every syntax error is reported at g_tok.pos, the token
// the parser is looking at when it cannot continue (invalid char, second
// operator in a row, missing operand/')' at '\n', extra ')').
// ---------------------------------------------------------------------------
static int parse_expr(Value *out);
 
// Explicit stack for the operands of a "**" chain. See parse_power for why.
typedef struct {
    Value v;
    size_t start;   // where this operand starts (the exponent position for the previous '**')
    size_t op_pos;  // position of the '**' that FOLLOWS this operand
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
 
static int parse_primary(Value *out) {
    if (g_tok.type == T_NUM) {
        *out = g_tok.val;
        advance();
        return 1;
    }
    if (g_tok.type == T_LPAREN) {
        advance();
        if (!parse_expr(out)) return 0;
        // A missing ')' is noticed at whatever follows: at '\n' for "(2 + 3",
        // which is why that error is reported at the end of the line.
        if (g_tok.type != T_RPAREN) return fail(g_tok.pos);
        advance();
        return 1;
    }
    // Operator, ')', end of line or invalid char where an operand must be.
    return fail(g_tok.pos);
}
 
// AI (Claude): prompt "make ** right-associative without deep recursion for very long chains".
//   I used the idea of an explicit operand stack folded from the right.
// ** is right-associative: 2 ** 3 ** 2 must be 2 ** (3 ** 2). The textbook way
// is `power := primary ['**' power]`, i.e. recursion on the right side. We do
// the same grouping but iteratively: collect operands of the whole chain, then
// fold from the right. Reason: a test line like "1 ** 1 ** 1 ** ..." thousands
// of terms long would otherwise recurse thousands of levels deep and could
// overflow the C stack, while the task only promises depth for parentheses.
// All operands are parsed before any power is evaluated, so a syntax error
// later in the chain is reported before an evaluation error earlier in it,
// exactly as the recursive version would do.
static int parse_power(Value *out) {
    size_t base = g_ptop;   // our chain lives above whatever outer calls pushed
    for (;;) {
        PowItem it;
        it.start = g_tok.pos;
        it.op_pos = 0;
        if (!parse_primary(&it.v)) { g_ptop = base; return 0; }
        int more = (g_tok.type == T_POW);
        if (more) it.op_pos = g_tok.pos;
        pstack_push(it);   // nested parse_primary calls finished before this push, so indices stay valid
        if (!more) break;
        advance();         // consume '**'
    }
    Value acc = g_pstack[g_ptop - 1].v;       // right-most operand
    for (size_t i = g_ptop - 1; i > base; i--) {
        PowItem left = g_pstack[i - 1];
        // exponent starts where operand i starts; the operator is the one after operand i-1
        if (!apply_pow(left.v, acc, left.op_pos, g_pstack[i].start, &acc)) { g_ptop = base; return 0; }
    }
    g_ptop = base;
    *out = acc;
    return 1;
}
 
static int parse_term(Value *out) {
    Value left;
    if (!parse_power(&left)) return 0;
    while (g_tok.type == T_STAR || g_tok.type == T_SLASH) {
        char op = (g_tok.type == T_STAR) ? '*' : '/';
        size_t op_pos = g_tok.pos;
        advance();
        size_t r_pos = g_tok.pos;   // remembered for the division-by-zero position
        Value right;
        if (!parse_power(&right)) return 0;
        int ok = (op == '*') ? apply_mul(left, right, op_pos, &left)
                             : apply_div(left, right, op_pos, r_pos, &left);
        if (!ok) return 0;
    }
    *out = left;
    return 1;
}
 
static int parse_expr(Value *out) {
    Value left;
    if (!parse_term(&left)) return 0;
    while (g_tok.type == T_PLUS || g_tok.type == T_MINUS) {
        char op = (g_tok.type == T_PLUS) ? '+' : '-';
        size_t op_pos = g_tok.pos;
        advance();
        Value right;
        if (!parse_term(&right)) return 0;
        if (!apply_addsub(op, left, right, op_pos, &left)) return 0;
    }
    *out = left;
    return 1;
}
 
// Evaluates the line buf[start, end). The whole line must be consumed: a token
// left over after the expression (typically an extra ')') is an error at that token.
static int eval_line(size_t start, size_t end, Value *out) {
    g_p = start;
    g_end = end;
    g_ptop = 0;
    advance();
    if (!parse_expr(out)) return 0;
    if (g_tok.type != T_EOL) return fail(g_tok.pos);
    return 1;
}
 
// ---------------------------------------------------------------------------
// Output buffer. Results are collected in memory and written once at the end,
// because "if any error occurs, the file contains ONLY the error" - we cannot
// know that until the last line has been checked. Doubling growth keeps the
// total copying work linear in the output size.
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
 
// Walks the file line by line IN PLACE (no per-line copies; memchr finds the
// '\n'). Returns 1 and fills `out` if every line is fine, otherwise returns 0
// with g_err_pos set to the first error (lines are processed in order, so the
// first failure is the first error in the file).
static int evaluate(char *buf, size_t len, OutBuf *out) {
    g_buf = buf;
    size_t i = 0;
    while (i < len) {
        const char *nl = memchr(buf + i, '\n', len - i);
        size_t le = nl ? (size_t)(nl - buf) : len;   // le = index of '\n' (or file length)
 
        size_t j = i;
        while (j < le && (buf[j] == ' ' || buf[j] == '\t')) j++;
 
        // Blank line -> no output. First non-blank '#' -> comment line, ignored
        // (its characters still count for positions because we only skip by index).
        if (j < le && buf[j] != '#') {
            Value v;
            if (!eval_line(j, le, &v)) return 0;
            char tmp[64];
            int n = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.d)
                               : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
            ob_append(out, tmp, (size_t)n);
        }
        i = le + 1;
    }
    return 1;
}
 
// ---------------------------------------------------------------------------
// File and path helpers
// ---------------------------------------------------------------------------
 
// AI (Claude): prompt "how do I read a whole file into memory in C without knowing its size".
//   I used the realloc doubling idea; the loop below is written/checked by me.
// Reads a whole file into one heap buffer. The size is not known in advance
// (works for any file type), so the buffer doubles when full: each byte is
// copied O(1) times on average instead of re-reading or re-copying per line.
// One extra byte is kept for the terminating NUL used by lex_number.
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
    if (ferror(f)) { free(buf); fclose(f); return NULL; }   // e.g. a folder was given as a file
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