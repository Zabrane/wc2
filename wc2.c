/*
    Implements the Unix command-line program 'wc' (word-count) program.
    This is a demonstration of using 'asynchronous state machines' as the
    core logic.
    Includes UTF-8 parsing.
*/
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define _FILE_OFFSET_BITS   64
#include <stdio.h>
#include <ctype.h>
#include <wctype.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <Windows.h>
#endif

/* Windows thing */
#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/**
 * The global "state-machine" table that we create on startup.
 */
unsigned char table[256][256];

/**
 * This is a translation of the above table, using pointers
 * instead of integer offsets, to remove one calculation in
 * the inner-loop. This is activated when the '-P' option
 * is set on the command-line.
 */
void *table_p[256][256];

/**
 * Translate from a numeric pointer to somehwere in 'table_p' to
 * the integer row number for 'table'.
 */
#define PSTATE(p) (((char*)p - (char*)table_p)/(256 * sizeof(void*)))

/**
 * Hold the configuration parsed from the command-line
 */
struct config {
    size_t file_count;
    int is_stdin;
    int is_counting_lines;
    int is_counting_words;
    int is_counting_bytes;
    int is_counting_chars;
    int is_printing_totals;
    unsigned column_width;
    int is_pointer_arithmetic;
};

/**
 * Holds the counts from reading a chunk, a file, or totals.
 */
struct results {
    unsigned long line_count;
    unsigned long word_count;
    unsigned long char_count;
    unsigned long byte_count;
};


/*
| bytes | bits |  first  |   last   |   byte1  |   byte2  |   byte3  |   byte4  |
|:-----:|:----:|:-------:|:--------:|:--------:|:--------:|:--------:|:--------:|
|   1   |    7 |  U+0000 |   U+007F | 0xxxxxxx |          |          |          |
|   2   |   11 |  U+0080 |   U+07FF | 110xxxxx | 10xxxxxx |          |          |
|   3   |   16 |  U+0800 |   U+FFFF | 1110xxxx | 10xxxxxx | 10xxxxxx |          |
|   4   |   21 | U+10000 | U+10FFFF | 11110xxx | 10xxxxxx | 10xxxxxx | 10xxxxxx |
 */
enum {
    DUO2_xx,
    DUO2_C2,
    TRI2_E0,
    TRI2_E1,
    TRI2_E2,
    TRI2_E3,
    TRI2_ED,
    TRI2_EE,
    TRI2_xx,
    TRI3_E0_xx,
    TRI3_E1_xx,
    TRI3_E1_9a,
    TRI3_E2_80,
    TRI3_E2_81,
    TRI3_E2_xx,
    TRI3_E3_80,
    TRI3_E3_81,
    TRI3_E3_xx,
    TRI3_Ed_xx,
    TRI3_Ee_xx,
    TRI3_xx_xx,
    QUAD2_xx,
    QUAD2_F0,
    QUAD2_F4,
    QUAD3_xx_xx,
    QUAD3_F0_xx,
    QUAD3_F4_xx,
    QUAD4_xx_xx_xx,
    QUAD4_F0_xx_xx,
    QUAD4_F4_xx_xx,
    ILLEGAL
};

enum {
    WASSPACE = 0,
    NEWLINE,
    NEWWORD,
    WASWORD,
    USPACE,
    UWORD=USPACE+ILLEGAL+1,
    STATE_MAX=UWORD+ILLEGAL+1
};

/**
 * Build an ASCII row. This configures low-order 7-bits, which should
 * be roughly the same for all states
 */
