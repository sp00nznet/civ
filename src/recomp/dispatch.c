/*
 * dispatch.c - computed-transfer target for the lifted code.
 *
 * The relifted code (pcrecomp lift16) emits recomp_dispatch(cpu, seg, off)
 * wherever the original does an indirect call/jmp or a far transfer we could
 * not resolve statically. Before the relift these sites were dropped on the
 * floor as a `/* needs dispatch *\/` comment, so every function pointer in the
 * game silently did nothing.
 *
 * Lifted functions are named far_SSSS_OOOO after their *image* segment:offset
 * (the EXEPACK dump's address space, which starts at CIV_LOAD_SEG:0000).
 * g_dispatch (RecompiledFuncs/civ_dispatch.c) is that table, sorted by
 * image linear address.
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */
#include "recomp/cpu.h"
#include <stdio.h>
#include <stdlib.h>

#define CIV_LOAD_BASE  (0x0100UL * 16)   /* linear address of image offset 0 */

typedef struct { unsigned long addr; void (*fn)(CPU*); } civ_dispatch_entry;
extern const civ_dispatch_entry g_dispatch[];
extern const int g_dispatch_count;

long g_dispatch_calls = 0;
long g_dispatch_misses = 0;

static void (*lookup(unsigned long img))(CPU*)
{
    int lo = 0, hi = g_dispatch_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned long a = g_dispatch[mid].addr;
        if (a == img) return g_dispatch[mid].fn;
        if (a < img) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* A far pointer of 0000:0000 is an empty slot, not an address.
 *
 * The game builds function-pointer tables at runtime and not every slot is
 * filled on every path. Resolving a null pointer by linear address finds
 * whatever happens to sit at image offset 0 and calls it -- which is how a
 * retry loop turned into 2.89 million calls to far_0000_0000 while every
 * other stub was called exactly once. Real code below the load base does not
 * exist either: that region is the interrupt vector table and the BIOS area.
 */
static int null_target(uint16_t seg, uint16_t off)
{
    return seg == 0 && off == 0;
}

void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off)
{
    unsigned long lin = ((unsigned long)seg << 4) + off;
    void (*fn)(CPU*);

    g_dispatch_calls++;
    if (null_target(seg, off)) return;

    /* ponytail: two address spaces reach here. An indirect call reads a live
     * (relocated) segment out of memory; a static far jmp the lifter could not
     * resolve carries the raw image segment from the instruction. They differ
     * by exactly CIV_LOAD_BASE, and the table is far too sparse for both to
     * hit, so try the relocated reading first and fall back. Split into two
     * entry points if a collision ever shows up. */
    fn = (lin >= CIV_LOAD_BASE) ? lookup(lin - CIV_LOAD_BASE) : NULL;
    if (!fn) fn = lookup(lin);

    if (fn) { fn(cpu); return; }

    g_dispatch_misses++;
    if (getenv("CIV_TRACEDISPATCH"))
        fprintf(stderr, "[DISPATCH] miss %04X:%04X (linear %06lX)\n", seg, off, lin);
}

/* DIV/IDIV by zero. On real hardware this is INT 0; a lifted program cannot
 * raise that, and killing the process on the first one hides the diagnosis --
 * a mis-sized operand produces a stream of them and the first few are the
 * evidence. Leave the quotient alone and carry on. */
void recomp_div0(const char *what)
{
    static long n = 0;
    if (++n <= 10 || getenv("CIV_TRACEDISPATCH"))
        fprintf(stderr, "[DIV0] divide by zero #%ld\n", n);
}

/* ---------- Call trace ----------
 *
 * A hang in lifted code leaves no evidence: the log stops and every function
 * looks equally guilty. This keeps a call count per function name (pointers
 * are string literals, so the name pointer itself is the key) and dumps the
 * busiest ones periodically, which turns "it hangs somewhere after fopen" into
 * a ranked list. Enabled by CIV_TRACEFN=1; costs a branch when off.
 *
 * ponytail: linear scan over a small table, no hashing. It is called from
 * every lifted function, so if the trace itself ever dominates a profile,
 * swap the scan for a pointer-keyed hash -- but only then.
 */
#define TRACE_MAX 512
static struct { const char *fn; unsigned long n; } g_tr[TRACE_MAX];
static int g_tr_used = 0;
static unsigned long g_tr_calls = 0;
int g_trace_fn = 0;
static int g_sp_odd_reported = 0;

/* Last few functions entered, for "who called this?" at a failure site.
 * The stream shows entries but not returns, so it cannot show nesting --
 * this ring is what a failing routine prints to name its own context. */
#define FN_RING 8
static const char *g_fn_ring[FN_RING];
static int g_fn_ring_pos = 0;

