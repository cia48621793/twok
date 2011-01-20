/* twok-0.10
   public domain
   python-ish native compiling scripting language
   http://h4ck3r.net/#twok

   Scott Graham 2011 <scott.twok@h4ck3r.net>
   No warranty implied; use at your own risk.


Before including,

    #define TWOK_DEFINE_IMPLEMENTATION

in *one* C file that you want to contain the implementation.


ABOUT:

    Native compile on x64, ARM (not yet), PPC (not yet), or interpreted for console
    No external dependencies

    ("twok" is a reference to the lines of code for the implementation)


TODO:

    lists
        - first usage in a function should be @blah (in args, or first
          assignment to local)
        - becomes pointer for rest of function, and can be used without @:

            @L = []
            push(L, 4)
            push(L, 3)
            x = pop(L)

    C function calls and runtime lib
    mempush/pop for 'gc'
    @var and free func for manual memory
    uninit var tracking (make it optional?)
    arm backend (on android ndk maybe)
    more tests for various math/expr ops

    dupe getReg calls in g_rval
    share genlocal and atom.T_IDENT


NOTES: (mostly internal mumbling)

    functions
        - indirected through global table for hotpatching
        - just add all interned names of functions to a list and use the index
          in that list as func identifier (hash won't work so well)
    need to know in body of function if name is a (global) function or not
        - if assigned (anywhere in the body) it's a local for the whole body
        - otherwise, it's a global function. only functions for now.
        - hmm, scan sucks. how about: global if already defined, otherwise local
          (works for funcs, except fwddecl, allow def f(): pass at some point)
    logic ops, and or:
        - allocate a stack tmp
        - store the value to check into tmp then branch if nz/z
        - reload stack for TOS at end of all or/ands
        - keeping reg alloc working is tricky because running through code in 
          straight line, so TOS doesn't mirror branching or bool ops. using
          stack means that the registers/vst aren't affected outside each arm
          of the or/and conditions.
    logical not: just == 0 and back into reg
    math functions, + - * / & | ^
    unary ops
    parens for precedence

    externs:
        - would be nice to dlsym externs automatically, but then we'd have to
          scan the body of functions to know what was assigned to, rather than
          just used.
        - we could either do that, or require explicit 'extern blah'
          declarations at global scope?
        - can't just dlsym first because then whatever was imported into the C
          program might override globals and locals of the program which would
          be stupid.
        - blech, dlsym sucks. can't get stuff from current elf unless you add
          -rdynamic to the command line. can load from clib or other .so, but,
          meh.

    function call args
        - follow the abi for the platform. x64 is unfortunately different
        between microsoft and linux/osx (which follow amd's). they have a
        different number of reg args and msft reserves spill locations for the
        register args.
        - we want to interop easily w/ C, so use this for internal functions
        too (unfortunately)

    stack layout on x64, each is REG_SIZE(=8) big

        higher numbers
        +-------------------------
        | extra (non-reg) arg 3
        +-------------------------
        | extra arg 2
        +-------------------------
        | extra arg 1
        +-------------------------
        | extra arg 0
        +-------------------------
        | return addr 
        +-------------------------
        | prev rbp         <-- RBP
        +-------------------------
        | linear arg copies
        | ...
        +-------------------------
        | locals/spills
        | ...
        +-------------------------
        lower numbers

        so,
        locals are rbp-8, -16, -24, ...
        extra args are rbp+8, +16, +24, ...

        for simplicity, on entry to function, we extend rsp and copy args into
        linear place to lookup. first 4 to 6 are in registers depending on abi.
        so,
        - rbp-8 is arg 0
        - rbp-16 is arg 1
        - rbp-24 is arg 2
        ...
        and,
        - rbp-N is local 0
        - rbp-N+8 is local 1
        ...
    

*/

#ifndef INCLUDED_TWOK_H
#define INCLUDED_TWOK_H

#ifdef __cplusplus
extern "C" {
#endif

extern int twokRun(char *code, void *(*externLookup)(char *name));

#ifdef __cplusplus
}
#endif

#endif /* INCLUDED_TWOK_H */

#ifdef TWOK_DEFINE_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>
#include <string.h>
#include <ctype.h>
#if __unix__ || (__APPLE__ && __MACH__)
    #include <sys/mman.h>
    static void* twok_allocExec(int size) { void* p = mmap(0, size, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); memset(p, 0x90, size); return p; }
    static void twok_freeExec(void* p, int size) { munmap(p, size); }
    static int twok_CTZ(int x) { return __builtin_ctz(x); }
#elif _WIN32
    #if _M_PPC
    #else
        #include <windows.h>
        static void* twok_allocExec(int size) { void* p = VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); memset(p, 0x90, size); return p; }
        static void twok_freeExec(void* p, int size) { VirtualFree(p, size, MEM_RELEASE); }
        #pragma intrinsic(_BitScanForward)
        static int twok_CTZ(int x) { unsigned long ret; _BitScanForward(&ret, x); return ret; }
        #define strdup _strdup
        #define strtoll _strtoi64
    #endif
#endif

typedef struct Token {
    int type, pos;
    union {
        char* str;
        long long tokn;
    } data;
} Token;

