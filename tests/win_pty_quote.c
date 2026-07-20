/* designs/pty/ 03 §6 / risk W4 — the argv -> command-line quoting unit table.
 *
 * Windows takes ONE command-line string, so the pty floor joins argv with the
 * documented CommandLineToArgvW-inverse rules (§2). A quoting bug there is
 * silent: the child simply receives different arguments. This driver includes
 * the floor itself (lv_win_build_cmdline is static — deliberately, it is not
 * part of the lv_plat.h surface) and pins the table.
 *
 * It needs neither ConPTY nor a console, so it runs anywhere the MinGW .exe
 * runs — including wine, where the behavioral cases must skip. Built and run
 * by tests/run_pty_win.sh.
 */
#include "../runtime/lv_plat_win32.c"

#include <stdio.h>

static int failures = 0;

static void expect(const char* label, char* const argv[], const char* want) {
    char* got = lv_win_build_cmdline(argv);
    if (!got || strcmp(got, want) != 0) {
        printf("FAIL %s: want [%s] got [%s]\n", label, want, got ? got : "(null)");
        failures++;
    } else {
        printf("ok   %s: %s\n", label, got);
    }
    free(got);
}

int main(void) {
    char* plain[]   = { (char*)"cmd.exe", (char*)"/c", (char*)"echo", NULL };
    char* spaced[]  = { (char*)"C:\\Program Files\\a.exe", (char*)"one two", NULL };
    char* quoted[]  = { (char*)"a.exe", (char*)"say \"hi\"", NULL };
    char* slashes[] = { (char*)"a.exe", (char*)"C:\\dir\\", NULL };
    char* slashq[]  = { (char*)"a.exe", (char*)"a\\\\\"b", NULL };
    char* empty[]   = { (char*)"a.exe", (char*)"", (char*)"z", NULL };
    char* tabbed[]  = { (char*)"a.exe", (char*)"a\tb", NULL };
    char* only[]    = { (char*)"a.exe", NULL };

    /* No whitespace, no quotes: verbatim, never gratuitously quoted. */
    expect("plain", plain, "cmd.exe /c echo");
    /* Whitespace anywhere in an argument quotes THAT argument only. */
    expect("spaced", spaced, "\"C:\\Program Files\\a.exe\" \"one two\"");
    /* An embedded quote is backslash-escaped inside the quotes. */
    expect("quoted", quoted, "a.exe \"say \\\"hi\\\"\"");
    /* A trailing backslash run before the CLOSING quote doubles, or the quote
     * would be escaped away and the argument would swallow the next one. */
    expect("trailing-slashes", slashes, "a.exe C:\\dir\\");
    /* Backslashes preceding an embedded quote double, then escape the quote;
     * backslashes elsewhere stay literal. */
    expect("slash-then-quote", slashq, "a.exe \"a\\\\\\\\\\\"b\"");
    /* The empty argument MUST be quoted or it vanishes entirely. */
    expect("empty-arg", empty, "a.exe \"\" z");
    /* Tab counts as whitespace to CommandLineToArgvW, like space. */
    expect("tab", tabbed, "a.exe \"a\tb\"");
    /* argv[0] alone: no trailing separator. */
    expect("only-argv0", only, "a.exe");

    printf(failures ? "quoting table: %d FAILURE(S)\n" : "quoting table: clean (%d)\n",
           failures);
    return failures ? 1 : 0;
}
