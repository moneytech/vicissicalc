#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// ANSI terminal control

#define ANSI "\x1b["

#define CLEAR_LINE_RIGHT ANSI "K"

static void aterm_clear_screen(void)    { printf(ANSI "2J" ANSI "H"); }
static void aterm_clear_to_bottom(void) { printf(ANSI "J"); }
static void aterm_home(void)            { printf(ANSI "H"); }
static void aterm_newline(void)         { printf(CLEAR_LINE_RIGHT "\r\n"); }

static void aterm_reset(void)           { printf("\x1b" "c"); fflush(stdout); }

static void aterm_set_foreground(unsigned color) {
    printf(ANSI "%um", 30 + color);
}
static void aterm_set_background(unsigned color) {
    printf(ANSI "%um", 40 + color);
}

// Colors. This is a macro for the sake of use in constant expressions:
#define aterm_bright(color)   (60 + (color))  
enum {
    aterm_black = 0,
    aterm_red,
    aterm_green,
    aterm_yellow,
    aterm_blue,
    aterm_magenta,
    aterm_cyan,
    aterm_white
};


 // Utilities

static void panic(const char *plaint) {
    system("stty sane"); aterm_reset();
    fprintf(stderr, "%s\n", plaint);
    exit(1);
}

// Copy into dest as much of `s` as will fit.
// strncpy won't do because it can leave dest unterminated.
static void stuff(char *dest, size_t dest_size, const char *s) {
    assert(0 < dest_size);
    size_t i;
    for (i = 0; i < dest_size-1; ++i)
        if ((dest[i] = s[i]) == 0) return;
    dest[i] = 0;
}

// Really strdup, but that name may be taken.
static char *dupe(const char *s) {
    char *result = malloc(strlen(s) + 1);
    if (!result) panic("Out of memory");
    strcpy(result, s);
    return result;
}

static int is_blank(const char *s) {
    const char *t = s + strspn(s, " \t");
    return *t == '\0';
}

static const char *orelse(const char *s1, const char *s2) {
    return s1 ? s1 : s2;
}

static int min(int x, int y) { return x < y ? x : y; }
static int max(int x, int y) { return x > y ? x : y; }


// Evaluating expressions

typedef double Value;

static const char *get_value(Value *value, unsigned row, unsigned column,
                             const char *derived_plaint);

typedef struct Context Context;
struct Context {
    const char *p;
    unsigned row;
    unsigned col;
    int token;
    Value token_value;
    const char *plaint;
};

static void complain(Context *s, const char *plaint) {
    if (s->plaint == NULL) {
        s->plaint = plaint;
        s->p += strlen(s->p);
    }
}

static void next(Context *s) {
    while (isspace(*s->p))
        s->p++;
    if (*s->p == '\0')
        s->token = '\0';
    else if (isdigit(*s->p)) {
        char *endptr;
        s->token = '0';
        s->token_value = strtod(s->p, &endptr);
        s->p = endptr;
    }
    else if (strchr("+-*/%^@cr()", *s->p))
        s->token = *s->p++;
    else {
        complain(s, "Syntax error: unknown token type");
        s->token = '\0';
    }
}

static Value parse_expr(Context *s, int precedence);

static Value parse_factor(Context *s) {
    Value v = s->token_value;
    switch (s->token) {
        case '0': next(s); return v;
        case '-': next(s); return -parse_factor(s);
        case 'c': next(s); return s->col;
        case 'r': next(s); return s->row;
        case '(':
            next(s); 
            v = parse_expr(s, 0);
            if (s->token != ')')
                complain(s, "Syntax error: expected ')'");
            next(s);
            return v;
        default:
            complain(s, "Syntax error: expected a factor");
            next(s);
            return 0;
    }
}

static Value zero_divide(Context *s) {
    complain(s, "Divide by 0");
    return 0;
}