static void
build_basic(unsigned char *row, unsigned char default_state, unsigned char ubase)
{
    unsigned c;
    for (c=0; c<256; c++) {
        if ((c & 0x80)) {
            if ((c & 0xE0) == 0xC0) {
                /* 110x xxxx - unicode 2 byte sequence */
                if (c < 0xC2)
                    row[c] = ubase + ILLEGAL;
                else if (c == 0xC2)
                    row[c] = ubase + DUO2_C2;
                else
                    row[c] = ubase + DUO2_xx;
            } else if ((c & 0xF0) == 0xE0) {
                /* 1110 xxxx - unicode 3 byte sequence */
                switch (c) {
                    case 0xE0:
                        row[c] = ubase + TRI2_E0;
                        break;
                    case 0xE1:
                        row[c] = ubase + TRI2_E1;
                        break;
                    case 0xE2:
                        row[c] = ubase + TRI2_E2;
                        break;
                    case 0xE3:
                        row[c] = ubase + TRI2_E3;
                        break;
                    case 0xEd:
                        row[c] = ubase + TRI2_ED;
                        break;
                    case 0xEe:
                        row[c] = ubase + TRI2_EE;
                        break;
                    default:
                        row[c] = ubase + TRI2_xx;
                        break;
                }
            } else if ((c & 0xF8) == 0xF0) {
                if (c >= 0xF5)
                    row[c] = ubase + ILLEGAL;
                else if (c == 0xF0)
                    row[c] = ubase + QUAD2_F0;
                else if (c == 0xF4)
                    row[c] = ubase + QUAD2_F4;
                else
                    row[c] = ubase + QUAD2_xx;
            } else
                row[c] = ubase + ILLEGAL;
        } else if (c == '\n')
            row[c] = NEWLINE;
        else if (isspace(c))
            row[c] = WASSPACE;
        else
            row[c] = default_state;
    }
}

void build_WASSPACE(unsigned char *row)
{
    build_basic(row, NEWWORD, USPACE);
}

void build_WASWORD(unsigned char *row)
{
    build_basic(row, WASWORD, UWORD);
}