typedef struct Value {
    union {
        unsigned long long _;
        int type;
        void (*handler)(struct Value*);
    } tag;
    union {
        unsigned long long _;
        int i;
        long long l;
        char* p;
    } data;
    int label;
} Value;
#define VAL(t, d) do { Value _ = { { (t) }, { (d) }, 0xbad1abe1 }; tvpush(C.vst, _); } while(0)
#define J_UNCOND 2
#define tarrsize(a) ((int)(sizeof(a)/sizeof((a)[0])))

typedef struct Context {
    Token *tokens;
    int curtok, irpos;
    char *input, *codeseg, *codesegend, *codep, **strs, **locals, *localisptr, **funcnames, **funcaddrs, **externnames, **externaddrs;
    void *(*externLookup)(char *name);
    Value *instrs, *vst;
    jmp_buf errBuf;
    char errorText[512];
} Context;


static Context C;
static void suite();
static void or_test();
static int atomplus();

/*
 * misc utilities.
 */

/* simple vector based on http://nothings.org/stb/stretchy_buffer.txt */
#define tvfree(a)                   ((a) ? (free(tv__zvraw(a)),(void*)0) : (void*)0)
#define tvpush(a,v)                 (tv__zvmaybegrow(a,1), (a)[tv__zvn(a)++] = (v))
#define tvpop(a)                    (((tv__zvn(a) > 0)?((void)0):error("assert")), tv__zvn(a)-=1)
#define tvsize(a)                   ((a) ? tv__zvn(a) : 0)
#define tvadd(a,n)                  (tv__zvmaybegrow(a,n), tv__zvn(a)+=(n), &(a)[tv__zvn(a)-(n)])
#define tvlast(a)                   ((a)[tv__zvn(a)-1])
#define tvindexofnp(a,i,n,psize)    (tv__zvfind((char*)(a),(char*)&(i),sizeof(*(a)),n,psize))
#define tvindexof(a,i)              ((a) ? (tv__zvfind((char*)(a),(char*)&(i),sizeof(*(a)),tv__zvn(a),sizeof(*(a)))) : -1)
#define tvcontainsnp(a,i,n,psize)   ((a) ? (tv__zvfind((char*)(a),(char*)&(i),sizeof(*(a)),n,psize)!=-1) : 0)
#define tvcontainsp(a,i,psize)      (tvcontainsnp((a),i,tv__zvn(a),psize))
#define tvcontainsn(a,i,n)          ((a) ? (tvcontainsnp((a),i,n,sizeof(*(a)))) : 0)
#define tvcontainsn_nonnull(a,i,n)  (tv__zvfind((char*)(a),(char*)&(i),sizeof(*(a)),n,sizeof(*(a)))!=-1) /* workaround for stupid warning */
#define tvcontains(a,i)             ((a) ? (tvcontainsp((a),i,sizeof(*(a)))) : 0)

#define tv__zvraw(a) ((int *) (a) - 2)
#define tv__zvm(a)   tv__zvraw(a)[0]
#define tv__zvn(a)   tv__zvraw(a)[1]

#define tv__zvneedgrow(a,n)  ((a)==0 || tv__zvn(a)+n >= tv__zvm(a))
#define tv__zvmaybegrow(a,n) (tv__zvneedgrow(a,(n)) ? tv__zvgrow(a,n) : 0)
#define tv__zvgrow(a,n)  tv__zvgrowf((void **) &(a), (n), sizeof(*(a)))

static void tv__zvgrowf(void **arr, int increment, int itemsize)
{
    int m = *arr ? 2*tv__zvm(*arr)+increment : increment+1;
    void *p = realloc(*arr ? tv__zvraw(*arr) : 0, itemsize * m + sizeof(int)*2);
    if (p) {
        if (!*arr) ((int *) p)[1] = 0;
        *arr = (void *) ((int *) p + 2);
        tv__zvm(*arr) = m;
    }
}
static int tv__zvfind(char* arr, char* find, int itemsize, int n, int partialsize)
{
    int i;
    for (i = 0; i < n; ++i)
        if (memcmp(&arr[i*itemsize], find, partialsize) == 0)
            return i;
    return -1;
}

#define PREVTOK (C.tokens[C.curtok - 1])
#define CURTOK (&C.tokens[C.curtok])
#define CURTOKt (CURTOK->type)

static void geterrpos(int offset, int* line, int* col, char** linetext)
{
    char* cur = C.input;
    *line = 1; *col = 1;
    *linetext = cur;
    while (--offset >= 0)
    {
        *col += 1;
        if (*cur++ == '\n')
        {
            *linetext = cur;
            *line += 1;
            *col = 1;
        }
    }
}

/* report error message and longjmp */
static void error(char *fmt, ...)
{
    va_list ap;
    int line, col;
    char* text, *eotext;
    char tmp[256];

    va_start(ap, fmt);
    if (C.curtok < tvsize(C.tokens)) /* for errors after parse finished */
    {
        geterrpos(CURTOK->pos, &line, &col, &text);
        sprintf(C.errorText, "line %d, col %d:\n", line, col);
        eotext = text;
        while (*eotext != '\n' && *eotext != 0) ++eotext;
        strncat(C.errorText, text, eotext - text + 1);
        while (--col) strcat(C.errorText, " "); /* todo; ! */
        strcat(C.errorText, "^\n");
    }
    vsprintf(tmp, fmt, ap);
    strcat(C.errorText, tmp);
    strcat(C.errorText, "\n");
    longjmp(C.errBuf, 1);
    va_end(ap);
}

