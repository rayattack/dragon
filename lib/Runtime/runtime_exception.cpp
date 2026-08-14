/// Dragon Runtime - Exception Handling
#include "runtime_internal.h"

extern "C" {

void dragon_raise_exc(int64_t type, const char* msg);
void dragon_raise_exc_cstr(int64_t type, const char* msg);

/// Assert: raises a catchable AssertionError (Python parity) instead of exit(1).
/// Message may be a heap DragonString from codegen; only the fallback literal uses the cstr path.
void dragon_assert(int64_t condition, const char* message) {
    if (!condition) {
        if (message) dragon_raise_exc(24, message);
        else dragon_raise_exc_cstr(24, "AssertionError");
    }
}

/// Assert without message
void dragon_assert_no_msg(int64_t condition) {
    dragon_assert(condition, nullptr);
}

// Exception context: a generator's own context (__dragon_exc_vt) wins over the
// green-thread context (__current_vthread); else TLS globals. See runtime_internal.h.
#define EXC_VT (__dragon_exc_vt ? __dragon_exc_vt : __current_vthread)


// Set by the green thread scheduler in Phase 3 before resuming a coroutine.

/// Push exception frame, return pointer to jmp_buf for setjmp.
/// Dual-path: vthread struct first, TLS fallback.
void* dragon_exc_push_frame() {
    // Track live frame count for the inline cleanup gate (balanced with pop).
    __dragon_active_frames++;
    if (EXC_VT) {
        if (EXC_VT->exc_sp >= DRAGON_EXC_STACK_SIZE - 1) {
            fprintf(stderr, "RuntimeError: exceeded maximum nested try/except depth (%d)\n",
                    DRAGON_EXC_STACK_SIZE);
            abort();
        }
        EXC_VT->exc_sp++;
        // Snapshot the cleanup depth so a longjmp into this frame frees exactly
        // the owned heap locals declared after this push (see DragonCleanupStack).
        EXC_VT->cleanup_saved[EXC_VT->exc_sp] =
            EXC_VT->cleanup.sp;
        return &EXC_VT->exc_stack[EXC_VT->exc_sp];
    }
    if (__dragon_exc_sp >= DRAGON_EXC_STACK_SIZE - 1) {
        fprintf(stderr, "RuntimeError: exceeded maximum nested try/except depth (%d)\n",
                DRAGON_EXC_STACK_SIZE);
        abort();
    }
    __dragon_exc_sp++;
    __dragon_cleanup_saved[__dragon_exc_sp] = __dragon_cleanup.sp;
    return &__dragon_exc_stack[__dragon_exc_sp];
}

/// Pop exception frame
void dragon_exc_pop_frame() {
    // Balanced with the increment in push_frame (see __dragon_active_frames).
    if (__dragon_active_frames > 0) __dragon_active_frames--;
    if (EXC_VT) {
        if (EXC_VT->exc_sp >= 0)
            EXC_VT->exc_sp--;
        return;
    }
    if (__dragon_exc_sp >= 0)
        __dragon_exc_sp--;
}

/// Get current exception type code
int64_t dragon_exc_get_type() {
    if (EXC_VT)
        return (int64_t)EXC_VT->exc_type;
    return (int64_t)__dragon_exc_type;
}

/// Get current exception message
const char* dragon_exc_get_msg() {
    if (EXC_VT)
        return EXC_VT->exc_msg;
    return __dragon_exc_msg;
}

/// Get current exception instance (NULL if the raise carried only a message).
/// `except X as e` binds `e` to this when X matches, so typed fields (e.g. HTTPError.code) survive unwinding.
void* dragon_exc_get_obj() {
    if (EXC_VT)
        return EXC_VT->exc_obj;
    return __dragon_exc_obj;
}

// exc_msg OWNS any mortal-heap DragonString: plain raises dup it, _consume raises take codegen's +1.
// Before this fix the slot only borrowed pointers, leaking every raise with a dynamic message.

static void dragon_exc_msg_set(const char* msg, int consume) {
    const char** slot = EXC_VT ? &EXC_VT->exc_msg
                                          : &__dragon_exc_msg;
    const char* old = *slot;
    if (old == msg) {
        // Re-raise of the in-flight message: the slot already owns it. A
        // consume transfer hands us a SECOND +1 - fold it into the slot's.
        if (consume) dragon_decref_str_dispatch(msg);
        return;
    }
    *slot = consume ? msg : dragon_exc_msg_preserve(msg);
    dragon_decref_str_dispatch(old);
}

/// Bind the in-flight message for `except ... as e`: takes its own +1 (no-op for literal/immortal)
/// so a nested raise in the handler can't leave `e` dangling. Handler scope cleanup decrefs it.
const char* dragon_exc_bind_msg(void) {
    const char* m = EXC_VT ? EXC_VT->exc_msg
                                      : __dragon_exc_msg;
    dragon_incref_str(m);
    return m;
}

// exc_obj OWNS the +1 transferred at raise; `except X as e` binds via its own +1, and each
// overwrite releases the previous instance (pinned by test_rc_exc_instance.dr against a prior leak).

static void dragon_exc_obj_set(void* obj, int consume) {
    void** slot = EXC_VT ? &EXC_VT->exc_obj : &__dragon_exc_obj;
    void* old = *slot;
    if (old == obj) {
        // Re-raise of the in-flight instance: the slot already owns it. A
        // consume transfer hands us a SECOND +1 - fold it into the slot's.
        if (consume && obj) dragon_decref(obj);
        return;
    }
    if (!consume && obj) dragon_incref(obj);
    *slot = obj;
    if (old) dragon_decref(old);
}

/// Bind the in-flight instance for `except X as e`: takes its own +1 so a nested raise in the
/// handler can't leave `e` dangling. Handler scope/unwind cleanup decrefs it. NULL-safe.
void* dragon_exc_bind_obj(void) {
    void* o = EXC_VT ? EXC_VT->exc_obj : __dragon_exc_obj;
    dragon_incref(o);
    return o;
}

/// NULL-safe retain for an instance saved before a `finally` body runs; the deferred re-raise
/// transfers this hold back into the slot (mirrors dragon_exc_bind_msg's retain-at-save).
void* dragon_exc_retain_obj(void* o) {
    dragon_incref(o);
    return o;
}

static void dragon_raise_exc_impl(int64_t type, void* obj, const char* msg,
                                  int consume) {
    dragon_exc_msg_set(msg, consume);
    // obj is always a +1 transfer or NULL; borrowed-instance sites retain first via
    // dragon_exc_retain_obj so the same-pointer fold in dragon_exc_obj_set keeps re-raises balanced.
    dragon_exc_obj_set(obj, /*consume=*/1);
    if (EXC_VT) {
        EXC_VT->exc_type = (int)type;
        if (EXC_VT->exc_sp >= 0) {
            int sp = EXC_VT->exc_sp;
            longjmp(EXC_VT->exc_stack[sp], (int)type);
        }
        fprintf(stderr, "Unhandled exception: %s\n",
                EXC_VT->exc_msg ? EXC_VT->exc_msg : "");
        exit(1);
    }
    __dragon_exc_type = (int)type;
    if (__dragon_exc_sp >= 0) {
        int sp = __dragon_exc_sp;
        longjmp(__dragon_exc_stack[sp], (int)type);
    }
    fprintf(stderr, "Unhandled exception: %s\n",
            __dragon_exc_msg ? __dragon_exc_msg : "");
    exit(1);
}

/// Raise with an integer type code. `msg` is borrowed (slot dups mortal heap strings).
/// Clears any stale exc_obj: built-in raises carry no instance.
void dragon_raise_exc(int64_t type, const char* msg) {
    dragon_raise_exc_impl(type, NULL, msg, 0);
}

/// Raise consuming an owned +1 message (codegen's concat / str() / f-string
/// temporaries). The slot takes the reference instead of dup'ing it.
void dragon_raise_exc_consume(int64_t type, const char* msg) {
    dragon_raise_exc_impl(type, NULL, msg, 1);
}

/// Raise with a raw C string message: copies it into a fresh heap DragonString and transfers
/// that +1 into the slot, so the slot never holds a raw C pointer. Raises are cold, so the copy is noise.
void dragon_raise_exc_cstr(int64_t type, const char* msg) {
    dragon_raise_exc_impl(type, NULL, msg ? dragon_string_dup_cstr(msg) : NULL, 1);
}

/// Raise MemoryError WITHOUT allocating: routing through _cstr's dragon_xmalloc dup would re-enter
/// the failing allocator and stack-overflow (A/B-proven 3700-frame SIGSEGV; pinned by oom_sustained.dr).
void dragon_raise_oom(void) {
    dragon_raise_exc_impl(43, NULL, "MemoryError: out of memory", 0);
}

/// Raise a typed exception with an attached user-class instance: `obj` transfers a +1 into
/// exc_obj (released on the next overwrite or uncaught path); `msg` is a best-effort string rendering.
void dragon_raise_exc_obj(int64_t type, void* obj, const char* msg) {
    dragon_raise_exc_impl(type, obj, msg, 0);
}

/// Obj-raise consuming an owned +1 message; used by the deferred re-raise after `finally`,
/// whose saved message and instance were retained at record time.
void dragon_raise_exc_obj_consume(int64_t type, void* obj, const char* msg) {
    dragon_raise_exc_impl(type, obj, msg, 1);
}

// Lets a longjmp-based unwind free the owned heap locals it skips over (see DragonCleanupStack
// in runtime_internal.h). Entry points are dual-path so green threads on a worker keep independent stacks.

static inline DragonCleanupStack* dragon_cleanup_active_stack() {
    return EXC_VT ? &EXC_VT->cleanup : &__dragon_cleanup;
}
static inline int32_t* dragon_cleanup_active_saved() {
    return EXC_VT ? EXC_VT->cleanup_saved : __dragon_cleanup_saved;
}
static inline int dragon_cleanup_active_exc_sp() {
    return EXC_VT ? EXC_VT->exc_sp : __dragon_exc_sp;
}

static void dragon_cleanup_grow(DragonCleanupStack* cs) {
    int32_t newcap = cs->cap ? cs->cap * 2 : 64;
    // Abort (not raise) on OOM: this IS the exception machinery, so raising would re-enter the
    // unwind mid-failure. xrealloc_or_abort aborts before NULL can land in the field on failure.
    cs->vals  = (int64_t*) dragon_xrealloc_or_abort(cs->vals,  (size_t)newcap * sizeof(int64_t));
    cs->kinds = (int32_t*) dragon_xrealloc_or_abort(cs->kinds, (size_t)newcap * sizeof(int32_t));
    cs->tags  = (int32_t*) dragon_xrealloc_or_abort(cs->tags,  (size_t)newcap * sizeof(int32_t));
    cs->cap = newcap;
}

/// Register an owned heap local for unwind cleanup; returns the slot index for later
/// dragon_cleanup_update. Only emitted by codegen for owned heap kinds, never a hot numeric loop.
int32_t dragon_cleanup_push(int64_t val, int32_t kind, int32_t tag) {
    DragonCleanupStack* cs = dragon_cleanup_active_stack();
    if (cs->sp >= cs->cap) dragon_cleanup_grow(cs);
    int32_t slot = cs->sp;
    cs->vals[slot]  = val;
    cs->kinds[slot] = kind;
    cs->tags[slot]  = tag;
    cs->sp = slot + 1;
    return slot;
}

/// Refresh a registered local's snapshot after reassignment so unwind frees the CURRENT value
/// (the old one was already decref'd). slot < 0 means never pushed - no-op.
void dragon_cleanup_update(int32_t slot, int64_t val, int32_t tag) {
    if (slot < 0) return;
    DragonCleanupStack* cs = dragon_cleanup_active_stack();
    if (slot < cs->sp) {
        cs->vals[slot] = val;
        cs->tags[slot] = tag;
    }
}

/// Current cleanup depth - codegen captures this at a scope's first push so the
/// matching scope exit can rewind to it.
int32_t dragon_cleanup_depth(void) {
    return dragon_cleanup_active_stack()->sp;
}

void dragon_exc_thread_state_release(void) {
    free(__dragon_cleanup.vals);
    free(__dragon_cleanup.kinds);
    free(__dragon_cleanup.tags);
    __dragon_cleanup.vals = NULL;
    __dragon_cleanup.kinds = NULL;
    __dragon_cleanup.tags = NULL;
    __dragon_cleanup.cap = 0;
    __dragon_cleanup.sp = 0;
    dragon_decref_str_dispatch(__dragon_exc_msg);
    __dragon_exc_msg = NULL;
    if (__dragon_exc_obj) dragon_decref_dispatch(__dragon_exc_obj);
    __dragon_exc_obj = NULL;
}

/// Rewind to `depth` WITHOUT freeing (emitScopeCleanupFor already decref'd these on the normal-exit
/// path); pops the snapshots so a later sibling exception can't re-free them.
void dragon_cleanup_reset(int32_t depth) {
    DragonCleanupStack* cs = dragon_cleanup_active_stack();
    if (depth >= 0 && depth <= cs->sp) cs->sp = depth;
}

/// Free every owned entry above `target` on `cs` and rewind to it. Shared by longjmp
/// arrival and by teardown of an abandoned context (generator / vthread), whose
/// registered locals were never scope-drained.
void dragon_cleanup_stack_drain(DragonCleanupStack* cs, int32_t target) {
    if (target < 0) target = 0;
    while (cs->sp > target) {
        int32_t i = --cs->sp;
        void* p = (void*)(uintptr_t)cs->vals[i];
        switch (cs->kinds[i]) {
            case DCLEAN_STR:      dragon_decref_str((const char*)p); break;
            case DCLEAN_CALLABLE: dragon_decref_callable(p); break;
            case DCLEAN_OBJ:      dragon_decref(p); break;
            case DCLEAN_DEFER_CALL: {
                // Pending defer: run the thunk over its snapshot values (still on the stack, so
                // every borrowed value is alive); the loop then pops and drains each entry normally.
                int32_t argc = cs->tags[i];
                if (argc >= 0 && i >= argc) {
                    void (*thunk)(int64_t*) = (void (*)(int64_t*))p;
                    thunk(&cs->vals[i - argc]);
                }
                break;
            }
            case DCLEAN_UNION: {
                int32_t tag = cs->tags[i];
                // Mirror emitUnionDecref: closure (10) before the generic heap
                // range (>=5) because TAG_CLOSURE also satisfies >=5.
                if (tag == TAG_STR) dragon_decref_str((const char*)p);
                else if (tag == DRAGON_TAG_CLOSURE) dragon_decref_callable(p);
                else if (tag >= TAG_LIST) dragon_decref(p);
                // else int/float/bool/none - non-heap, nothing to free
                break;
            }
            case DCLEAN_FREE:     free(p); break;
            default: break;
        }
    }
    cs->sp = target;
}

/// Free every owned heap local registered since the top exc frame was pushed (what a longjmp into
/// it skipped). Mirrors emitScopeCleanupFor; called at longjmp arrival BEFORE dragon_exc_pop_frame.
void dragon_exc_cleanup_unwind(void) {
    int sp_exc = dragon_cleanup_active_exc_sp();
    if (sp_exc < 0) return;  // malformed arrival - free nothing rather than guess
    // exc_obj now OWNS its +1, so an aliasing local is freed here safely. The old `keep_obj`
    // skip (built for the prior borrowed-slot contract) would leak one ref per raise in a retry loop.
    dragon_cleanup_stack_drain(dragon_cleanup_active_stack(),
                               dragon_cleanup_active_saved()[sp_exc]);
}

// Each fire trampoline pushes a setjmp frame; an uncaught raise inside longjmps here, which logs
// + clears the exception so the worker marks the vthread done, instead of exit(1) killing the whole process.

static const char* dragon_exc_name_for_code(int code) {
    switch (code) {
        case 0:   return "BaseException";
        case 1:   return "SystemExit";
        case 2:   return "KeyboardInterrupt";
        case 3:   return "GeneratorExit";
        case 10:  return "Exception";
        case 11:  return "StopIteration";
        case 20:  return "ArithmeticError";
        case 21:  return "FloatingPointError";
        case 22:  return "OverflowError";
        case 23:  return "ZeroDivisionError";
        case 24:  return "AssertionError";
        case 25:  return "AttributeError";
        case 26:  return "BufferError";
        case 27:  return "EOFError";
        case 30:  return "ImportError";
        case 31:  return "ModuleNotFoundError";
        case 40:  return "LookupError";
        case 41:  return "IndexError";
        case 42:  return "KeyError";
        case 43:  return "MemoryError";
        case 44:  return "NameError";
        case 45:  return "UnboundLocalError";
        case 50:  return "OSError";
        case 51:  return "FileNotFoundError";
        case 52:  return "FileExistsError";
        case 53:  return "IsADirectoryError";
        case 54:  return "NotADirectoryError";
        case 55:  return "PermissionError";
        case 56:  return "TimeoutError";
        case 57:  return "ConnectionError";
        case 58:  return "BrokenPipeError";
        case 59:  return "ConnectionAbortedError";
        case 60:  return "ConnectionRefusedError";
        case 61:  return "ConnectionResetError";
        case 70:  return "RuntimeError";
        case 71:  return "NotImplementedError";
        case 72:  return "RecursionError";
        case 73:  return "StopAsyncIteration";
        case 74:  return "SyntaxError";
        case 80:  return "TypeError";
        case 90:  return "ValueError";
        case 91:  return "UnicodeError";
        case 92:  return "UnicodeDecodeError";
        case 93:  return "UnicodeEncodeError";
        case 94:  return "UnicodeTranslateError";
        case 100: return "Warning";
        case 101: return "DeprecationWarning";
        case 102: return "FutureWarning";
        case 103: return "ResourceWarning";
        case 104: return "RuntimeWarning";
        case 105: return "UserWarning";
        default:  return code >= 1000 ? "UserException" : "Exception";
    }
}

/// Emitted by codegen as the longjmp-arrival branch atop each fire trampoline. Logs the in-flight
/// exception (vthread struct preferred, TLS fallback) and clears it for the next vthread.
void dragon_vthread_log_uncaught() {
    int code = 0;
    const char* msg = NULL;
    void* obj = NULL;
    if (EXC_VT) {
        code = EXC_VT->exc_type;
        msg = EXC_VT->exc_msg;
        obj = EXC_VT->exc_obj;
        EXC_VT->exc_type = 0;
        EXC_VT->exc_msg = NULL;
        EXC_VT->exc_obj = NULL;
    } else {
        code = __dragon_exc_type;
        msg = __dragon_exc_msg;
        obj = __dragon_exc_obj;
        __dragon_exc_type = 0;
        __dragon_exc_msg = NULL;
        __dragon_exc_obj = NULL;
    }
    fprintf(stderr, "vthread terminated by uncaught %s: %s\n",
            dragon_exc_name_for_code(code), msg ? msg : "");
    fflush(stderr);
    // Slot owned the message (dragon_exc_msg_set); release it now that it's logged and cleared.
    dragon_decref_str_dispatch(msg);
    // Typed raises carry a +1 that unwind cleanup skips (keep_obj) for a matching handler to take;
    // uncaught in a fired vthread there's no handler, so release it here or it leaks (unbounded RSS).
    if (obj) dragon_decref_dispatch(obj);
}

// Built-in exception ranges: {code, hi} for parent exceptions
static struct { int64_t code; int64_t hi; } __builtin_exc_ranges[] = {
    {0, 105},    // BaseException
    {10, 105},   // Exception
    {20, 23},    // ArithmeticError
    {30, 31},    // ImportError
    {40, 42},    // LookupError
    {44, 45},    // NameError
    {50, 61},    // OSError
    {57, 61},    // ConnectionError
    {70, 72},    // RuntimeError
    {90, 94},    // ValueError
    {91, 94},    // UnicodeError
    {100, 105},  // Warning
};
#define NUM_BUILTIN_RANGES (sizeof(__builtin_exc_ranges) / sizeof(__builtin_exc_ranges[0]))

// User-defined exception parent table (codes 1000+)
static int64_t __dragon_user_exc_parents[DRAGON_EXC_MAX_USER];
static int     __dragon_user_exc_count = 0;

/// Register a user-defined exception with its parent code
void dragon_exc_register(int64_t code, int64_t parent_code) {
    int idx = (int)(code - 1000);
    if (idx >= 0 && idx < DRAGON_EXC_MAX_USER) {
        __dragon_user_exc_parents[idx] = parent_code;
        if (idx >= __dragon_user_exc_count)
            __dragon_user_exc_count = idx + 1;
    }
}

/// Check if raised exception type matches a caught exception type.
/// Handles both built-in (range-based) and user-defined (parent chain walk).
int64_t dragon_exc_matches(int64_t raised, int64_t caught) {
    int64_t current = raised;
    int maxDepth = 50;

    while (maxDepth-- > 0) {
        // Exact match
        if (current == caught) return 1;

        if (current >= 1000) {
            // User-defined: walk parent chain
            int idx = (int)(current - 1000);
            if (idx < __dragon_user_exc_count)
                current = __dragon_user_exc_parents[idx];
            else
                break;
        } else {
            // Built-in: check if caught is a parent via range table
            for (int i = 0; i < (int)NUM_BUILTIN_RANGES; i++) {
                if (__builtin_exc_ranges[i].code == caught) {
                    return (current >= caught &&
                            current <= __builtin_exc_ranges[i].hi) ? 1 : 0;
                }
            }
            return 0; // caught is a leaf, and current != caught
        }
    }
    return 0;
}

} // extern "C"