void build_urow(unsigned ubase, unsigned id, unsigned next)
{
    size_t i;
    unsigned default_state;

    default_state = table[ubase + ILLEGAL][0];

    if (next == 0)
        next = default_state;
    else
        next = ubase + next;

    memcpy(table[ubase + id], table[ubase + ILLEGAL], 256);

    for (i=0x80; i<0xC0; i++) {
        table[ubase + id][i] = next;
    }
    for (i=0xC0; i<0x100; i++) {
        table[ubase + id][i] = ubase + ILLEGAL;
    }

}
void build_unicode(unsigned char default_state, unsigned ubase)
{
    size_t i;

    build_basic(table[ubase + ILLEGAL], default_state, ubase);

    /*
     * Two byte
     */
    build_urow(ubase, DUO2_xx, 0);
    build_urow(ubase, DUO2_C2, 0);

    /*
     * Three byte
     */
    build_urow(ubase, TRI2_E0, TRI3_E0_xx);
    build_urow(ubase, TRI2_E1, TRI3_E1_xx);
    build_urow(ubase, TRI2_E2, TRI3_E2_xx);
    build_urow(ubase, TRI2_E3, TRI3_E3_xx);
    build_urow(ubase, TRI2_ED, TRI3_Ed_xx);
    build_urow(ubase, TRI2_EE, TRI3_Ee_xx);
    build_urow(ubase, TRI2_xx, TRI3_xx_xx);

    build_urow(ubase, TRI3_E0_xx, 0);
    build_urow(ubase, TRI3_E1_xx, 0);
    build_urow(ubase, TRI3_E1_9a, 0);
    build_urow(ubase, TRI3_E2_80, 0);
    build_urow(ubase, TRI3_E2_81, 0);
    build_urow(ubase, TRI3_E2_xx, 0);
    build_urow(ubase, TRI3_E3_80, 0);
    build_urow(ubase, TRI3_E3_81, 0);
    build_urow(ubase, TRI3_E3_xx, 0);
    build_urow(ubase, TRI3_Ed_xx, 0);
    build_urow(ubase, TRI3_Ee_xx, 0);
    build_urow(ubase, TRI3_xx_xx, 0);

    table[ubase + TRI2_E1][0x9a] = ubase + TRI3_E1_9a;
    table[ubase + TRI2_E2][0x80] = ubase + TRI3_E2_80;
    table[ubase + TRI2_E2][0x81] = ubase + TRI3_E2_81;
    table[ubase + TRI2_E3][0x80] = ubase + TRI3_E3_80;
    table[ubase + TRI2_E3][0x81] = ubase + TRI3_E3_81;


    /*
     * Four byte
     */
    build_urow(ubase, QUAD2_xx, QUAD3_xx_xx);
    build_urow(ubase, QUAD2_F0, QUAD3_F0_xx);
    build_urow(ubase, QUAD2_F4, QUAD3_F4_xx);

    build_urow(ubase, QUAD3_xx_xx, QUAD4_xx_xx_xx);
    build_urow(ubase, QUAD3_F0_xx, QUAD4_F0_xx_xx);
    build_urow(ubase, QUAD3_F4_xx, QUAD4_F4_xx_xx);

    build_urow(ubase, QUAD4_xx_xx_xx, 0);
    build_urow(ubase, QUAD4_F0_xx_xx, 0);
    build_urow(ubase, QUAD4_F4_xx_xx, 0);

    /*
     * Mark Unicode spaces
     */
    if (iswspace(0x0085))
        table[ubase + DUO2_C2][0x85] = WASSPACE;
    if (iswspace(0x00A0))
        table[ubase + DUO2_C2][0xA0] = WASSPACE;
    if (iswspace(0x1680)) /* 0x1680 = 0xe1 0x9a 0x80 = OGHAM SPACE MARK*/
        table[ubase + TRI3_E1_9a][0x80] = WASSPACE;
    for (i=0x2000; i<0x200b+1; i++) {
        if (iswspace(i))
            table[ubase + TRI3_E2_80][0x80 + (i&0x6F)] = WASSPACE;
    }
    if (iswspace(0x2028))
        table[ubase + TRI3_E2_80][0xA8] = WASSPACE;
    if (iswspace(0x2029))
        table[ubase + TRI3_E2_80][0xA9] = WASSPACE;
    if (iswspace(0x202F))
        table[ubase + TRI3_E2_80][0xAF] = WASSPACE;
    if (iswspace(0x205F))
        table[ubase + TRI3_E2_81][0x9F] = WASSPACE;
    if (iswspace(0x3000))
        table[ubase + TRI3_E3_80][0x80] = WASSPACE;


    /*
     * Mark illegal sequences
     *
     * The following need to be marked as illegal because they can
     * be represented with a shorter string. In other words,
     * 0xC0 0x81 is the same as 0x01, so needs to be marked as an
     * illegal sequence.
     */
    for (i=0x80; i<0xA0; i++) {
        table[ubase + TRI2_E0][i] = ubase + ILLEGAL;
    }
    for (i=0x80; i<0x90; i++) {
        table[ubase + QUAD2_F0][i] = ubase + ILLEGAL;
    }
    /* Exceeds max possible size of unicode character */
    for (i=0x90; i<0xC0; i++) {
        table[ubase + QUAD2_F4][i] = ubase + ILLEGAL;
    }
    /* Surrogate space */
    for (i=0xA0; i<0xC0; i++) {
        table[ubase + TRI2_ED][i] = ubase + ILLEGAL;
    }

}



/**
 * For pointer-arithmetic version of the inner loop, converts
 * the integer indexes to precomputed pointers. This table
 * will be used when '-P' option is set on the command-line.
 */
static void
compile_pointers(void)
{
    size_t i;
    size_t j;

    for (i=0; i<STATE_MAX; i++) {
        for (j=0; j<256; j++) {
            table_p[i][j] = (char*)table_p + table[i][j]*256*sizeof(void*);
            assert(PSTATE(table_p[i][j]) == table[i][j]);
        }
    }
}