/*
 * tokenize. build a tv of Token's for rest. indent/dedent is a bit icky.
 */

static char KWS[] = " if elif else or for def return extern mod and not print pass << >> <= >= == != ";
#define KW(k) ((int)((strstr(KWS, #k " ") - KWS) + T_KW))
char* strintern(char* s)
{
    int i;
    for (i = 0; i < tvsize(C.strs); ++i)
        if (strcmp(s, C.strs[i]) == 0)
            return C.strs[i];
    tvpush(C.strs, strdup(s));
    return tvlast(C.strs);
}
enum { T_UNK, T_KW=1<<7, T_IDENT = 1<<8, T_END, T_NL, T_NUM, T_INDENT, T_DEDENT };
#define TOK(t) do { Token _ = { t, (int)(startpos - C.input), { strintern(#t) } }; tvpush(C.tokens, _); } while(0)
#define TOKI(t, s) do { Token _ = { t, (int)(startpos - C.input), { strintern(s) } }; tvpush(C.tokens, _); } while(0)
#define TOKN(t, v) do { Token _ = { t, (int)(startpos - C.input), { 0 } }; _.data.tokn=v; tvpush(C.tokens, _); } while(0)
#define isid(ch) (isalnum(ch) || ch == '_')

static void tokenize()
{
    char *pos = C.input, *startpos;
    int i, tok, column;
    int *indents = 0;
    char *ident = 0, *tempident = 0;

    tvpush(indents, 0);

    for (;;)
    {
        column = 0;
        while (*pos == ' ') { pos++; column++; }
        startpos = pos;

        if (*pos == 0)
        {
donestream: for (i = 1; i < tvsize(indents); ++i)
                TOK(T_DEDENT);
            TOK(T_END);
            tvfree(indents);
            tvfree(ident);
            return;
        }

        if (*pos == '#' || *pos == '\r' || *pos == '\n')
        {
            if (*pos == '#')
                while (*pos++ != '\n') {}
            else
                ++pos;
        }
        while (column < tvlast(indents))
        {
            if (!tvcontains(indents, column)) error("unindent does not match any outer indentation level");
            tvpop(indents);
            TOK(T_DEDENT);
        }
        if (column > tvlast(indents))
        {
            tvpush(indents, column);
            TOK(T_INDENT);
        }

        while (*pos != '\n')
        {
            while (*pos == ' ') ++pos;
            startpos = pos;
            ident = tvfree(ident);
            tok = *pos;
            if (isid(*pos))
            {
                while (isid(*pos))
                    tvpush(ident, *pos++);
                tvpush(ident, 0);
                if (isdigit(tok))
                    TOKN(T_NUM, strtoll(ident, 0, 0));
                else
                {
                    /* oops, need to search with space before/after so "i"
                     * isn't found in "if" and "x" isn't found in "extern". */
                    tvpush(tempident, ' ');
                    for (i = 0; i < tvsize(ident); ++i) tvpush(tempident, ident[i]);
                    tvpush(tempident, ' ');
                    tvpush(tempident, 0);
                    tok = T_IDENT;
                    if (strstr(KWS, tempident)) tok = (int)(strstr(KWS, tempident) + 1 /*space*/ - KWS + T_KW);
                    TOKI(tok, ident);
                    tempident = tvfree(tempident);
                }
            }
            else if (*pos == '#')
            {
                while (*pos != '\n' && *pos != 0) ++pos;
                break;
            }
            else
            {
                char tmp[3] = { 0, 0, 0 };
                if (!*pos) goto donestream;
                tmp[0] = *pos++;
                if (0) {}
                #define twochar(t) else if (*(pos-1) == #t[0] && *pos == #t[1]) { tmp[1] = *pos++; TOKI(KW(t), tmp); }
                    twochar(<<) twochar(>>)
                    twochar(<=) twochar(>=)
                    twochar(!=) twochar(==)
                #undef twochar
                else TOKI(tmp[0], tmp);
            }
        }
        TOK(T_NL);
        ++pos;
    }
}

/*
 * backends, define one of them.
 */

/* x64 backend */
#if defined(_M_X64) || defined(__amd64__)

/* currently used: rax, rcx, rdx, rsi, rdi; all volatile across calls.
 * hmm. actually difficult to construct basic math ops that use more than 4
 * regs anyway, so not worth it straight away. */