static Value apply(Context *s, char rator, Value lhs, Value rhs) {
    switch (rator) {
        case '+': return lhs + rhs;
        case '-': return lhs - rhs;
        case '*': return lhs * rhs;
        case '/': return rhs == 0 ? zero_divide(s) : lhs / rhs;
        case '%': return rhs == 0 ? zero_divide(s) : fmod(lhs, rhs);
        case '^': return pow(lhs, rhs); // XXX report domain errors
        case '@': {
            Value value = 0;
            unsigned r = lhs, c = rhs;
            const char *plaint = get_value(&value, r, c, "");
            if (plaint)
                complain(s, plaint);
            return value;
        }
        default: assert(0); return 0;
    }
}

static Value parse_expr(Context *s, int precedence) {
    Value lhs = parse_factor(s);
    for (;;) {
        int lp, rp, rator = s->token;  // left/right precedence and operator
        switch (rator) {
            case '+': lp = 1; rp = 2; break;
            case '-': lp = 1; rp = 2; break;
            case '*': lp = 3; rp = 4; break;
            case '/': lp = 3; rp = 4; break;
            case '%': lp = 3; rp = 4; break;
            case '^': lp = 5; rp = 5; break;
            case '@': lp = 7; rp = 8; break;
            default: return lhs;
        }
        if (lp < precedence)
            return lhs;
        next(s);
        lhs = apply(s, rator, lhs, parse_expr(s, rp));
    }
}

// A formula, if it's given, follows the '=' prefix.
static const char *find_formula(const char *s) {
    const char *t = s + strspn(s, " \t");
    return *t == '=' ? t + 1 : NULL;
}

static const char *evaluate(Value *result, 
                            const char *expression, unsigned r, unsigned c) {
    Context context;
    context.plaint = NULL;
    context.p = find_formula(expression);
    if (!context.p)
        return "No formula";
    context.row = r;
    context.col = c;
    next(&context);
    *result = parse_expr(&context, 0);
    if (context.token != '\0')
        complain(&context, "Syntax error: unexpected token");
    return context.plaint;
}


// The array of spreadsheet cells

static const char *the_plaint = NULL;

static void error(const char *plaint) {
    if (!the_plaint)
        the_plaint = plaint;
}

typedef struct Cell Cell;
struct Cell {
    char *text;                 // malloced
    const char *plaint;         // in static memory
    Value value;
};

// These states of the plaint field have special meaning -- see update():
static const char unknown[]     = "Unknown";
static const char calculating[] = "Circular reference";
#define valid                     NULL

enum { nrows = 20, ncols = 4 };
static Cell cells[nrows][ncols];

static void setup(void) {
    for (unsigned r = 0; r < nrows; ++r)
        for (unsigned c = 0; c < ncols; ++c) {
            cells[r][c].text = dupe("");
            cells[r][c].plaint = unknown;
        }
}

static void set_text(unsigned row, unsigned col, const char *text) {
    assert(row < nrows && col < ncols);
    if (cells[row][col].text == text) return;
    free(cells[row][col].text);
    cells[row][col].text = dupe(text);
    for (unsigned r = 0; r < nrows; ++r)
        for (unsigned c = 0; c < ncols; ++c)
            cells[r][c].plaint = unknown;
}

static void update(unsigned r, unsigned c) {
    assert(r < nrows && c < ncols);
    Cell *cell = &cells[r][c];
    cell->plaint = calculating;
    cell->plaint = evaluate(&cell->value, cell->text, r, c);
    if (cell->plaint)
        error(cell->plaint);
}

// Set *value to the value of the cell at(r,c), unless there's an
// error; in which case return either the error's plaint or
// derived_plaint -- the latter to keep from propagating a plaint
// between cells -- we want to propagate only the fact of the error,
// not the plaint itself.
static const char *get_value(Value *value, unsigned r, unsigned c,
                             const char *derived_plaint) {
    if (nrows <= r || ncols <= c)
        return "Cell out of range";
    Cell *cell = &cells[r][c];
    if (cell->plaint == unknown)
        update(r, c);
    if (cell->plaint == calculating)
        return calculating;
    if (cell->plaint)
        return orelse(derived_plaint, cell->plaint);
    *value = cell->value;
    return NULL;
}


// File loading/saving

static FILE *open_file(const char *filename, const char *mode) {
    FILE *file = fopen(filename, mode);
    if (!file)
        error(strerror(errno));
    return file;
}