/**
 * This function compiles a DFA-style state-machine for parsing UTF-8
 * variable-length byte sequences.
 */
static void
compile_utf8_statemachine(int is_multibyte)
{
    if (is_multibyte) {
        setlocale(LC_ALL, "");
        build_WASSPACE(table[WASSPACE]);
        build_WASSPACE(table[NEWLINE]);
        build_WASWORD(table[WASWORD]);
        build_WASWORD(table[NEWWORD]);
        build_unicode(NEWWORD, USPACE);
        build_unicode(WASWORD, UWORD);
    } else {
        int c;
        setlocale(LC_ALL, "");
        for (c=0; c<256; c++) {
            if (c == '\n') {
                table[WASSPACE][c] = NEWLINE;
                table[NEWLINE][c] = NEWLINE;
                table[NEWWORD][c] = NEWLINE;
                table[WASWORD][c] = NEWLINE;
            } else if (isspace(c)) {
                table[WASSPACE][c] = WASSPACE;
                table[NEWLINE][c] = WASSPACE;
                table[NEWWORD][c] = WASSPACE;
                table[WASWORD][c] = WASSPACE;
            } else {
                table[WASSPACE][c] = NEWWORD;
                table[NEWLINE][c] = NEWWORD;
                table[NEWWORD][c] = WASWORD;
                table[WASWORD][c] = WASWORD;
            }
        }
    }
}


/**
 * Print the results structure. We need to make sure there is a space
 * between each of the fields, though not before the first field, and
 * not after the last field. Every field is optional, depending upon
 * whether the corresponding parameter was specified on the command-line.
 * So that printing multiple results line up, we also have a consistent
 * column-width for all the columns.
 */
static void
print_results(const char *filename, struct results *results, struct config *cfg)
{
    int needs_space = 0; /* space needed between output */
    unsigned width = cfg->column_width;

    /* -l */
    if (cfg->is_counting_lines)
        printf("%s%*lu", needs_space++?" ":"", width, results->line_count);

    /* -w */
    if (cfg->is_counting_words)
        printf("%s%*lu", needs_space++?" ":"", width, results->word_count);

    /* -c */
    if (cfg->is_counting_bytes)
        printf("%s%*lu", needs_space++?" ":"", width, results->byte_count);

    /* -m */
    if (cfg->is_counting_chars)
        printf("%s%*lu", needs_space++?" ":"", width, results->char_count);

    /* NULL if <stdin>, "total" for the last line showing totals, otherwise,
     * the name of the file that was processed */
    if (filename)
        printf("%s%s", needs_space++?" ":"", filename);
    printf("\n");
}


/**
 * A pointer-arithmetic version for tracking state. This alternative
 * will be called when '-P' is specified on the command-line.
 */
static struct results
parse_chunk_pp(const unsigned char *buf, size_t length, unsigned *inout_state)
{
    void **state = (void**)((char*)table_p + *inout_state);
    const unsigned char *end = buf + length;
    unsigned long counts[STATE_MAX];


    state = (void**)((char*)table_p + (*inout_state) * 256 * sizeof(void*));

    /* We only care about the first four states, so these will be initialized to zero.
     * Since we don't use the other ~100 counts for the other states, we won't initialize them */
    counts[NEWLINE] = 0;
    counts[NEWWORD] = 0;
    counts[WASSPACE] = 0;
    counts[WASWORD] = 0;

    /* This is the inner-loop where 99.9% of the execution time of this program will
     * be spent. */
    while (buf < end) {
        unsigned char c = *buf++;
        state = state[c];
        counts[PSTATE(state)]++;
    }

    /* Save the ending state for the next chunk */
    *inout_state = PSTATE(state);

    /* Return the results */
    {
        struct results results;
        results.line_count = counts[NEWLINE];
        results.word_count = counts[NEWWORD];
        results.char_count = counts[NEWLINE] + counts[WASSPACE] + counts[WASWORD] + counts[NEWWORD];
        results.byte_count = length;

        return results;
    }
}