enum { V_TEMP=0x1000, V_ADDR=0x2000, V_LOCAL=0x4000, V_FUNC=0x8000, V_IMMED=0x10000,
       V_REG_RAX=0x0001, V_REG_RCX=0x0002, V_REG_RDX=0x0004, V_REG_RBX=0x0008,
       V_REG_RSP=0x0010, V_REG_RBP=0x0020, V_REG_RSI=0x0040, V_REG_RDI=0x0080,
       V_REG_R8=0x0100, V_REG_R9=0x0200, V_REG_R10=0x0400, V_REG_R11=0x0800,
       V_REG_ANY=V_REG_RAX | V_REG_RCX | V_REG_RDX | V_REG_R8 | V_REG_R9, /* all volatile for all abis */
       V_REG_FIRST = V_REG_RAX, V_REG_LAST = V_REG_R11,
};

/* bah. asshats used different abis for x64. */
static int funcArgRegs[] = {
#if __unix__ || (__APPLE__ && __MACH__)
    V_REG_RDI, V_REG_RSI, V_REG_RDX, V_REG_RCX, V_REG_R8, V_REG_R9
#elif _WIN32
    V_REG_RCX, V_REG_RDX, V_REG_R8, V_REG_R9
#endif
};


enum { REG_SIZE = 8, FUNC_THUNK_SIZE = 8 };

#define ob(b) (*C.codep++ = (b))
#define lead(r) ob(0x48 | ((r>=V_REG_R8)?1:0))
#define lead2(r1,r2) ob(0x48 | ((r1>=V_REG_R8)?1:0) | ((r2>=V_REG_R8)?4:0))
#define outnum32(n) { unsigned int _ = (unsigned int)(n); unsigned int mask = 0xff; unsigned int sh = 0; int i; for (i = 0; i < 4; ++i) { ob((char)((_&mask)>>sh)); mask <<= 8; sh += 8; } }
#define outnum64(n) { unsigned long long _ = (unsigned long long)(n); unsigned long long mask = 0xff; unsigned long long sh = 0; int i; for (i = 0; i < 8; ++i) { ob((char)((_&mask)>>sh)); mask <<= 8; sh += 8; } }

typedef struct NativeContext {
    int spills[64]; /* is the Nth spill location in use? */
    char *numlocsp;
    char **paramnames;
} NativeContext;
static NativeContext NC;

#define vreg_to_enc(vr) (((vr) >= V_REG_R8) ? twok_CTZ(((vr)>>8)) : twok_CTZ(vr))

#define put32(p, n) (*(int*)(p) = (n))
#define get32(p) (*(int*)p)

static char* functhunkaddr(long long idx) { return C.codesegend - (idx + 1) * FUNC_THUNK_SIZE; }
static void addfunc(char* name, char* addr)
{
    char* p;
    if (tvcontains(C.funcnames, name)) error("%s already defined", name);
    tvpush(C.funcnames, name);
    tvpush(C.funcaddrs, addr);
    p = functhunkaddr(tvsize(C.funcnames) - 1);
    *p++ = 0xe9; /* jmp relimmed */
    put32(p, (int)(addr - p - 4)); p += 4;
    *p++ = 0xcc; *p++ = 0xcc; *p++ = 0xcc; /* add int3 to rest of thunk */
}
static int funcidx(char* name) { char* p = strintern(name); return tvindexof(C.funcnames, p); }

/* store the given register (offset) into the given stack slot */
static void g_storespill(int reg, int slot)
{
    lead(reg); ob(0x89); ob(0x85 + vreg_to_enc(reg));
    outnum32((-1 - slot - tvsize(NC.paramnames) - tvsize(C.locals)) * REG_SIZE);
}

/* load spill # slot into reg */
static void g_loadspill(int reg, int slot)
{
    lead(reg); ob(0x8b); ob(0x85 + vreg_to_enc(reg));
    outnum32((-1 - slot - tvsize(NC.paramnames) - tvsize(C.locals)) * REG_SIZE);
}


static int getReg(int valid)
{
    int i, j, reg;

    /* figure out if it's currently in use */
    for (i = V_REG_FIRST; i <= V_REG_LAST; i <<= 1)
    {
        if ((i & valid) == 0) continue;
        for (j = 0; j < tvsize(C.vst); ++j)
            if ((C.vst[j].tag.type & i))
                break;
        /* not in use, return this one */
        if (j == tvsize(C.vst))
            return i;
    }

    /* otherwise, find the oldest in the class */
    for (j = 0; j < tvsize(C.vst); ++j)
    {
        if ((C.vst[j].tag.type & valid))
        {
            /* and a location to spill it to */
            for (i = 0; i < tarrsize(NC.spills); ++i)
                if (!NC.spills[i]) break;

            /* and send it there and update the flags */
            reg = C.vst[j].tag.type & valid;
            g_storespill(reg, i);
            NC.spills[i] = 1;
            C.vst[j].tag.type &= ~V_REG_ANY;
            C.vst[j].tag.type |= V_TEMP;
            C.vst[j].data.i = i;

            /* and return that register */
            return reg;
        }
    }
    error("internal error, or out of stack slots");
    return -1;
}