static const char *filename = NULL;

static void write_file(void) {
    assert(filename);
    FILE *file = open_file(filename, "w");
    if (!file) return;
    for (unsigned r = 0; r < nrows; ++r)
        for (unsigned c = 0; c < ncols; ++c) {
            const char *text = cells[r][c].text;
            if (!is_blank(text))
                fprintf(file, "%u %u %s\n", r, c, text);
        }
    fclose(file);
}

static void read_file(void) {
    assert(filename);
    FILE *file = fopen(filename, "r");
    if (!file) return;  // XXX complain
    char line[1024];
    while (fgets(line, sizeof line, file)) {
        unsigned r, c;
        char text[sizeof line];
        if (3 != sscanf(line, "%u %u %[^\n]", &r, &c, text))
            error("Bad line in file");
        else if (nrows <= r || ncols <= c)
            error("Row or column number out of range in file");
        else
            set_text(r, c, text);
    }    
    fclose(file);
}


// UI display

enum { colwidth = 18 };

typedef struct Colors Colors;
struct Colors {
    unsigned fg, bg;
};
static void set_color(Colors colors) {
    aterm_set_background(colors.bg);
    aterm_set_foreground(colors.fg);
}

typedef struct Style Style;
struct Style {
    Colors unhighlighted, highlighted;
};
static Style ok_style = {
    .unhighlighted = { .fg = aterm_black,
                       .bg = aterm_white },
    .highlighted   = { .fg = aterm_bright(aterm_white),
                       .bg = aterm_bright(aterm_blue) }
};
static Style error_style = {
    .unhighlighted = { .fg = aterm_black,
                       .bg = aterm_bright(aterm_cyan) },
    .highlighted   = { .fg = aterm_bright(aterm_white), 
                       .bg = aterm_bright(aterm_red) }
};
static Colors border_colors = { .fg = aterm_blue,
                                .bg = aterm_bright(aterm_yellow) };

typedef enum { formulas, values } View;

static void show_at(unsigned r, unsigned c, View view, int highlighted) {
    char text[1024];
    const Style *style = &ok_style;
    const char *formula = find_formula(cells[r][c].text);
    if (view == formulas || !formula)
        stuff(text, sizeof text, orelse(formula, cells[r][c].text));
    else {
        Value value;
        const char *plaint = get_value(&value, r, c, NULL);
        if (plaint) {
            style = &error_style;
            stuff(text, sizeof text, plaint);
        }
        else
            snprintf(text, sizeof text, "%*g", colwidth, value);
    }
    if (colwidth < strlen(text))
        strcpy(text + colwidth - 3, "...");
    set_color(highlighted ? style->highlighted : style->unhighlighted);
    printf(" %*s", colwidth, text);
}

static void show(View view, unsigned cursor_row, unsigned cursor_col) {
    aterm_home();
    set_color(ok_style.unhighlighted);
    printf("%-79.79s", cells[cursor_row][cursor_col].text);
    aterm_newline();
    set_color(border_colors);
    printf("%s%*u",
           view == formulas ? "(formulas)" : "          ",
           (int) (colwidth - sizeof "(formulas)" + 4), 0);
    for (unsigned c = 1; c < ncols; ++c)
        printf(" %*u", colwidth, c);
    aterm_newline();
    for (unsigned r = 0; r < nrows; ++r) {
        set_color(border_colors);
        printf("%2u", r);
        for (unsigned c = 0; c < ncols; ++c)
            show_at(r, c, view, r == cursor_row && c == cursor_col);
        aterm_newline();
    }
    const char *cell_plaint = cells[cursor_row][cursor_col].plaint;
    if (cell_plaint == unknown) cell_plaint = NULL;
    printf("%-80.80s", orelse(the_plaint, orelse(cell_plaint, "")));
    the_plaint = NULL;
    aterm_clear_to_bottom();
}


// Keyboard input

