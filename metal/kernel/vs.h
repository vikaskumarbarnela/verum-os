/* Verum Script on bare metal: the only language a cell may run in. */
#ifndef VERUM_VS_H
#define VERUM_VS_H
#include "lib.h"

typedef enum { T_NIL, T_BOOL, T_INT, T_STR, T_SYM, T_LIST, T_DICT, T_FN, T_PRIM, T_CAP } VType;

typedef struct Val Val;
typedef struct Env Env;
typedef struct Interp Interp;
typedef Val *(*PrimFn)(Interp *it, Val **argv, int argc);

struct Val {
    VType t;
    union {
        int   i;
        bool  b;
        char *s;                                   /* STR / SYM */
        struct { Val **items; int n; } list;
        struct { char **keys; Val **vals; int n; } dict;
        struct { Val *params; Val *body; Env *env; const char *name; } fn;   /* body: LIST of forms */
        struct { const char *name; PrimFn f; } prim;
        struct { char token[33]; const char *label; } cap;
    };
};

struct Env {
    const char *name; Val *val;                    /* one binding per node */
    Env *next;                                     /* same scope */
    Env *outer;                                    /* enclosing scope (only on head node) */
};

/* The one door out of the VM. Returns a value, or longjmps via vs_error. */
typedef Val *(*SyscallFn)(Interp *it, Val *cap, const char *op, Val **argv, int argc);

struct Interp {
    u32 fuel, used;
    int depth;
    SyscallFn syscall;
    void *ctx;                                     /* the kernel's cell pointer */
    u32 jmp[6];                                    /* vs_setjmp buffer */
    const char *error;
};

/* values */
Val *v_nil(void);
Val *v_bool(bool b);
Val *v_int(int i);
Val *v_str(const char *s);
Val *v_sym(const char *s);
Val *v_list(int cap);
void list_push(Val *l, Val *x);
Val *v_dict(void);
Val *dict_get(Val *d, const char *k);
void dict_set(Val *d, const char *k, Val *v);     /* copy-on-write is the caller's job */
Val *v_cap(const char *token, const char *label);
bool truthy(Val *v);
bool v_equal(Val *a, Val *b);
void v_show(Val *v, char *out, usize cap);         /* human rendering */
void v_canon(Val *v, char *out, usize cap);        /* canonical (JSON-like, sorted keys) */
void v_serialize(Val *v, char *out, usize cap);    /* as Verum Script source: (list ...) (dict ...) */
Val *v_deserialize(const char *src);               /* inverse of v_serialize; NULL on error */
Val *v_persist(Val *v);                            /* deep copy into the persistent heap (closures become nil) */

/* environments */
Env *env_new(Env *outer);
void env_define(Env *e, const char *name, Val *v);
Val *env_lookup(Env *e, const char *name);
Env *env_builtins(void);

/* reader & evaluator */
Val *vs_parse(const char *src, const char **err);  /* LIST of top-level forms, or NULL */
void vs_error(Interp *it, const char *msg) __attribute__((noreturn));
Val *vs_eval(Interp *it, Val *x, Env *env);
Val *vs_apply(Interp *it, Val *fn, Val **argv, int argc);

/* Run `forms` in env; returns NULL and sets it->error on failure. */
Val *vs_run(Interp *it, Val *forms, Env *env);
Val *vs_call(Interp *it, Val *fn, Val **argv, int argc);   /* protected apply */

int  vs_setjmp(u32 *buf);
void vs_longjmp(u32 *buf, int v) __attribute__((noreturn));

#define VS_MAX_SIZE  20000
#define VS_MAX_DEPTH 60

#endif