static int g_rval(int valid)
{
    int reg, reg2, tag = tvlast(C.vst).tag.type;
    long long val = tvlast(C.vst).data.l;
    if (tag & V_IMMED)
    {
        if (tag & V_ADDR)
        {
            /* mov Reg, const */
            reg = getReg(valid);
            lead(reg); ob(0xb8 + vreg_to_enc(reg));
            outnum64(val);
            /* mov [Reg], Reg */
            lead2(reg, reg); ob(0x89);
            ob(vreg_to_enc(reg) + vreg_to_enc(reg) * 8);
        }
        else
        {
            /* mov Reg, const */
            reg = getReg(valid);
            lead(reg); ob(0xb8 + vreg_to_enc(reg));
            outnum64(val);
        }
    }
    else if (tag & V_LOCAL)
    {
        /* todo; uninit var; keep shadow stack of initialized flags, error on
         * read before write. need to figure out calling C lib */
        reg = getReg(valid);
        lead(reg); ob(0x8b); ob(0x85 + vreg_to_enc(reg) * 8); /* mov rXx, [rbp + xxx] (long form) */
        outnum32(val * REG_SIZE);
    }
    else if (tag & V_FUNC)
    {
        reg = getReg(valid);
        lead(reg); ob(0xb8 + vreg_to_enc(reg)); outnum64(functhunkaddr(val)); /* mov rXx, functhunk */
    }
    else if ((reg = (tag & V_REG_ANY) & valid)) { /* nothing to do, just return register */ }
    else if ((reg2 = (tag & V_REG_ANY)))
    {
        /* in a register, but not the one we need */
        int reg = getReg(valid);
        lead2(reg, reg2); ob(0x89); ob(0xc0 + vreg_to_enc(reg) + vreg_to_enc(reg2) * 8); /* mov rXx, rXx */
    }
    else if (tag & V_TEMP)
    {
        int reg = getReg(valid);
        g_loadspill(reg, (int)val);
    }
    else
    {
        error("internal error, unexpected stack state");
    }
    tvpop(C.vst);
    return reg;
}

static int g_lval(int valid)
{
    int reg = 0, tag = tvlast(C.vst).tag.type;
    long long val = tvlast(C.vst).data.l;
    if (tag & V_ADDR)
    {
        if ((reg = (tag & V_REG_ANY))) { /* nothing, just pop and return reg */ }
        else if (tag & V_IMMED)
        {
            /* mov Reg, const */
            reg = getReg(valid);
            lead(reg); ob(0xb8 + vreg_to_enc(reg));
            outnum64(val);
        }
        else if (tag & V_LOCAL)
        {
            reg = getReg(valid);
            lead(reg); ob(0x8d); ob(0x85 + vreg_to_enc(reg) * 8); /* lea rXx, [rbp - xxx] (long form) */
            outnum32(val * REG_SIZE);
        }
    }
    else
    {
        error("expecting lval");
    }
    tvpop(C.vst);
    return reg;
}

static void i_const(long long v) { VAL(V_IMMED, v); }
static void i_addr(long long v, int extra) { VAL(extra | V_ADDR, v); } /* extra is V_LOCAL, V_FUNC, V_IMMED */
static void i_func(char *name, char **paramnames)
{
    int i;
    while (((unsigned long)C.codep) % 16 != 0) ob(0x90); /* nop to align */
    addfunc(name, C.codep);
    ob(0x55); /* push rbp */
    lead(0); ob(0x89); ob(0xe5); /* mov rbp, rsp */
    lead(0); ob(0x81); ob(0xec); outnum32(0); /* sub rsp, xxx */
    NC.numlocsp = C.codep - 4; /* save for endfunc to patch */
    NC.paramnames = paramnames;
    /* copy args to shadow location, we put them in "upside down" from how
     * they are on the arg stack */
    for (i = 0; i < tvsize(paramnames); ++i)
    {
        /* get either from reg, or from stack, depending on index and abi */
        if (i >= tarrsize(funcArgRegs))
        {
            ob(0x48); ob(0x8b); ob(0x85); outnum32(16+8*i); /* mov rax, [rbp + argoffset] */
        }
        else
        {
            int reg = funcArgRegs[i];
            lead2(V_REG_RAX, reg); ob(0x89); ob(0xc0 + vreg_to_enc(V_REG_RAX) + vreg_to_enc(reg) * 8); /* mov rXx, rXx */
        }
        /* copy into local location */
        ob(0x48); ob(0x89); ob(0x85); outnum32(-8 - 8*i);   /* mov [rbp - copyoffset], rax */
    }
    VAL(V_IMMED, 0); /* for fall off ret */
}
static void i_extern(Token* tok)
{
    /* note, tok->data.str is already interned */
    void *p = C.externLookup(tok->data.str);
    if (!p) error("'%s' not found", tok->data.str);
    if (tvcontains(C.externnames, tok->data.str)) return; /* not an error, just ignore. */
    tvpush(C.externnames, tok->data.str);
    tvpush(C.externaddrs, p);
}

static void i_ret() { g_rval(V_REG_RAX); ob(0xc9); /* leave */ ob(0xc3); /* ret */ }
static void i_endfunc()
{
    i_ret();
    put32(NC.numlocsp, (tvsize(C.locals)+1) * REG_SIZE + 256); /* todo; XXX hardcoded # spills */
}