void recomp_trace_ring(const char *why)
{
    int k;
    fprintf(stderr, "[RING] %s, most recent last:\n", why);
    for (k = 0; k < FN_RING; k++) {
        const char *f = g_fn_ring[(g_fn_ring_pos + k) % FN_RING];
        if (f) fprintf(stderr, "[RING]   %s\n", f);
    }
}

void recomp_trace(CPU *cpu, const char *fn)
{
    int i;
    /* SP must stay even: every x86 stack op is word-sized. An odd SP
     * means an unpaired inc/dec sp ran -- opcodes 0x44/0x4C, which are
     * ASCII 'D' and 'L', i.e. text decoded as code. After that every
     * frame is a byte out and every argument read straddles two words.
     * Report the first function that sees it, with the ring for context. */
    if ((cpu->sp & 1) && !g_sp_odd_reported) {
        g_sp_odd_reported = 1;
        fprintf(stderr, "[SPODD] SP=%04X on entry to %s\n", cpu->sp, fn);
        recomp_trace_ring("SP went odd");
    }
    g_fn_ring[g_fn_ring_pos] = fn;
    g_fn_ring_pos = (g_fn_ring_pos + 1) % FN_RING;
    /* CIV_TRACEFN=2 streams every entry. A loop *inside* one lifted
     * function makes no calls at all, so counters never fire and the
     * only evidence is the last name printed before the log goes
     * quiet. Verbose, but you only ever read the tail. */
    if (g_trace_fn == 2) { fprintf(stderr, "[FN] sp=%04X %s\n", cpu->sp, fn); }

    for (i = 0; i < g_tr_used; i++) {
        if (g_tr[i].fn == fn) { g_tr[i].n++; goto counted; }
    }
    if (g_tr_used < TRACE_MAX) {
        g_tr[g_tr_used].fn = fn;
        g_tr[g_tr_used].n = 1;
        g_tr_used++;
    }
counted:
    /* Report on a schedule rather than at exit: a hang never exits. */
    if ((++g_tr_calls % 20000000UL) == 0) {
        int top[8], k, t;
        for (k = 0; k < 8; k++) top[k] = -1;
        for (i = 0; i < g_tr_used; i++)
            for (k = 0; k < 8; k++)
                if (top[k] < 0 || g_tr[i].n > g_tr[top[k]].n) {
                    for (t = 7; t > k; t--) top[t] = top[t - 1];
                    top[k] = i;
                    break;
                }
        fprintf(stderr, "[TRACEFN] after %lu calls, busiest:\n", g_tr_calls);
        for (k = 0; k < 8 && top[k] >= 0; k++)
            fprintf(stderr, "[TRACEFN]   %-22s %lu\n",
                    g_tr[top[k]].fn, g_tr[top[k]].n);
        fflush(stderr);
    }
}

/* ---------- Stack-balance detector ----------
 *
 * Entry tracing cannot find a leaked pop: the damage happens on the way OUT,
 * and by the next entry the guilty frame is gone. Every x86 stack op is
 * word-sized, so a function whose SP moves an ODD number of bytes between
 * entry and exit is provably wrong, whatever its calling convention.
 * That is the whole test -- no need to model near vs far vs `ret N`.
 */
#define SHADOW_MAX 256
static struct { const char *fn; uint16_t sp; } g_shadow[SHADOW_MAX];
static int g_depth = 0;
static int g_odd_delta_reported = 0;
static int g_bp_reported = 0;

void recomp_enter_sp(CPU *cpu, const char *fn)
{
    if (g_depth < SHADOW_MAX) {
        g_shadow[g_depth].fn = fn;
        g_shadow[g_depth].sp = cpu->sp;
    }
    g_depth++;
}

void recomp_leave_sp(CPU *cpu, const char *fn)
{
    if (g_depth > 0) g_depth--;
    if (g_depth < SHADOW_MAX && g_shadow[g_depth].fn == fn) {
        int delta = (int)(int16_t)(cpu->sp - g_shadow[g_depth].sp);
        if ((delta & 1) && g_odd_delta_reported < 12) {
            g_odd_delta_reported++;
            fprintf(stderr, "[SPBAD] %s moved SP by %d (odd) : %04X -> %04X\n",
                    fn, delta, g_shadow[g_depth].sp, cpu->sp);
            if (g_odd_delta_reported == 1) recomp_trace_ring("first odd delta");
        }
    }
}

/* Per-invocation stack check.
 *
 * The previous version kept a shadow stack keyed by depth, which is only
 * correct if every function exits through a hook. One that does not drifts
 * the counter and the tool then compares mismatched frames -- it reported a
 * 49-byte odd delta that could not be true of the function it named. This
 * takes the entry SP as an argument from a local in the function itself, so
 * there is no pairing to get wrong.
 */
