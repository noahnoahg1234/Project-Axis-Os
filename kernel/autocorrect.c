/*
 * AxisOS - autocorrect.
 *
 * Case-insensitive Damerau-Levenshtein distance measured in half-edits:
 * insert / delete / substitute cost 2, swapping two neighbours ("sl" for
 * "ls") costs 1, because swapped letters are the most common typing slip.
 */
#include "axis.h"

int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40) return 100;
    static int d[42][42];
    for (int i = 0; i <= la; i++) d[i][0] = 2 * i;
    for (int j = 0; j <= lb; j++) d[0][j] = 2 * j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = tolower(a[i - 1]) == tolower(b[j - 1]) ? 0 : 2;
            int v = d[i - 1][j] + 2;
            if (d[i][j - 1] + 2 < v) v = d[i][j - 1] + 2;
            if (d[i - 1][j - 1] + cost < v) v = d[i - 1][j - 1] + cost;
            /* transposition: "sl" -> "ls" */
            if (i > 1 && j > 1 && tolower(a[i - 1]) == tolower(b[j - 2]) && tolower(a[i - 2]) == tolower(b[j - 1]) &&
                d[i - 2][j - 2] + 1 < v)
                v = d[i - 2][j - 2] + 1;
            d[i][j] = v;
        }
    }
    return d[la][lb];
}

/* how many edits we accept before a word is considered something else */
int ac_threshold(const char *w) {
    size_t l = strlen(w);
    if (l <= 3) return 2; /* one edit */
    if (l <= 6) return 4; /* two edits */
    return 6;             /* three edits */
}