static void i_cmp(int op)
{
    int a, into;
    struct { char kw, cc; } cmpccs[] = {
        { '<', 0xc },
        { '>', 0xf },
        { KW(<=), 0xe },
        { KW(>=), 0xd },
        { KW(==), 4 },
        { KW(!=), 5 },
    };
    a = g_rval(V_REG_RAX);
    into = g_rval(V_REG_ANY & ~a);
    lead(into); ob(0x39 + vreg_to_enc(a)); ob(0xc0 + vreg_to_enc(into)); /* cmp rXx, rax */

    lead(into); ob(0xb8 + vreg_to_enc(into));
    outnum64(0);
    ob(0x0f);
    ob(0x90 + cmpccs[tvindexofnp(cmpccs, op, 6, 1)].cc);
    ob(0xc0 + vreg_to_enc(into));
    VAL(into, 0);
}

/* emit a jump, returns the location that needs to be fixed up. make a linked
 * list to previous items that are going to jump to the same final location so
 * that when the jump target is reached we can fix them all up by walking the
 * list that we created. */
static char* i_jmpc(int cond, char* prev)
{
    if (cond == J_UNCOND)
    {
        ob(0xe9);
        outnum32(prev ? prev - C.codeseg : 0);
    }
    else
    {
        int reg = g_rval(V_REG_ANY);
        lead2(reg, reg); ob(0x85); ob(0xc0 + vreg_to_enc(reg) * 9); /* test rXx, rXx */
        ob(0x0f); ob(0x84 + cond); /* jz/jnz rrr */
        outnum32(prev ? prev - C.codeseg : 0);
    }
    return C.codep - 4;
}

static void i_label(char* p)
{
    char* to = C.codep;
    while (p)
    {
        char* tmp = get32(p) ? get32(p) + C.codeseg : 0; /* next value in the list before we overwrite it */
        put32(p, (int)(to - p - 4));
        p = tmp;
    }
}

/* lhs, rhs on stack */
static void i_store()
{
    int val = g_rval(V_REG_ANY);
    int into = g_lval(V_REG_ANY & ~val);
    lead2(val, into); ob(0x89);
    ob(vreg_to_enc(into) + vreg_to_enc(val) * 8);
}

static void i_storelocal(int loc)
{
    int val = g_rval(V_REG_ANY), into;
    i_addr(-loc - tvsize(NC.paramnames) - 1, V_LOCAL);
    into = g_lval(V_REG_ANY & ~val);
    lead2(into, val); ob(0x89);
    ob(vreg_to_enc(into) + vreg_to_enc(val) * 8);
}

static void i_addrparam(int loc) { i_addr(-loc - 1, V_LOCAL); }
static void i_addrlocal(int loc) { i_addr(-loc - tvsize(NC.paramnames) - 1, V_LOCAL); }

static void i_call(int argcount)
{
    int i, stackdelta = (argcount - tarrsize(funcArgRegs)) * 8, argnostack = 1;
    if (stackdelta < 0) stackdelta = 0;
#if _WIN32
    stackdelta += 32; /* shadow stack on msft */
    argnostack = 0;
#endif
    if (stackdelta > 0)
    {
        lead(0); ob(0x81); ob(0xec); outnum32(stackdelta);
    }

    /* we have them in reverse order (pushed L->R), so reverse index */
    for (i = 0; i < argcount; ++i)
    {
        int idx = argcount - i - 1;
        if (idx >= tarrsize(funcArgRegs))
        {
            g_rval(V_REG_R11);
            /* mov [rsp+X], r11 */
            ob(0x4c); ob(0x89); ob(0x5c); ob(0x24); ob((idx - tarrsize(funcArgRegs)*argnostack) * 8);
        }
        else g_rval(funcArgRegs[idx]);
    }

    g_rval(V_REG_R11); /* al is used for varargs on amd64 abi, r11 is volatile for both */
    ob(0x41); ob(0xff); ob(0xd3); /* call r11 */

    if (stackdelta > 0)
    {
        lead(0); ob(0x81); ob(0xc4); outnum32(stackdelta); /* clean up */
    }
}

static void i_mathunary(int op)
{
    int reg;
    if (op == '+') return;
    reg = g_rval(V_REG_ANY);
    /* either neg or not */
    lead(reg); ob(0xf7); ob(op == '-' ? 0xd8 : 0xd0 + vreg_to_enc(reg));
    VAL(reg, 0);
}

/* + - * / % & | ^ */
static void i_math(int op)
{
    struct { char math, opc; } map[] = { /* maps KW to x64 instr */
        { '+', 0x01 },
        { '-', 0x29 },
        { '*', 0x0f }, /* actually 0f af */
        { '&', 0x21 },
        { '^', 0x31 },
        { '|', 0x09 } };
    int opi = tvindexofnp(map, op, 6, 1);
    if (opi >= 0 || op == '*')
    {
        int v1 = g_rval(V_REG_ANY);
        int v0 = g_rval(V_REG_ANY & ~v1);
        lead2(v0, v1); ob(map[opi].opc);
        if (op == '*') ob(0xaf);
        ob(0xc0 + vreg_to_enc(v0) + vreg_to_enc(v1) * 8);
        VAL(op == '*' ? v1 : v0, 0); /* bleh, extended imul args backwards? */
    }
    else if (op == '/' || op == '%')
    {
        int v1 = g_rval(V_REG_ANY & ~(V_REG_RAX | V_REG_RDX));
        g_rval(V_REG_RAX);
        lead(V_REG_RAX); ob(0x99); /* cqo (sign extend rax into rdx) */
        lead(v1); ob(0xf7); ob(0xf8 + vreg_to_enc(v1)); /* idiv rXx */
        VAL(op == '/' ? V_REG_RAX : V_REG_RDX, 0); /* quotient in A, remainder in D */
    }
}