void recomp_check_sp(CPU *cpu, const char *fn,
                     uint16_t entry_sp, uint16_t entry_bp)
{
    int delta = (int)(int16_t)(cpu->sp - entry_sp);
    /* BP is a global register here, so a callee that does not restore it
     * poisons its caller's `mov sp, bp` epilogue -- and that callee's own SP
     * delta stays even, so an SP-only check can never see it. This is the
     * check that can. */
    if (cpu->bp != entry_bp && g_bp_reported < 12) {
        g_bp_reported++;
        fprintf(stderr, "[BPBAD] %s did not restore BP : %04X -> %04X\n",
                fn, entry_bp, cpu->bp);
    }
    if ((delta & 1) && g_odd_delta_reported < 12) {
        g_odd_delta_reported++;
        fprintf(stderr, "[SPBAD] %s moved SP by %d (odd) : %04X -> %04X\n",
                fn, delta, entry_sp, cpu->sp);
        if (g_odd_delta_reported == 1) recomp_trace_ring("first odd delta");
    }
}

/* ---------- Indirect call dispatch ----------
 *
 * The lifter gates indirect call/jmp resolution behind Lifter.dispatch, and
 * these are the two helpers it emits when it is on. They differ from
 * recomp_dispatch in one way that matters: the call site has already pushed a
 * return frame, so when the target cannot be resolved these must pop it back
 * off. recomp_dispatch does not, and a caller then reads its own locals off a
 * stack two or four bytes out -- which corrupts quietly rather than crashing.
 *
 * A miss is not fatal: the game builds function-pointer tables at runtime and
 * not every slot is populated on every path.
 */
long g_dispatch_far_miss = 0;
long g_dispatch_near_miss = 0;

void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off)
{
    unsigned long lin = ((unsigned long)seg << 4) + off;
    void (*fn)(CPU*) = (lin >= CIV_LOAD_BASE) ? lookup(lin - CIV_LOAD_BASE) : NULL;

    g_dispatch_calls++;
    if (null_target(seg, off)) { cpu->sp += 4; return; }
    if (!fn) fn = lookup(lin);
    if (fn) { fn(cpu); return; }

    /* The refill slot, when the game filled it with a pointer it derived from
     * our fake FILE.
     *
     * fopen returns an opaque token (CIV_FILE_TOKEN_BASE 0xF200) rather than a
     * guest FILE struct, so the CRT stream setup computes its buffer-refill
     * callback out of fields that are not there and stores garbage -- observed
     * as F25A:CD60 and C19E:C19E. The intended target is known: far_1FB6_0642,
     * the buffer fill. Only fire when the unresolved target is exactly what
     * DS:0xE84A holds, so this cannot catch an unrelated bad pointer.
     *
     * ponytail: a shim, and the honest fix is a real guest FILE struct so the
     * CRT computes this itself. Do that when anything else dereferences a
     * FILE; until then this is the whole of the damage.
     */
    if (mem_read16(cpu, cpu->ds, 0xE84A) == off &&
        mem_read16(cpu, cpu->ds, 0xE84C) == seg) {
        void (*fill)(CPU*) = lookup(0x1FB6UL * 16 + 0x0642);
        if (fill) { fill(cpu); return; }
    }

    cpu->sp += 4;                      /* unwind the far frame the caller pushed */
    g_dispatch_far_miss++;
    if (getenv("CIV_TRACEDISPATCH") || g_dispatch_far_miss <= 10)
        fprintf(stderr, "[DISPATCH] far miss %04X:%04X (ds=%04X slot E84A=%04X:%04X)\n",
                seg, off, cpu->ds,
                mem_read16(cpu, cpu->ds, 0xE84C),
                mem_read16(cpu, cpu->ds, 0xE84A));
}

void dispatch_near(CPU *cpu, uint16_t seg, uint16_t off)
{
    unsigned long lin = ((unsigned long)seg << 4) + off;
    void (*fn)(CPU*) = (lin >= CIV_LOAD_BASE) ? lookup(lin - CIV_LOAD_BASE) : NULL;

    g_dispatch_calls++;
    if (null_target(seg, off)) { cpu->sp += 2; return; }
    if (!fn) fn = lookup(lin);
    if (fn) { fn(cpu); return; }

    cpu->sp += 2;                      /* unwind the near frame */
    g_dispatch_near_miss++;
    if (getenv("CIV_TRACEDISPATCH") || g_dispatch_near_miss <= 10)
        fprintf(stderr, "[DISPATCH] near miss %04X:%04X\n", seg, off);
}