/**
 * Same as 'parse-chunk', but with pointer instead of index
 */
static struct results
parse_chunk_p(const unsigned char *buf, size_t length, unsigned *inout_state)
{
    size_t state = *inout_state;
    const unsigned char *end;
    unsigned counts[STATE_MAX];
    unsigned char c;

    /* We only care about the first four states, so these will be initialized to zero.
     * Since we don't use the other ~100 counts for the other states, we won't initialize them */
    counts[NEWLINE] = 0;
    counts[NEWWORD] = 0;
    counts[WASSPACE] = 0;
    counts[WASWORD] = 0;

    /* This is the inner-loop where 99.9% of the execution time of this program will
     * be spent. */
    end = buf + length;
    while (buf < end) {
        c = *buf++;
        state = table[state][c];
        counts[state]++;
    }

    /* Save the ending state for the next chunk */
    *inout_state = state;

    /* Return the results */
    {
        struct results results;
        results.line_count = counts[NEWLINE];
        results.word_count = counts[NEWWORD];
        results.char_count = counts[NEWLINE] + counts[WASSPACE] + counts[WASWORD] + counts[NEWWORD];
        results.byte_count = length;

        return results;
    }
}


/**
 * Parse a single 64k chunk. Since a word can cross a chunk
 * boundary, we have to remember the 'state' from a previous
 * chunk.
 */
static struct results
parse_chunk(const unsigned char *buf, size_t length, unsigned *inout_state)
{
    size_t state = *inout_state;
    size_t i;
    unsigned counts[STATE_MAX];
    unsigned char c;

    /* We only care about the first four states, so these will be initialized to zero.
     * Since we don't use the other ~100 counts for the other states, we won't initialize them */
    counts[NEWLINE] = 0;
    counts[NEWWORD] = 0;
    counts[WASSPACE] = 0;
    counts[WASWORD] = 0;

    /* This is the inner-loop where 99.9% of the execution time of this program will
     * be spent. */
    for (i=0; i<length; i++) {
        c = buf[i];
        state = table[state][c];
        counts[state]++;
    }

    /* Save the ending state for the next chunk */
    *inout_state = state;

    /* Return the results */
    {
        struct results results;
        results.line_count = counts[NEWLINE];
        results.word_count = counts[NEWWORD];
        results.char_count = counts[NEWLINE] + counts[WASSPACE] + counts[WASWORD] + counts[NEWWORD];
        results.byte_count = length;

        return results;
    }
}

/**
 * Parse an individual file, or <stdin>, and print the results
 */
static struct results
parse_file(FILE *fp, const struct config *cfg)
{
    enum {BUFSIZE=65536};
    struct results results = {0, 0, 0, 0};
    unsigned state = 0; /* state held between chunks */
    unsigned char *buf;

    buf = malloc(BUFSIZE);
    if (buf == NULL)
        abort();

    /* Process a 64k chunk at a time */
    for (;;) {
        size_t count;
        struct results x;

        /* Read the next chunk of data from the file */
        count = fread(buf, 1, BUFSIZE, fp);
        if (count <= 0)
            break;

        /* Do the word-count algorithm */
        if (cfg->is_pointer_arithmetic > 1)
            x = parse_chunk_pp(buf, count, &state);
        else if (cfg->is_pointer_arithmetic)
            x = parse_chunk_p(buf, count, &state);
        else
            x = parse_chunk(buf, count, &state);

        /* Sum the results */
        results.line_count += x.line_count;
        results.word_count += x.word_count;
        results.byte_count += x.byte_count;
        results.char_count += x.char_count;
    }

    free(buf);
    return results;
}

/**
 * Calculate the width for the columns, so that when printing the
 * results from several files, all the columns will line up. The
 * width for all the columns is determined by the size of the files.
 */