#endif

/*
 * parsing and intermediate gen
 */
#define NEXT() do { if (C.curtok >= tvsize(C.tokens)) error("unexpected end of input"); C.curtok++; } while(0)
#define SKIP(t) do { if (CURTOKt != t) error("'%c' expected, got '%s'", t, CURTOK->data.str); NEXT(); } while(0)

static int genlocal()
{
    static int count = 0;
    char buf[128], *name;
    sprintf(buf, "$loc%d", count++);
    name = strintern(buf);
    if (!tvcontains(C.locals, name)) { tvpush(C.locals, name); tvpush(C.localisptr, 0); }
    return tvindexof(C.locals, name);
}

static int atom()
{
    if (CURTOKt == '(')
    {
        NEXT();
        or_test();
        SKIP(')');
        return 1;
    }
    else if (CURTOKt == T_NUM)
    {
        i_const(CURTOK->data.tokn);
        NEXT();
        return 1;
    }
    else if (CURTOKt == '@' || CURTOKt == T_IDENT)
    {
        int i, isptr = 0;
        if (CURTOKt == '@')
        {
            isptr = 1;
            NEXT();
        }
        if ((i = tvindexof(NC.paramnames, CURTOK->data.str)) != -1) i_addrparam(i);
        else if ((i = tvindexof(C.externnames, CURTOK->data.str)) != -1) i_const((unsigned long long)C.externaddrs[i]);
        else if (funcidx(CURTOK->data.str) != -1) i_addr(funcidx(CURTOK->data.str), V_FUNC);
        else
        {
            if (!tvcontains(C.locals, CURTOK->data.str))
            {
                tvpush(C.locals, CURTOK->data.str);
                tvpush(C.localisptr, isptr);
            }
            i_addrlocal(tvindexof(C.locals, CURTOK->data.str));
        }
        NEXT();
        return 1;
    }
    return 0;
}

static int arglist()
{
    int count = atomplus();
    while (count && CURTOKt == ',')
    {
        SKIP(',');
        count += atomplus();
    }
    return count;
}

static char** parameters()
{
    char **ret = 0;
    if (CURTOKt == T_IDENT) 
    {
        tvpush(ret, CURTOK->data.str);
        NEXT();
    }
    while (ret && CURTOKt == ',')
    {
        SKIP(',');
        if (CURTOKt != T_IDENT) error("expecting parameter name");
        tvpush(ret, CURTOK->data.str);
        NEXT();
    }
    return ret;
}

static int trailer()
{
    if (CURTOKt == '(')
    {
        int count;
        NEXT();
        count = arglist();
        SKIP(')');
        i_call(count);
        VAL(V_REG_RAX, 0); /* ret */
    }
    return 0;
}

static int atomplus()
{
    int ret = atom();
    while (trailer()) {}
    return ret;
}

static void factor()
{
    if (CURTOKt == '+' || CURTOKt == '-' || CURTOKt == '~')
    {
        int op = CURTOKt;
        NEXT();
        factor();
        i_mathunary(op);
    }
    else
    {
        atomplus();
    }
}

#define EXPRP(name, sub, tok0, tok1, tok2)                              \
static void name()                                                      \
{                                                                       \
    sub();                                                              \
    while (CURTOKt == tok0 || CURTOKt == tok1 || CURTOKt == tok2)       \
    {                                                                   \
        int op = CURTOKt;                                               \
        NEXT();                                                         \
        sub();                                                          \
        i_math(op);                                                     \
    }                                                                   \
}
EXPRP(term, factor, '*', '/', '%')
EXPRP(arith_expr, term, '+', '-', '-')
EXPRP(and_expr, arith_expr, '&', '&', '&')
EXPRP(xor_expr, and_expr, '^', '^', '^')
EXPRP(expr, xor_expr, '|', '|', '|')

static void comparison()
{
    char cmps[] = { '<', '>', KW(<=), KW(>=), KW(==), KW(!=) };
    expr();
    for (;;)
    {
        Token* cmp = CURTOK;
        if (!tvcontainsn_nonnull(cmps, CURTOKt, 6)) break;
        NEXT();
        expr();
        i_cmp(cmp->type);
    }
}

static void not_test()
{
    if (CURTOKt == KW(not))
    {
        SKIP(KW(not));
        comparison();
        VAL(V_IMMED, 0);
        i_cmp(KW(==));
    }
    else
        comparison();
}

