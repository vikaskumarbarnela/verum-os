/* The ledger on disk, plus the attic.
 *
 * Layout of the raw image (primary ATA master, 8 MiB = 16384 sectors):
 *   sector 0                 header: magic "VERUMLD2", ledger bytes/entries, attic bytes/branches
 *   sectors 1      .. 8191   the ledger: one byte stream of event lines, "\n"-terminated
 *   sectors 8192   .. 16383  the attic: branches cut off by `rewind`, same line format,
 *                            each branch introduced by a "branch|from|count|uptime" line
 *
 * Appending to either stream rewrites only the last partial sector, any new
 * sectors and the header. Truncating the ledger (rewind) rewrites the header;
 * the discarded lines have already been copied to the attic, so nothing is
 * ever lost. */
#include "verum.h"

#define MAGIC "VERUMLD2"
#define MAGIC_V1 "VERUMLD1"
#define SEC 512u
#define LEDGER_FIRST 1u
#define LEDGER_SECTORS 8191u
#define ATTIC_FIRST 8192u
#define ATTIC_SECTORS 8192u

typedef struct { u32 first, sectors; u32 bytes, count; } Stream;

static bool present = false;
static Stream ledger = { LEDGER_FIRST, LEDGER_SECTORS, 0, 0 };
static Stream attic  = { ATTIC_FIRST, ATTIC_SECTORS, 0, 0 };
static u8 sector[SEC];

static bool write_header(void) {
    memset(sector, 0, SEC);
    memcpy(sector, MAGIC, 8);
    memcpy(sector + 8, &ledger.bytes, 4);
    memcpy(sector + 12, &ledger.count, 4);
    memcpy(sector + 16, &attic.bytes, 4);
    memcpy(sector + 20, &attic.count, 4);
    return ata_write(0, sector, 1);
}

bool disk_init(void) {
    present = ata_present();
    if (!present) return false;
    if (!ata_read(0, sector, 1)) { present = false; return false; }
    if (memcmp(sector, MAGIC_V1, 8) == 0) {              /* older image: same ledger stream, empty attic */
        memcpy(&ledger.bytes, sector + 8, 4);
        memcpy(&ledger.count, sector + 12, 4);
        attic.bytes = attic.count = 0;
        return write_header();
    }
    if (memcmp(sector, MAGIC, 8) != 0) {                 /* blank image: format it */
        ledger.bytes = ledger.count = attic.bytes = attic.count = 0;
        if (!write_header()) { present = false; return false; }
        return true;
    }
    memcpy(&ledger.bytes, sector + 8, 4);
    memcpy(&ledger.count, sector + 12, 4);
    memcpy(&attic.bytes, sector + 16, 4);
    memcpy(&attic.count, sector + 20, 4);
    return true;
}

bool disk_present(void) { return present; }
u32 disk_entries(void) { return ledger.count; }
u32 disk_bytes(void) { return ledger.bytes; }
u32 disk_attic_bytes(void) { return attic.bytes; }
u32 disk_attic_branches(void) { return attic.count; }

static char *stream_read_all(Stream *s) {
    if (!present) return 0;
    u32 nsec = (s->bytes + SEC - 1) / SEC;
    char *buf = kalloc(nsec * SEC + 1);
    if (nsec && !ata_read(s->first, (u8 *)buf, nsec)) return 0;
    buf[s->bytes] = 0;
    return buf;
}

static bool stream_append(Stream *s, const char *line, bool count_it) {
    if (!present) return false;
    usize len = strlen(line);
    if ((s->bytes + len + 1 + SEC - 1) / SEC > s->sectors) return false;   /* region full */
    u32 off = s->bytes % SEC, lba = s->first + s->bytes / SEC;
    const char *p = line; usize left = len + 1;                            /* +1: the newline */
    while (left) {
        if (off) { if (!ata_read(lba, sector, 1)) return false; }
        else memset(sector, 0, SEC);
        u32 room = SEC - off;
        u32 take = left < room ? (u32)left : room;
        for (u32 i = 0; i < take; i++) sector[off + i] = (left - i == 1) ? (u8)'\n' : (u8)p[i];
        if (!ata_write(lba, sector, 1)) return false;
        p += take; left -= take; off = 0; lba++;
    }
    s->bytes += (u32)len + 1;
    if (count_it) s->count++;
    return write_header();
}

char *disk_read_all(void) { return stream_read_all(&ledger); }
bool disk_append(const char *line) { return stream_append(&ledger, line, true); }

bool disk_truncate(u32 bytes, u32 entries) {
    if (!present) return false;
    ledger.bytes = bytes; ledger.count = entries;
    return write_header();
}

char *disk_attic_read_all(void) { return stream_read_all(&attic); }
/* A branch header line counts as a branch; its lines do not. */
bool disk_attic_append(const char *line, bool is_branch_header) { return stream_append(&attic, line, is_branch_header); }