enum {
    esc = 27,
    key_up = 1024,   // just making up our own codes for non-ASCII keys
    key_down,
    key_left,
    key_right,
    key_unknown,
    key_shift = 1<<0,   // Key-chord modifier bits for non-ASCII keys
    key_alt   = 1<<1,
    key_ctrl  = 1<<2,
};

static int weirdo(int last_key) {
    return last_key == EOF ? EOF : key_unknown;
}

static int chord(int m1, int n1, int key) {
    if (!(1 <= m1 && m1 <= 8 && 1 <= n1 && n1 <= 8))
        return weirdo(key);
    int m_bits = m1-1, n_bits = n1-1;
    if (m_bits != 0) return weirdo(key);
    if (n_bits != 0)
        assert(1024 <= key); // (modifier codes only go with non-ASCII keys)
    // TODO for the Home key this would need adjustment:
    return key | n_bits;
}

static int get_key(void) {
    int k0 = getchar();
    if (k0 != esc) return k0;
    int k1 = getchar();
    if (k1 != '[') return weirdo(k1);
    // This ought to be a sequence like
    //   esc, '[', optional(digit, optional(';', digit)), character.
    // Call the digits `m1` and `n1`; they default to 1.
    int m1 = 1, n1 = 1;
    int k = getchar();
    if (isdigit(k)) {
        m1 = k - '0';
        k = getchar();
        if (k == ';') {
            k = getchar();
            if (!isdigit(k)) return weirdo(k);
            n1 = k - '0';
            k = getchar();
        }
    }
    // Now k is the last character of the above sequence.
    switch (k) {
    case 'A': return chord(m1, n1, key_up);
    case 'B': return chord(m1, n1, key_down);
    case 'C': return chord(m1, n1, key_right);
    case 'D': return chord(m1, n1, key_left);
    default:  return weirdo(k);
    }
}


// Interaction and main program

static View view = values;
static int row = 0;
static int col = 0;

static void refresh(void) {
    show(view, row, col);
}

static char input[81];

// Return true iff the user commits a change.
static int edit_loop(void) {
    size_t p = strlen(input);
    for (;;) {
        printf("\r" CLEAR_LINE_RIGHT "? %s", input); fflush(stdout);
        int k = get_key();
        if (k == '\r' || k == EOF)
            return 1;
        else if (k == 7) // C-g
            return 0;
        else if (k == '\b' || k == 127) { // backspace
            if (0 < p)
                input[--p] = '\0';
        }
        else if (isprint(k) && p+1 < sizeof input) {
            input[p++] = k;
            input[p] = '\0';
            putchar(k); fflush(stdout);
        }
    }
}

static void enter_text(void) {
    stuff(input, sizeof input, cells[row][col].text);
    if (edit_loop())
        set_text(row, col, input);
    else
        error("Aborted");
}

static void copy_text(unsigned r, unsigned c) {
    set_text(r, c, cells[row][col].text);
    row = r;
    col = c;
}

static void reactor_loop(void) {
    for (;;) {
        refresh();
        int k = get_key();
        switch (k) {

        case 'q': return;

        case ' ': enter_text(); break;

        case 'w': write_file(); break;

        case 'f': view = (view == formulas ? values : formulas); break;

        case key_left:  col = max(col-1, 0);       break;
        case key_right: col = min(col+1, ncols-1); break;
        case key_down:  row = min(row+1, nrows-1); break;
        case key_up:    row = max(row-1, 0);       break;

        case key_ctrl|key_left:  copy_text(row,         max(col-1, 0));
                                 break;
        case key_ctrl|key_right: copy_text(row,         min(col+1, ncols-1));
                                 break;
        case key_ctrl|key_down:  copy_text(min(row+1, nrows-1), col);
                                 break;
        case key_ctrl|key_up:    copy_text(max(row-1, 0),       col);
                                 break;

        default:  error("Unknown key");
        }
    }
}

int main(int argc, char **argv) {
    if (2 < argc)
        panic("usage: vicissicalc [filename]");
    setup();
    if (argc == 2) {
        filename = argv[1];
        read_file();
    }
    system("stty raw -echo");
    aterm_clear_screen();
    reactor_loop();
    system("stty sane"); aterm_reset();
    return 0;
}