#define BOOLOP(name, sub, kw, cond)             \
static void name()                              \
{                                               \
    char *label = 0;                            \
    sub();                                      \
    if (CURTOKt == KW(kw))                      \
    {                                           \
        int tmp = genlocal(), done = 0;         \
        for (;;)                                \
        {                                       \
            i_storelocal(tmp);                  \
            i_addrlocal(tmp);                   \
            label = i_jmpc(cond, label);        \
            if (done) break;                    \
            SKIP(KW(kw));                       \
            sub();                              \
            done = CURTOKt != KW(kw);           \
        }                                       \
        i_addrlocal(tmp);                       \
    }                                           \
    i_label(label);                             \
}
BOOLOP(and_test, not_test, and, 0)
BOOLOP(or_test, and_test, or, 1)

static void expr_stmt()
{
    or_test();
    if (CURTOKt == '=')
    {
        NEXT();
        or_test();
        i_store();
    }
    else tvpop(C.vst); /* discard */
    SKIP(T_NL);
}

static void stmt()
{
    char *labeldone = 0, *labeltest = 0;
    if (CURTOKt == KW(return))
    {
        SKIP(KW(return));
        if (CURTOKt == T_NL || CURTOKt == T_DEDENT) i_const(20710);
        else or_test();
        i_ret();
        SKIP(T_NL);
    }
    else if (CURTOKt == KW(print))
    {
        SKIP(KW(print));
        NEXT();
        SKIP(T_NL);
    }
    else if (CURTOKt == KW(pass))
    {
        NEXT();
        SKIP(T_NL);
    }
    else if (CURTOKt == KW(if))
    {
        SKIP(KW(if));
        or_test();

        labeltest = i_jmpc(0, 0);
        suite();
        labeldone = i_jmpc(J_UNCOND, 0);
        i_label(labeltest);

        while (CURTOKt == KW(elif) || CURTOKt == KW(else))
        {
            NEXT();
            if (PREVTOK.type == KW(elif))
            {
                or_test();
                labeltest = i_jmpc(0, 0);
            }
            else labeltest = 0;
            suite();
            if (labeltest)
            {
                labeldone = i_jmpc(J_UNCOND, labeldone);
                i_label(labeltest);
            }
        }
        i_label(labeldone);
    }
    else if (CURTOKt == T_NL) error("bad indent");
    else expr_stmt();
}

static void suite()
{
    SKIP(':');
    SKIP(T_NL);
    SKIP(T_INDENT);
    stmt();
    while (CURTOKt != T_DEDENT)
        stmt();
    SKIP(T_DEDENT);
}

static void funcdef()
{
    if (CURTOKt == KW(extern))
    {
        SKIP(KW(extern));
        i_extern(CURTOK);
        NEXT();
        SKIP(T_NL);
    }
    else
    {
        char **argnames, *fname;
        SKIP(KW(def));
        tvfree(C.locals);
        tvfree(C.localisptr);
        fname = CURTOK->data.str;
        SKIP(T_IDENT);
        SKIP('(');
        argnames = parameters();
        i_func(fname, argnames);
        SKIP(')');
        suite();
        i_endfunc();
        tvfree(argnames);
    }
}

static void fileinput()
{
    while (CURTOKt != T_END)
    {
        if (CURTOKt == T_NL) NEXT();
        else funcdef();
    }
    SKIP(T_END);
}

/*
 * main api entry point
 */
int twokRun(char *code, void *(*externLookup)(char *name))
{
    int ret, allocSize, i, entryidx;
    memset(&C, 0, sizeof(C));
    C.input = code;
    C.externLookup = externLookup;
    allocSize = 1<<17;
    if (setjmp(C.errBuf) == 0)
    {
        tokenize();
        /* dump tokens generated from stream */
#if 0
        { int j;
        for (j = 0; j < tvsize(C.tokens); ++j)
        {
            if (C.tokens[j].type == T_NUM)
                printf("%d: %d %d\n", j, C.tokens[j].type, C.tokens[j].data.tokn);
            else
                printf("%d: %d %s\n", j, C.tokens[j].type, C.tokens[j].data.str);
        }}
#endif
        C.codeseg = C.codep = twok_allocExec(allocSize);
        C.codesegend = C.codeseg + allocSize;
        fileinput();
        if (tvsize(C.vst) != 0) error("internal error, values left on stack");
        /* dump disassembly of generated code, needs ndisasm in path */
#if 1
        { FILE* f = fopen("dump.dat", "wb");
        fwrite(C.codeseg, 1, C.codep - C.codeseg, f);
        fclose(f);
        ret = system("ndisasm -b64 dump.dat"); }
#endif

        entryidx = funcidx("__main__");
        if (entryidx == -1) error("no entry point '__main__'");
        ret = ((int (*)())(C.codesegend - (entryidx + 1) * FUNC_THUNK_SIZE))();
    }
    else ret = -1;
    tvfree(C.tokens);
    tvfree(C.vst);
    tvfree(C.instrs);
    tvfree(C.locals);
    tvfree(C.localisptr);
    for (i = 0; i < tvsize(C.strs); ++i) free(C.strs[i]);
    tvfree(C.strs);
    tvfree(C.funcnames); tvfree(C.funcaddrs);
    tvfree(C.externnames); tvfree(C.externaddrs);
    twok_freeExec(C.codeseg, allocSize);
    return ret;
}

#endif /* TWOK_DEFINE_IMPLEMENTATION */
