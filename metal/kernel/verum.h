/* VERUM kernel on bare metal: store, ledger, capabilities, seats, cells, disk. */
#ifndef VERUM_H
#define VERUM_H
#include "vs.h"

#define MAX_OBJS    512
#define MAX_CAPS    256
#define MAX_CELLS   12
#define MAX_SEATS   8
#define MAX_LEDGER  2048
#define MAX_TAGS    6
#define MAX_INPUTS  4
#define INBOX       16
#define NAME_LEN    16
#define DEFAULT_FUEL 2000000
#define SLICE_TICKS  5                /* 50 ms of CPU per message, enforced by the timer */
#define SHELL "shell"

/* ---------------------------------------------------------------- store */
typedef struct {
    char hash[65];
    const char *kind, *by, *op;
    Val *data;
    const char *tags[MAX_TAGS]; int ntags;
    char inputs[MAX_INPUTS][65]; int ninputs;
    u32 tick;
} Obj;

int   store_put(const char *kind, Val *data, const char **tags, int ntags, const char *by,
                const char *op, const char (*inputs)[65], int ninputs, u32 tick);
Obj  *store_get(int idx);
int   store_resolve(const char *prefix);
int   store_find(const char *tag, int *out, int cap);
int   store_count(void);
int   store_find_code(const char *name);
void  store_reset(void);

/* ---------------------------------------------------------------- ledger */
typedef struct { u32 seq; char prev[65]; char hash[65]; const char *text; u32 bytes; } Entry;
int   ledger_append(const char *text);
bool  ledger_verify(char *why, usize cap);
int   ledger_count(void);
Entry *ledger_entry(int i);
const char *ledger_head(void);
void  ledger_reset(void);
void  ledger_set_loading(bool loading);

/* ---------------------------------------------------------------- disk (disk.c) */
bool  disk_init(void);
bool  disk_present(void);
u32   disk_entries(void);
u32   disk_bytes(void);
char *disk_read_all(void);
bool  disk_append(const char *line);
bool  disk_truncate(u32 bytes, u32 entries);
u32   disk_attic_bytes(void);
u32   disk_attic_branches(void);
char *disk_attic_read_all(void);
bool  disk_attic_append(const char *line, bool is_branch_header);

/* ---------------------------------------------------------------- capabilities */
enum { K_STORE, K_CONSOLE, K_CLOCK, K_CELL, K_KERNEL, K_COUNT };
enum { R_GET = 1, R_PUT = 2, R_FIND = 4, R_LINEAGE = 8, R_PRINT = 16, R_NOW = 32, R_SEND = 64,
       R_SPAWN = 128, R_MINT = 256, R_REVOKE = 512, R_CELLS = 1024,
       R_DELEGATE = 2048 };          /* valid on every kind: holder may pass a narrower copy to a linked cell */

typedef struct {
    char token[33];
    int kind; u32 rights;
    char holder[NAME_LEN]; char target[NAME_LEN];
    int parent; bool revoked; bool used;
} Cap;

int   cap_mint(int kind, u32 rights, const char *holder, const char *target, int parent, const char *token);
int   cap_attenuate(int parent, const char *by, u32 rights, const char *holder, const char *token, const char **err);
int   cap_revoke(int idx, const char *by, const char **err);
Cap  *cap_check(const char *token, const char *holder, u32 right, const char **err);
Cap  *cap_by_token(const char *token);
Cap  *cap_at(int idx);
int   cap_index(Cap *c);
int   cap_count(void);
const char *cap_kind_name(int kind);
u32   cap_rights_of_kind(int kind);
u32   cap_parse_rights(const char *csv, int kind);
void  cap_rights_csv(u32 rights, char *out);
void  cap_describe(Cap *c, char *out, usize cap);
void  caps_reset(void);

/* ---------------------------------------------------------------- seats: the humans */
typedef struct { char name[NAME_LEN]; bool used; int caps[K_COUNT]; } Seat;   /* caps[kind] = index or -1 */

Seat *seat_find(const char *name);
Seat *seat_create(const char *name, const char **err);   /* records; needs the active seat's kernel[mint] */
int   seat_count(Seat **out, int cap);
extern const char *k_me;                                 /* the active seat's name; the shell's "whoami" */
int   my_cap(int kind);                                  /* the active seat's cap index of that kind, or -1 */
bool  i_may(int kind, u32 right);                        /* active seat holds that right? */

/* ---------------------------------------------------------------- kernel */
typedef struct {
    char name[NAME_LEN]; bool alive;
    int code;
    Env *env;
    struct { char from[NAME_LEN]; Val *body; } inbox[INBOX]; int in_head, in_count;
    u32 fuel, runs, errors, fuel_used;
    /* scheduling: a cell with work gets its own kernel stack and can be paused between VM steps */
    u32 *sp; u8 *stack; bool running; bool mid_run; u32 slices;
    u8 *arena; usize arena_used; Interp *it;          /* per-run memory, reset each message; the live interpreter */
} Cell;
#define TASK_STACK (512u * 1024u)
#define ARENA_SIZE (8u * 1024u * 1024u)                /* memory budget per message */

extern u32 k_tick;
extern bool k_replaying;

void  kernel_boot(void);
int   kernel_put(const char *kind, Val *data, const char **tags, int ntags, const char *by,
                 const char *op, const char (*inputs)[65], int ninputs);
Cell *cell_find(const char *name);
Cell *cell_spawn(const char *name, int code, const char **err);
bool  cell_kill(const char *name);
/* Attenuate `parent_cap` (held by `by`) to `rights` for `holder` (a cell or a seat), bound as `label`. */
bool  kernel_grant(int parent_cap, const char *by, u32 rights, const char *holder, const char *label, const char **err);
bool  cell_link(const char *src, const char *target, const char *label, const char **err);
bool  cell_send(const char *to, Val *body, const char *from, const char **err);
bool  kernel_revoke(int cap_idx, const char *by, const char **err);
int   kernel_run(int max_ticks);                         /* rounds of time slices; paused cells resume on the next run */
Val  *kernel_syscall(Interp *it, Val *cap, const char *op, Val **argv, int argc);
int   cell_count(Cell **out, int cap);
bool  kernel_rewind(int upto, const char **err);         /* discarded tail goes to the attic */

/* attic: branches cut off by rewind, restorable when the ledger is back at their start */
typedef struct { u32 from, count, uptime; const char *first; } Branch;
int   attic_list(Branch *out, int cap);
bool  attic_restore(int k, const char **err);

/* ---------------------------------------------------------------- embedded cells */
typedef struct { const char *name; const char *src; } BuiltinCell;
extern const BuiltinCell BUILTIN_CELLS[];
extern const int N_BUILTIN_CELLS;

/* ---------------------------------------------------------------- shell */
void shell_main(void);

#endif
