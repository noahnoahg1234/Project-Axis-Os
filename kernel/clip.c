/*
 * AxisOS - system clipboard.
 *
 * A single shared text buffer used for copy/cut/paste across the OS: the text
 * editor, the shell line editor, input prompts (so you can paste an API key
 * into Settings), and the `clip` / `paste` commands.
 */
#include "axis.h"

static char *clip_buf;
static uint32_t clip_n;

void clip_set(const char *s, uint32_t n) {
    char *nb = kmalloc(n + 1);
    if (!nb) return;
    memcpy(nb, s, n);
    nb[n] = 0;
    kfree(clip_buf);
    clip_buf = nb;
    clip_n = n;
}
void clip_set_str(const char *s) { clip_set(s, (uint32_t)strlen(s)); }
const char *clip_text(void) { return clip_buf ? clip_buf : ""; }
uint32_t clip_len(void) { return clip_n; }
bool clip_has(void) { return clip_n > 0; }