static unsigned
get_column_width(int argc, char *argv[], int is_stdin)
{
    int i;
    off_t maxsize = 1;
    unsigned width = 0;

    for (i=1; i<argc; i++) {
        const char *filename = argv[i];
        struct stat st;

        if (filename[0] == '-')
            continue;

        if (stat(filename, &st) == 0) {
            if (S_ISREG(st.st_mode)) {
                if (maxsize <= st.st_size)
                    maxsize = st.st_size;
            } else if (maxsize <= 1000000)
                maxsize = 1000000;
        }
    }

    if (is_stdin) {
        if (maxsize <= 1000000)
            maxsize = 1000000;
    }

    while (maxsize) {
        width++;
        maxsize /= 10;
    }

    return width;
}

/**
 * Print a help message
 */
static void
print_help(void)
{
    printf("wc -- word, line, and byte or character count\n");
    printf("use:\n wc [-c|-m][-lw][file...]\n");
    printf("where:\n");
    printf(" -c\tPrint the number of bytes in each input file.\n");
    printf(" -l\tPrint the number of newlines in each input file.\n");
    printf(" -m\tPrint number of multibyte characters in each input file.\n");
    printf(" -w\tPrint the number of words in each input file.\n");
    printf("If no files specified, reads from stdin.\n");
    printf("If no options specified, -lwc will be used.\n");
}

/**
 * Parse the command-line options in order to get the configuration
 * for the program.
 */
static struct config
read_command_line(int argc, char *argv[])
{
    struct config cfg;
    int i;

    memset(&cfg, 0, sizeof(cfg));

    /* We set this as the errno so that 'perror()' will print a localized
     * error message, whatever "Invalid argument" is in the user's local
     * language */
    errno = EINVAL;

    for (i=1; i<argc; i++) {
        size_t j;
        size_t maxj;
        if (argv[i][0] != '-') {
            /* Assume anything not an -option is a filename */
            cfg.file_count++;
            continue;
        }
        if (argv[i][1] == '\0') {
            /* A bare dash '-' on the command-line means we should
             * also handle <stdin> */
            cfg.is_stdin = 1;
            continue;
        }
        if (argv[i][1] == '-') {
            if (argv[i][2] == '\0') {
                cfg.file_count += argc - i - 1;
                break;
            } else if (strcmp(argv[i], "--version") == 0) {
                fprintf(stderr, "--- wc-fast-ut8 1.0 by Robert Graham ---\n");
                exit(0);
            } else if (strcmp(argv[i], "--help") == 0) {
                print_help();
                exit(0);
            } else {
                perror(argv[i]);
                exit(1);
            }
        }

        maxj = strlen(argv[i]);
        for (j=1; j<maxj; j++) {
            char c = argv[i][j];
            const char *parm = NULL;
            switch (c) {
                case 'l': cfg.is_counting_lines++; break;
                case 'w': cfg.is_counting_words++; break;
                case 'c':
                    if (cfg.is_counting_chars) {
                        perror("-c");
                        exit(1);
                    }
                    cfg.is_counting_bytes++;
                    break;
                case 'm':
                    if (cfg.is_counting_bytes) {
                        perror("-m");
                        exit(1);
                    }
                    cfg.is_counting_chars++;
                    break;
                case 'W':
                    if (argv[i][j+1] == '\0') {
                        if (i+1 < argc)
                            parm = argv[++i];
                    } else {
                        parm = argv[i] + j + 1;
                    }
                    if (parm == NULL || !isdigit(*parm)) {
                        perror("-W");
                        exit(1);
                    } else
                        cfg.column_width = atoi(parm);
                    j = maxj;
                    break;
                case 'P':
                    cfg.is_pointer_arithmetic++;
                    break;
                default:
                    {
                        char foo[3];
                        foo[0] = '-';
                        foo[1] = c;
                        foo[2] = '\0';
                        perror(foo);
                        exit(1);
                    }
                    break;
            }
        }
    }

    /* If no files specified, then we do <stdin> instead */
    if (cfg.file_count == 0)
        cfg.is_stdin = 1;

    /* Default is -lwc if no options are given */
    if (cfg.is_counting_lines == 0
        && cfg.is_counting_words == 0
        && cfg.is_counting_bytes == 0
        && cfg.is_counting_chars == 0) {
        cfg.is_counting_lines = 1;
        cfg.is_counting_words = 1;
        cfg.is_counting_bytes = 1;
    }

    /* Calculate the width for the columns */
    if (cfg.column_width == 0) {
        if (cfg.file_count > 0) {
            cfg.column_width = get_column_width(argc, argv, cfg.is_stdin);
        } else
            cfg.column_width = 1;
    }

    /* If there are more than one files, or if there are both files and
     * <stdin>, then we need to print totals at the end. Otherwise, if only
     * a single result is to be printed, then printing an additional result
     * would be redundant */
    if (cfg.file_count > 1 || (cfg.file_count && cfg.is_stdin))
        cfg.is_printing_totals = 1;

    return cfg;
}

