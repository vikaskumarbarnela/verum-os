/* लिपि - Harvard-Kyoto transliteration on bare metal, so Sanskrit can be
   typed on a Latin keyboard: paribhASA -> परिभाषा, saMskRta -> संस्कृत.
   Same scheme as verum/translit.py (aa ii uu sh Sh chh accepted). A consonant
   not followed by a vowel gets a virama. Output is UTF-8. */
#include "lib.h"

typedef struct { const char *key; const char *indep; const char *sign; } Vowel;
typedef struct { const char *key; const char *out; } Pair;

static const Vowel VOWELS[] = {
    {"RRi", "ऋ", "ृ"}, {"RR", "ॠ", "ॄ"}, {"lR", "ऌ", "ॢ"}, {"aa", "आ", "ा"}, {"ii", "ई", "ी"}, {"uu", "ऊ", "ू"},
    {"ai", "ऐ", "ै"}, {"au", "औ", "ौ"}, {"A", "आ", "ा"}, {"I", "ई", "ी"}, {"U", "ऊ", "ू"}, {"R", "ऋ", "ृ"},
    {"a", "अ", ""}, {"i", "इ", "ि"}, {"u", "उ", "ु"}, {"e", "ए", "े"}, {"o", "ओ", "ो"},
};
static const Pair CONS[] = {
    {"chh", "छ"}, {"kSh", "क्ष"}, {"kS", "क्ष"}, {"jJ", "ज्ञ"},
    {"kh", "ख"}, {"gh", "घ"}, {"ch", "छ"}, {"jh", "झ"}, {"Th", "ठ"}, {"Dh", "ढ"}, {"th", "थ"}, {"dh", "ध"},
    {"ph", "फ"}, {"bh", "भ"}, {"sh", "श"}, {"Sh", "ष"},
    {"k", "क"}, {"g", "ग"}, {"G", "ङ"}, {"c", "च"}, {"j", "ज"}, {"J", "ञ"}, {"T", "ट"}, {"D", "ड"}, {"N", "ण"},
    {"t", "त"}, {"d", "द"}, {"n", "न"}, {"p", "प"}, {"b", "ब"}, {"m", "म"}, {"y", "य"}, {"r", "र"}, {"l", "ल"},
    {"v", "व"}, {"w", "व"}, {"z", "श"}, {"S", "ष"}, {"s", "स"}, {"h", "ह"}, {"L", "ळ"},
};
static const Pair MARKS[] = { {"||", "॥"}, {"M", "ं"}, {"H", "ः"}, {"~", "ँ"}, {"'", "ऽ"}, {"|", "।"} };
static const char *const DIGITS[10] = { "०", "१", "२", "३", "४", "५", "६", "७", "८", "९" };
static const char VIRAMA[] = "्";

static bool starts(const char *s, const char *k) { return strncmp(s, k, strlen(k)) == 0; }

/* Transliterate `in` into `out` (capacity `cap`). Returns the output length. */
usize lipi_word(const char *in, char *out, usize cap) {
    usize n = 0; bool pending = false;           /* a consonant awaiting its vowel */
#define PUT(s) do { usize l = strlen(s); if (n + l + 1 < cap) { memcpy(out + n, s, l); n += l; } } while (0)
    while (*in) {
        const Vowel *v = 0; const Pair *c = 0; const Pair *m = 0; usize klen = 0;
        for (usize i = 0; i < sizeof CONS / sizeof CONS[0]; i++) if (starts(in, CONS[i].key)) { c = &CONS[i]; klen = strlen(c->key); break; }
        for (usize i = 0; i < sizeof VOWELS / sizeof VOWELS[0]; i++) if (starts(in, VOWELS[i].key) && strlen(VOWELS[i].key) > klen) { v = &VOWELS[i]; c = 0; klen = strlen(v->key); break; }
        for (usize i = 0; i < sizeof MARKS / sizeof MARKS[0]; i++) if (starts(in, MARKS[i].key) && strlen(MARKS[i].key) > klen) { m = &MARKS[i]; v = 0; c = 0; klen = strlen(m->key); break; }
        if (c) { if (pending) PUT(VIRAMA); PUT(c->out); pending = true; }
        else if (v) { if (pending) { PUT(v->sign); pending = false; } else PUT(v->indep); }
        else if (m) { if (pending) { PUT(VIRAMA); pending = false; } PUT(m->out); }
        else {
            if (pending) { PUT(VIRAMA); pending = false; }
            if (*in >= '0' && *in <= '9') PUT(DIGITS[*in - '0']);
            else { char one[2] = { *in, 0 }; PUT(one); }
            klen = 1;
        }
        in += klen;
    }
    if (pending) PUT(VIRAMA);
    out[n] = 0;
    return n;
#undef PUT
}

/* Transliterate a shell line in place (except the first word, and words `keep` says are English).
   `:word` forces transliteration, `\word` keeps Latin, quoted strings are transliterated. */
void lipi_line(char *line, usize cap, bool (*keep)(const char *word)) {
    char *out = kalloc(cap); usize n = 0;
    const char *p = line;
    while (*p == ' ') { if (n + 1 < cap) out[n++] = *p; p++; }
    while (*p && *p != ' ') { if (n + 1 < cap) out[n++] = *p; p++; }       /* the intent stays */
    while (*p) {
        if (*p == '"') {
            const char *e = p + 1; while (*e && *e != '"') e++;
            char word[128]; usize l = (usize)(e - p - 1); if (l >= sizeof word) l = sizeof word - 1;
            memcpy(word, p + 1, l); word[l] = 0;
            char dev[512]; lipi_word(word, dev, sizeof dev);
            if (n + 1 < cap) out[n++] = '"';
            for (const char *q = dev; *q && n + 1 < cap; q++) out[n++] = *q;
            if (n + 1 < cap) out[n++] = '"';
            p = *e ? e + 1 : e;
            continue;
        }
        if (*p == ' ' || *p == '(' || *p == ')') { if (n + 1 < cap) out[n++] = *p; p++; continue; }
        const char *e = p; while (*e && *e != ' ' && *e != '(' && *e != ')' && *e != '"') e++;
        char word[128]; usize l = (usize)(e - p); if (l >= sizeof word) l = sizeof word - 1;
        memcpy(word, p, l); word[l] = 0;
        bool alpha = true; for (const char *q = word; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '\'' || *q == '~' || *q == '?' || (*q >= '0' && *q <= '9'))) alpha = false;
        char dev[512];
        if (word[0] == ':') lipi_word(word + 1, dev, sizeof dev);
        else if (word[0] == '\\') strcpy(dev, word + 1);
        else if (!alpha || (keep && keep(word))) strcpy(dev, word);
        else lipi_word(word, dev, sizeof dev);
        for (const char *q = dev; *q && n + 1 < cap; q++) out[n++] = *q;
        p = e;
    }
    out[n] = 0;
    strcpy(line, out);
}