int main(int argc, char *argv[])
{
    int i;
    struct results totals = {0,0,0,0};
    struct config cfg;

    /* Force output to be an atomic line-at-a-time, so that other
     * programs reading the output never see a partial line */
#ifndef _WIN32
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif

    /* Read in the configuration parameters from the command-line */
    cfg = read_command_line(argc, argv);

    /* Compile the ASCII/UTF8 state-machine that we'll use to
     * parse multi-byte characters */
    compile_utf8_statemachine(cfg.is_counting_chars);
    compile_pointers();


    /* Process all the files specified on the command-line */
    for (i=1; i<argc; i++) {
        FILE *fp;
        const char *filename = argv[i];
        struct results results;

        if (argv[i][0] == '-')
            continue;

        fp = fopen(filename, "rb");
        if (fp == NULL) {
            perror(argv[i]);
            continue;
        }

        results = parse_file(fp, &cfg);
        print_results(filename, &results, &cfg);

        totals.line_count += results.line_count;
        totals.word_count += results.word_count;
        totals.byte_count += results.byte_count;
        totals.char_count += results.char_count;

        fclose(fp);
    }

    /* If no files specified, or the "-" file specified, then
     * we need to read in <stdin>. We need to change the mode
     * to "binary", to prevent the library from doing it's own
     * notions of text processing */
    if (cfg.is_stdin) {
        struct results results;
        FILE *fp;

        /* Make sure we read <stdin> in binary mode, because on some
         * platforms (Windows) it defaults to text-mode that will
         * chnage some characters */
        fp = freopen(NULL, "rb", stdin);
        if (fp == NULL) {
            perror("stdin");
            fp = stdin;
        }

        results = parse_file(fp, &cfg);
        print_results(NULL, &results, &cfg);

        totals.line_count += results.line_count;
        totals.word_count += results.word_count;
        totals.byte_count += results.byte_count;
        totals.char_count += results.char_count;
    }

    /* If we read more than one thing, then we also need to print an
     * additional totals line */
    if (cfg.is_printing_totals)
        print_results("total", &totals, &cfg);

#if _WIN32
    {
      FILETIME begin;
      FILETIME end;
      FILETIME kernel;
      FILETIME user;
        if (GetProcessTimes(GetCurrentProcess(), &begin, &end, &kernel, &user)) {
            unsigned long long elapsed = ((unsigned long long)user.dwLowDateTime | (unsigned long long)user.dwHighDateTime<<32ULL)/10000ULL;
            printf("user %lu.%03u\n", (unsigned)(elapsed/1000ULL), (unsigned)(elapsed % 1000));
        }
    }
#endif
    return 0;
}
