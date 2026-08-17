/* dpyes-ext — external-UI reimplementation of DPYes DPS meters for Grim Dawn.
 *
 * DPS behavior aligned with DPYes 18p:
 *   - CombatManager::ApplyDamage captures attacker/target ownership context.
 *   - One selected damage stream enters the meters: raw OR post-mitigation.
 *   - Four independent meters: player dealt, player incoming, player pets, other.
 *   - Ten 500 ms buckets, 64 damage-type slots, a rolling five-second window.
 *   - DPS divisor is the active event span, clamped to at least one second.
 *   - Running totals and a retained best-average snapshot are kept per meter.
 *
 * The presentation remains an external Win32 window rather than DPYes's ImGui
 * overlays, but the damage collection and aggregation semantics follow DPYes.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <MinHook.h>

#define LOG_PREFIX "GDEXT: "
#define LOG_FILE ("\\\\?\\D:\\Game\\grimdawn.Build.24346246\\dpyes-ext\\dpyes_ext.log")

#if defined(_WIN64) || defined(__x86_64__)
#define GAME_ARCH_NAME          "x64"
#define GAME_MEMBER_CALL
#define COMBAT_TARGET_OFFSET    8
#define SYM_GetMainPlayer       "?GetMainPlayer@GameEngine@GAME@@QEBAPEAVPlayer@2@XZ"
#define SYM_GetCurrentLife      "?GetCurrentLife@Character@GAME@@QEBA?BNXZ"
#define SYM_GetExperiencePoints "?GetExperiencePoints@Character@GAME@@QEBA?BIXZ"
#define SYM_ApplyDamageCM       "?ApplyDamage@CombatManager@GAME@@QEAA_NMAEBUPlayStatsDamageType@2@W4CombatAttributeType@2@AEBV?$vector@I@mem@@@Z"
#define SYM_SubtractLife        "?SubtractLife@Character@GAME@@QEAAXMAEBUPlayStatsDamageType@2@_N_N@Z"
#define SYM_GetAttackerIdCM     "?GetAttackerId@CombatManager@GAME@@QEBA?BIXZ"
#define SYM_GetObjectId         "?GetObjectId@Object@GAME@@QEBAIXZ"
#define SYM_GetMasterAttacker   "?GetMasterAttacker@GameEngine@GAME@@QEBAII@Z"
#else
#define GAME_ARCH_NAME          "x86"
#define GAME_MEMBER_CALL        __thiscall
#define COMBAT_TARGET_OFFSET    4
#define SYM_GetMainPlayer       "?GetMainPlayer@GameEngine@GAME@@QBEPAVPlayer@2@XZ"
#define SYM_GetCurrentLife      "?GetCurrentLife@Character@GAME@@QBE?BNXZ"
#define SYM_GetExperiencePoints "?GetExperiencePoints@Character@GAME@@QBE?BIXZ"
#define SYM_ApplyDamageCM       "?ApplyDamage@CombatManager@GAME@@QAE_NMABUPlayStatsDamageType@2@W4CombatAttributeType@2@ABV?$vector@I@mem@@@Z"
#define SYM_SubtractLife        "?SubtractLife@Character@GAME@@QAEXMABUPlayStatsDamageType@2@_N_N@Z"
#define SYM_GetAttackerIdCM     "?GetAttackerId@CombatManager@GAME@@QBE?BIXZ"
#define SYM_GetObjectId         "?GetObjectId@Object@GAME@@QBEIXZ"
#define SYM_GetMasterAttacker   "?GetMasterAttacker@GameEngine@GAME@@QBEII@Z"
#endif

#define DPS_TYPE_COUNT       64
#define DPS_BUCKET_COUNT     10
#define DPS_BUCKET_MS        500u
#define DPS_WINDOW_MS        5000u
#define DPS_MIN_DIVISOR_MS   1000u
#define BEST_MIN_SPAN_MS     2500u
#define BEST_RETENTION_MS    120000u
#define DAMAGE_CONTEXT_DEPTH 8
#define EVTLOG_N             300

#define HOTKEY_TOGGLE 1
#define TOGGLE_VK     VK_INSERT
#define UI_TIMER_MS   500

/* The old per-hit list on the right is intentionally compiled out for now.
 * Set this back to 1 to restore its controls and refresh code. */
#define ENABLE_DAMAGE_LOG_UI 0

#define IDC_STATS       200
#define IDC_RESET       300
#define IDC_MITIGATED   301
#define IDC_TRUE_ENV    302
#define IDC_EVTLOG      400

static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_dps_cs;

static void log_msg(const char *msg) {
    EnterCriticalSection(&g_log_cs);
    OutputDebugStringA(LOG_PREFIX);
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    {
        HANDLE f = CreateFileA(LOG_FILE, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char line[1024];
            int n = wsprintfA(line, "%s%s\r\n", LOG_PREFIX, msg);
            DWORD wr;
            WriteFile(f, line, (DWORD)n, &wr, NULL);
            CloseHandle(f);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

/* ---------------- Game.dll functions and captured objects ---------------- */
typedef void* (GAME_MEMBER_CALL *GetMainPlayer_t)(void *self);
typedef double (GAME_MEMBER_CALL *GetDouble_t)(void *self);
typedef unsigned (GAME_MEMBER_CALL *GetUInt_t)(void *self);
typedef unsigned (GAME_MEMBER_CALL *GetMasterAttacker_t)(void *engine,
    unsigned objectId);
typedef unsigned char (GAME_MEMBER_CALL *ApplyDamageCM_t)(void *self, float damage,
    const void *dmgType, int attr, const void *vec);
typedef void (GAME_MEMBER_CALL *SubtractLife_t)(void *self, float damage,
    const void *dmgType, unsigned char a, unsigned char b);

static GetMainPlayer_t g_OrigGetMainPlayer;
static ApplyDamageCM_t g_OrigApplyDamageCM;
static SubtractLife_t g_OrigSubtractLife;
static GetUInt_t g_fnGetAttackerIdCM;
static GetUInt_t g_fnGetObjectId;
static GetMasterAttacker_t g_fnGetMasterAttacker;
static GetDouble_t g_fnLife;
static GetUInt_t g_fnXP;

static void * volatile g_engine;
static void * volatile g_player;
static volatile LONG g_player_object_id;
static volatile LONG g_use_mitigated = 1; /* DPYes 18p default: enabled. */
static volatile LONG g_show_true_environment = 0;
static volatile LONG g_classification_logs;
static int g_identity_symbols_ready;

/* ---------------- DPYes-style meter state ---------------- */
typedef enum MeterKind {
    METER_PLAYER_DEALT = 0,
    METER_PLAYER_INCOMING,
    METER_PET_DEALT,
    METER_OTHER,
    METER_COUNT
} MeterKind;

typedef struct DpsBucket {
    DWORD bucket_ms;
    DWORD first_event_ms;
    DWORD last_event_ms;
    float damage[DPS_TYPE_COUNT];
    int valid;
} DpsBucket;

typedef struct DpsMeter {
    DpsBucket buckets[DPS_BUCKET_COUNT];
    unsigned bucket_index;
    double type_total[DPS_TYPE_COUNT];
    double total_damage;
    unsigned event_count;

    float best_dps[DPS_TYPE_COUNT];
    float best_total_dps;
    DWORD best_at_ms;
} DpsMeter;

typedef struct MeterView {
    float dps[DPS_TYPE_COUNT];
    float best_dps[DPS_TYPE_COUNT];
    double type_total[DPS_TYPE_COUNT];
    float total_dps;
    float best_total_dps;
    double total_damage;
    DWORD active_span_ms;
    unsigned event_count;
} MeterView;

static DpsMeter g_meters[METER_COUNT];
static unsigned g_event_count;

/* DPYes's 64-entry display order at DPYes.dll 0x180255430. */
static const unsigned char g_type_order[DPS_TYPE_COUNT] = {
     6,  5,  8,  7,  4, 15,  9, 11, 10,  2,  1,  3, 14, 57, 12, 13,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 55, 56, 58, 59, 60, 62, 54, 61, 63,  0
};

static wchar_t g_unknown_type_names[DPS_TYPE_COUNT][16];

static const wchar_t *damage_type_wname(unsigned type) {
    switch (type) {
    case 0:  return L"环境";
    case 1:  return L"穿刺";
    case 2:  return L"物理";
    case 3:  return L"火焰";
    case 4:  return L"冰冷";
    case 5:  return L"闪电";
    case 6:  return L"活力";
    case 7:  return L"虚化";
    case 8:  return L"混乱";
    case 9:  return L"酸性";
    case 10: return L"创伤";
    case 11: return L"生命偷取";
    case 12: return L"霜燃";
    case 13: return L"燃烧";
    case 14: return L"电击";
    case 15: return L"流血";
    default:
        if (type >= DPS_TYPE_COUNT) type = DPS_TYPE_COUNT - 1;
        return g_unknown_type_names[type];
    }
}

static const wchar_t *meter_wname(MeterKind kind) {
    switch (kind) {
    case METER_PLAYER_DEALT:    return L"玩家输出 / Dealt damage";
    case METER_PLAYER_INCOMING: return L"玩家承伤 / Incoming damage";
    case METER_PET_DEALT:       return L"宠物输出 / Pet damage";
    default:                    return L"其他伤害 / Other damage";
    }
}

static const wchar_t *meter_short_wname(MeterKind kind) {
    switch (kind) {
    case METER_PLAYER_DEALT:    return L"玩家";
    case METER_PLAYER_INCOMING: return L"承伤";
    case METER_PET_DEALT:       return L"宠物";
    default:                    return L"其他";
    }
}

static void reset_stats_locked(void) {
    unsigned i;
    memset(g_meters, 0, sizeof(g_meters));
    for (i = 0; i < METER_COUNT; ++i)
        g_meters[i].bucket_index = DPS_BUCKET_COUNT - 1;
    g_event_count = 0;
}

static void reset_stats(void) {
    EnterCriticalSection(&g_dps_cs);
    reset_stats_locked();
    LeaveCriticalSection(&g_dps_cs);
}

static void meter_record_locked(DpsMeter *meter, DWORD now,
    float damage, unsigned type) {
    DWORD aligned = now - (now % DPS_BUCKET_MS);
    DpsBucket *bucket = &meter->buckets[meter->bucket_index];

    if (!bucket->valid || bucket->bucket_ms != aligned) {
        meter->bucket_index = (meter->bucket_index + 1) % DPS_BUCKET_COUNT;
        bucket = &meter->buckets[meter->bucket_index];
        memset(bucket, 0, sizeof(*bucket));
        bucket->valid = 1;
        bucket->bucket_ms = aligned;
        bucket->first_event_ms = now;
    }

    bucket->last_event_ms = now;
    bucket->damage[type] += damage;
    meter->type_total[type] += damage;
    meter->total_damage += damage;
    meter->event_count++;
}

static void meter_make_view_locked(DpsMeter *meter, DWORD now,
    MeterView *view) {
    unsigned i, t;
    DWORD oldest_age = 0;
    DWORD newest_age = 0xFFFFFFFFu;
    int have_event = 0;
    DWORD actual_span = 0;
    DWORD divisor_ms;

    memset(view, 0, sizeof(*view));
    for (t = 0; t < DPS_TYPE_COUNT; ++t)
        view->type_total[t] = meter->type_total[t];
    view->total_damage = meter->total_damage;
    view->event_count = meter->event_count;

    for (i = 0; i < DPS_BUCKET_COUNT; ++i) {
        DpsBucket *bucket = &meter->buckets[i];
        DWORD bucket_age;
        if (!bucket->valid) continue;
        bucket_age = now - bucket->bucket_ms;
        if (bucket_age > DPS_WINDOW_MS) continue;

        for (t = 0; t < DPS_TYPE_COUNT; ++t)
            view->dps[t] += bucket->damage[t];

        {
            DWORD first_age = now - bucket->first_event_ms;
            DWORD last_age = now - bucket->last_event_ms;
            if (!have_event || first_age > oldest_age) oldest_age = first_age;
            if (!have_event || last_age < newest_age) newest_age = last_age;
            have_event = 1;
        }
    }

    if (have_event && oldest_age >= newest_age)
        actual_span = oldest_age - newest_age;
    divisor_ms = actual_span < DPS_MIN_DIVISOR_MS
        ? DPS_MIN_DIVISOR_MS : actual_span;
    view->active_span_ms = actual_span;

    if (have_event) {
        float seconds = (float)divisor_ms / 1000.0f;
        for (t = 0; t < DPS_TYPE_COUNT; ++t) {
            view->dps[t] /= seconds;
            view->total_dps += view->dps[t];
        }
    }

    if (meter->best_at_ms && now - meter->best_at_ms > BEST_RETENTION_MS) {
        memset(meter->best_dps, 0, sizeof(meter->best_dps));
        meter->best_total_dps = 0;
        meter->best_at_ms = 0;
    }

    if (have_event && actual_span >= BEST_MIN_SPAN_MS &&
        view->total_dps > meter->best_total_dps) {
        memcpy(meter->best_dps, view->dps, sizeof(meter->best_dps));
        meter->best_total_dps = view->total_dps;
        meter->best_at_ms = now;
    }

    memcpy(view->best_dps, meter->best_dps, sizeof(view->best_dps));
    view->best_total_dps = meter->best_total_dps;
}

/* ---------------- Selected-stream event log ---------------- */
static wchar_t g_evtlog[EVTLOG_N][192];
static unsigned g_evtlog_head;
static unsigned g_evtlog_count;
static unsigned g_debug_event_logs;

static void clear_event_log(void) {
    EnterCriticalSection(&g_dps_cs);
    g_evtlog_head = 0;
    g_evtlog_count = 0;
    LeaveCriticalSection(&g_dps_cs);
}

static void record_selected_damage(MeterKind kind, float damage,
    unsigned type) {
    DWORD now;
    unsigned event_number;
    int write_debug_log;

    if (!(damage > 0.0f)) return;
    if (type >= DPS_TYPE_COUNT) type = DPS_TYPE_COUNT - 1;
    now = GetTickCount();

    EnterCriticalSection(&g_dps_cs);
    meter_record_locked(&g_meters[kind], now, damage, type);
    event_number = ++g_event_count;
    {
        wchar_t *slot = g_evtlog[g_evtlog_head];
        _snwprintf(slot, 192, L"[%-4s] %-7s %10.1f",
            meter_short_wname(kind), damage_type_wname(type), damage);
        slot[191] = L'\0';
        g_evtlog_head = (g_evtlog_head + 1) % EVTLOG_N;
        if (g_evtlog_count < EVTLOG_N) g_evtlog_count++;
    }
    write_debug_log = g_debug_event_logs < 40;
    if (write_debug_log) g_debug_event_logs++;
    LeaveCriticalSection(&g_dps_cs);

    if (write_debug_log) {
        char line[192];
        _snprintf(line, sizeof(line),
            "DPS#%u meter=%u dmg=%.1f type=%u stream=%s",
            event_number, (unsigned)kind, damage, type,
            InterlockedCompareExchange(&g_use_mitigated, 0, 0)
                ? "mitigated" : "raw");
        line[sizeof(line) - 1] = '\0';
        log_msg(line);
    }
}

/* ---------------- ApplyDamage -> SubtractLife context ---------------- */
typedef struct DamageContext {
    void *target;
    MeterKind meter;
    unsigned damage_type;
    unsigned attacker_id;
    unsigned target_object_id;
    unsigned attacker_master;
    unsigned target_master;
    int mitigated;
    int valid;
} DamageContext;

typedef struct ThreadDamageContexts {
    DamageContext stack[DAMAGE_CONTEXT_DEPTH];
    unsigned depth;
} ThreadDamageContexts;

static DWORD g_context_tls = TLS_OUT_OF_INDEXES;

static ThreadDamageContexts *thread_contexts(int create) {
    ThreadDamageContexts *contexts;
    if (g_context_tls == TLS_OUT_OF_INDEXES) return NULL;
    contexts = (ThreadDamageContexts *)TlsGetValue(g_context_tls);
    if (!contexts && create) {
        contexts = (ThreadDamageContexts *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, sizeof(*contexts));
        if (contexts && !TlsSetValue(g_context_tls, contexts)) {
            HeapFree(GetProcessHeap(), 0, contexts);
            contexts = NULL;
        }
    }
    return contexts;
}

static unsigned read_damage_type(const void *dmgType, unsigned attr,
    int show_true_environment) {
    unsigned type = 0;
    if (dmgType)
        type = *(const unsigned *)((const char *)dmgType + 4);
    type &= 0x7FFFFFFFu;

    /* DPYes normally uses PlayStatsDamageType+4. For environment (zero),
     * it uses CombatAttributeType unless true-environment display is enabled. */
    if (type == 0 && !show_true_environment)
        type = attr & 0x7FFFFFFFu;
    if (type >= DPS_TYPE_COUNT)
        type = DPS_TYPE_COUNT - 1;
    return type;
}

static MeterKind classify_damage(void *target, unsigned attacker_id,
    unsigned target_object_id, unsigned attacker_master,
    unsigned target_master) {
    unsigned player_object_id =
        (unsigned)InterlockedCompareExchange(&g_player_object_id, 0, 0);

    /* DPYes compares CombatManager::GetAttackerId() against
     * Object::GetObjectId(mainPlayer), not Character::GetControllerId().
     * GetMasterAttacker() also consumes these object IDs. */
    if (player_object_id) {
        if (attacker_id == player_object_id)
            return METER_PLAYER_DEALT;
        if (target_object_id == player_object_id)
            return METER_PLAYER_INCOMING;
        if (attacker_id != player_object_id &&
            attacker_master == player_object_id)
            return METER_PET_DEALT;
        if (target_object_id != player_object_id &&
            target_master == player_object_id)
            return METER_COUNT; /* DPYes excludes damage dealt to player's pets. */
    } else if (target && target == (void *)g_player) {
        return METER_PLAYER_INCOMING;
    }

    return METER_OTHER;
}

static DamageContext make_damage_context(void *combatManager,
    const void *dmgType, unsigned attr) {
    DamageContext ctx;
    void *target = NULL;
    unsigned player_object_id = 0;
    LONG classification_log_number;
    int show_true_environment = (int)InterlockedCompareExchange(
        &g_show_true_environment, 0, 0);

    memset(&ctx, 0, sizeof(ctx));
    if (combatManager)
        target = *(void **)((char *)combatManager + COMBAT_TARGET_OFFSET);
    ctx.target = target;
    ctx.mitigated = (int)InterlockedCompareExchange(&g_use_mitigated, 0, 0);
    ctx.damage_type = read_damage_type(dmgType, attr, show_true_environment);

    if (g_fnGetObjectId && g_player) {
        player_object_id = g_fnGetObjectId((void *)g_player);
        InterlockedExchange(&g_player_object_id, (LONG)player_object_id);
    }
    if (g_fnGetAttackerIdCM)
        ctx.attacker_id = g_fnGetAttackerIdCM(combatManager);
    if (g_fnGetObjectId && target)
        ctx.target_object_id = g_fnGetObjectId(target);
    if (g_fnGetMasterAttacker && g_engine) {
        ctx.attacker_master = g_fnGetMasterAttacker(
            (void *)g_engine, ctx.attacker_id);
        ctx.target_master = g_fnGetMasterAttacker(
            (void *)g_engine, ctx.target_object_id);
    }

    ctx.meter = classify_damage(target, ctx.attacker_id,
        ctx.target_object_id, ctx.attacker_master, ctx.target_master);
    /* Ignore loading-screen/world events until the local player identity exists. */
    ctx.valid = g_player && ctx.meter < METER_COUNT;

    /* Keep a small amount of identity diagnostics. This makes any remaining
     * version-specific ID mismatch visible without flooding the log forever. */
    classification_log_number = g_player
        ? InterlockedIncrement(&g_classification_logs) : 41;
    if (classification_log_number <= 40) {
        char line[256];
        _snprintf(line, sizeof(line),
            "CLASS#%ld playerObj=%u attacker=%u targetObj=%u "
            "attackerMaster=%u targetMaster=%u meter=%u target=%p player=%p",
            classification_log_number, player_object_id, ctx.attacker_id,
            ctx.target_object_id, ctx.attacker_master, ctx.target_master,
            (unsigned)ctx.meter, target, (void *)g_player);
        line[sizeof(line) - 1] = '\0';
        log_msg(line);
    }
    return ctx;
}

static DamageContext *push_damage_context(DamageContext ctx) {
    ThreadDamageContexts *contexts = thread_contexts(1);
    DamageContext *slot;
    if (!contexts) return NULL;
    if (contexts->depth < DAMAGE_CONTEXT_DEPTH) {
        slot = &contexts->stack[contexts->depth++];
    } else {
        /* Preserve the most recent contexts if pathological nesting occurs. */
        memmove(&contexts->stack[0], &contexts->stack[1],
            sizeof(contexts->stack[0]) * (DAMAGE_CONTEXT_DEPTH - 1));
        slot = &contexts->stack[DAMAGE_CONTEXT_DEPTH - 1];
    }
    *slot = ctx;
    return slot;
}

static void pop_damage_context(void) {
    ThreadDamageContexts *contexts = thread_contexts(0);
    if (contexts && contexts->depth)
        --contexts->depth;
}

static DamageContext *current_damage_context(void) {
    ThreadDamageContexts *contexts = thread_contexts(0);
    if (!contexts || !contexts->depth) return NULL;
    return &contexts->stack[contexts->depth - 1];
}

#if defined(_WIN64) || defined(__x86_64__)
static void *HK_GetMainPlayer(void *self) {
#else
static void *__fastcall HK_GetMainPlayer(void *self, void *unused_edx) {
    (void)unused_edx;
#endif
    void *player = g_OrigGetMainPlayer ? g_OrigGetMainPlayer(self) : NULL;
    g_engine = self;
    if (player) {
        g_player = player;
        if (g_fnGetObjectId)
            InterlockedExchange(&g_player_object_id,
                (LONG)g_fnGetObjectId(player));
    }
    return player;
}

#if defined(_WIN64) || defined(__x86_64__)
static unsigned char HK_ApplyDamageCM(void *self, float damage,
    const void *dmgType, int attr, const void *vec) {
#else
static unsigned char __fastcall HK_ApplyDamageCM(void *self, void *unused_edx,
    float damage, const void *dmgType, int attr, const void *vec) {
    (void)unused_edx;
#endif
    DamageContext ctx = make_damage_context(self, dmgType, (unsigned)attr);
    unsigned char result;
    push_damage_context(ctx);

    if (ctx.valid && !ctx.mitigated)
        record_selected_damage(ctx.meter, damage, ctx.damage_type);

    result = g_OrigApplyDamageCM(self, damage, dmgType, attr, vec);
    pop_damage_context();
    return result;
}

#if defined(_WIN64) || defined(__x86_64__)
static void HK_SubtractLife(void *self, float damage, const void *dmgType,
    unsigned char a, unsigned char b) {
#else
static void __fastcall HK_SubtractLife(void *self, void *unused_edx,
    float damage, const void *dmgType, unsigned char a, unsigned char b) {
    (void)unused_edx;
#endif
    DamageContext *ctx = current_damage_context();
    static volatile LONG mismatch_logs;

    (void)dmgType; /* DPYes uses the context captured by ApplyDamage. */
    if (ctx && ctx->valid && ctx->mitigated) {
        if (ctx->target == self) {
            record_selected_damage(ctx->meter, damage, ctx->damage_type);
        } else if (InterlockedIncrement(&mismatch_logs) <= 10) {
            char line[160];
            _snprintf(line, sizeof(line),
                "Unexpected attacked character: %p instead of %p", self, ctx->target);
            line[sizeof(line) - 1] = '\0';
            log_msg(line);
        }
    }
    g_OrigSubtractLife(self, damage, dmgType, a, b);
}

/* ---------------- External UI ---------------- */
static void add_list_line(HWND list, const wchar_t *line) {
    SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line);
}

static void add_meter_lines(HWND list, MeterKind kind, const MeterView *view) {
    wchar_t line[256];
    unsigned i;
    int any = 0;

    _snwprintf(line, 256, L"―― %s ――", meter_wname(kind));
    line[255] = L'\0';
    add_list_line(list, line);
    add_list_line(list, L"  类型             DPS       Best        Total");

    for (i = 0; i < DPS_TYPE_COUNT; ++i) {
        unsigned type = g_type_order[i];
        if (view->dps[type] <= 0.05f && view->best_dps[type] <= 0.05f &&
            view->type_total[type] <= 0.5)
            continue;
        any = 1;
        _snwprintf(line, 256, L"  %-7s %10.0f %10.0f %12.0f",
            damage_type_wname(type), view->dps[type], view->best_dps[type],
            view->type_total[type]);
        line[255] = L'\0';
        add_list_line(list, line);
    }

    if (!any)
        add_list_line(list, L"  （暂无伤害）");
    _snwprintf(line, 256,
        L"  合计      %10.0f %10.0f %12.0f  [%u事件/跨度%.1fs]",
        view->total_dps, view->best_total_dps, view->total_damage,
        view->event_count, (double)view->active_span_ms / 1000.0);
    line[255] = L'\0';
    add_list_line(list, line);
    add_list_line(list, L"");
}

static void refresh_stats(HWND h) {
    HWND list = GetDlgItem(h, IDC_STATS);
#if ENABLE_DAMAGE_LOG_UI
    HWND elog = GetDlgItem(h, IDC_EVTLOG);
#endif
    MeterView views[METER_COUNT];
    DWORD now = GetTickCount();
    unsigned i;
    wchar_t line[256];

    if (!list) return;
    EnterCriticalSection(&g_dps_cs);
    for (i = 0; i < METER_COUNT; ++i)
        meter_make_view_locked(&g_meters[i], now, &views[i]);
    LeaveCriticalSection(&g_dps_cs);

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);

    if (!g_player || !g_fnLife) {
        add_list_line(list, L"等待角色载入…（Insert 显示/隐藏）");
    } else {
        double life = g_fnLife((void *)g_player);
        unsigned xp = g_fnXP ? g_fnXP((void *)g_player) : 0;
        _snwprintf(line, 256, L"血量 %.1f   经验 %u   当前口径：%s",
            life, xp,
            InterlockedCompareExchange(&g_use_mitigated, 0, 0)
                ? L"减伤后实际伤害" : L"减伤前 RAW");
        line[255] = L'\0';
        add_list_line(list, line);
        if (!g_identity_symbols_ready)
            add_list_line(list, L"警告：身份分类符号未完整解析，Other 分类可能增多");
        add_list_line(list, L"");
        for (i = 0; i < METER_COUNT; ++i)
            add_meter_lines(list, (MeterKind)i, &views[i]);
    }

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);

#if ENABLE_DAMAGE_LOG_UI
    /* Right-side per-hit damage log UI (temporarily disabled). */
    if (elog) {
        SendMessageW(elog, WM_SETREDRAW, FALSE, 0);
        SendMessageW(elog, LB_RESETCONTENT, 0, 0);
        EnterCriticalSection(&g_dps_cs);
        {
            unsigned show = g_evtlog_count > 100 ? 100 : g_evtlog_count;
            for (i = 0; i < show; ++i) {
                unsigned idx = (g_evtlog_head + EVTLOG_N - show + i) % EVTLOG_N;
                SendMessageW(elog, LB_ADDSTRING, 0, (LPARAM)g_evtlog[idx]);
            }
        }
        LeaveCriticalSection(&g_dps_cs);
        SendMessageW(elog, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(elog, NULL, TRUE);
    }
#endif
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)l;
    switch (m) {
    case WM_TIMER:
        if (w == 1) refresh_stats(h);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_RESET:
            reset_stats();
            clear_event_log();
            log_msg("DPS meters reset");
            return 0;
        case IDC_MITIGATED:
            if (HIWORD(w) == BN_CLICKED) {
                int checked = SendMessageW(GetDlgItem(h, IDC_MITIGATED),
                    BM_GETCHECK, 0, 0) == BST_CHECKED;
                InterlockedExchange(&g_use_mitigated, checked ? 1 : 0);
                reset_stats(); /* Never mix RAW and mitigated totals. */
                clear_event_log();
                log_msg(checked
                    ? "damage stream changed: mitigated"
                    : "damage stream changed: raw");
            }
            return 0;
        case IDC_TRUE_ENV:
            if (HIWORD(w) == BN_CLICKED) {
                int checked = SendMessageW(GetDlgItem(h, IDC_TRUE_ENV),
                    BM_GETCHECK, 0, 0) == BST_CHECKED;
                InterlockedExchange(&g_show_true_environment, checked ? 1 : 0);
                reset_stats();
                clear_event_log();
                log_msg(checked
                    ? "environment type display: true environment"
                    : "environment type display: combat attribute");
            }
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (w == HOTKEY_TOGGLE) {
            BOOL visible = IsWindowVisible(h);
            ShowWindow(h, visible ? SW_HIDE : SW_SHOW);
            if (!visible) SetForegroundWindow(h);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ui_run(HINSTANCE hInst) {
    WNDCLASSW wc;
    HWND h, list, reset, mitigated, true_env;
#if ENABLE_DAMAGE_LOG_UI
    HWND elog, label;
#endif
    HFONT font;
    int shown_once = 0;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DPYesExtUI";
    RegisterClassW(&wc);

    h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
        L"dpyes-ext — DPYes-aligned DPS", WS_OVERLAPPEDWINDOW,
#if ENABLE_DAMAGE_LOG_UI
        40, 40, 980, 650, NULL, NULL, hInst, NULL);
#else
        40, 40, 620, 650, NULL, NULL, hInst, NULL);
#endif
    list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        8, 8, 580, 550, h, (HMENU)IDC_STATS, hInst, NULL);
#if ENABLE_DAMAGE_LOG_UI
    /* Right-side per-hit damage log UI (temporarily disabled). */
    elog = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        596, 8, 360, 550, h, (HMENU)IDC_EVTLOG, hInst, NULL);
#endif
    reset = CreateWindowExW(0, L"BUTTON", L"重置 DPS",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        8, 566, 100, 28, h, (HMENU)IDC_RESET, hInst, NULL);
    mitigated = CreateWindowExW(0, L"BUTTON", L"减伤后实际伤害",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        120, 568, 145, 24, h, (HMENU)IDC_MITIGATED, hInst, NULL);
    true_env = CreateWindowExW(0, L"BUTTON", L"显示真实环境类型",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        275, 568, 155, 24, h, (HMENU)IDC_TRUE_ENV, hInst, NULL);
#if ENABLE_DAMAGE_LOG_UI
    label = CreateWindowExW(0, L"STATIC",
        L"右侧：已进入 DPS meter 的单一口径事件（Insert 显示/隐藏）",
        WS_CHILD | WS_VISIBLE,
        596, 570, 360, 22, h, (HMENU)401, hInst, NULL);
#endif

    SendMessageW(mitigated, BM_SETCHECK,
        InterlockedCompareExchange(&g_use_mitigated, 0, 0)
            ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(true_env, BM_SETCHECK,
        InterlockedCompareExchange(&g_show_true_environment, 0, 0)
            ? BST_CHECKED : BST_UNCHECKED, 0);

    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (font) {
        SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
#if ENABLE_DAMAGE_LOG_UI
        SendMessageW(elog, WM_SETFONT, (WPARAM)font, TRUE);
#endif
        SendMessageW(reset, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(mitigated, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(true_env, WM_SETFONT, (WPARAM)font, TRUE);
#if ENABLE_DAMAGE_LOG_UI
        SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
#endif
    }

    SetTimer(h, 1, UI_TIMER_MS, NULL);
    RegisterHotKey(h, HOTKEY_TOGGLE, 0, TOGGLE_VK);
    log_msg("UI up: four DPYes-style meters; INSERT toggles");

    for (;;) {
        MSG msg;
        if (!shown_once && g_player) {
            ShowWindow(h, SW_SHOW);
            shown_once = 1;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(30);
    }
}

/* ---------------- Worker and hook setup ---------------- */
static void *resolve(HMODULE module, const char *symbol, const char *label) {
    void *p = (void *)GetProcAddress(module, symbol);
    if (!p) {
        char line[192];
        _snprintf(line, sizeof(line), "WARN: unresolved %s", label);
        line[sizeof(line) - 1] = '\0';
        log_msg(line);
    }
    return p;
}

static DWORD WINAPI worker(LPVOID unused) {
    HMODULE game = NULL;
    void *pGMP, *pApply, *pSubtract;
    int i;
    (void)unused;

    {
        char line[96];
        _snprintf(line, sizeof(line),
            "DPYes-aligned %s worker up; locating Game.dll...", GAME_ARCH_NAME);
        line[sizeof(line) - 1] = '\0';
        log_msg(line);
    }
    for (i = 0; i < 60 && !game; ++i) {
        game = GetModuleHandleW(L"Game.dll");
        Sleep(1000);
    }
    if (!game) {
        log_msg("FAIL: Game.dll not found after 60s");
        return 1;
    }
    if (g_context_tls == TLS_OUT_OF_INDEXES) {
        log_msg("FAIL: unable to allocate damage-context TLS");
        return 1;
    }

    pGMP = resolve(game, SYM_GetMainPlayer, "GetMainPlayer");
    pApply = resolve(game, SYM_ApplyDamageCM, "CombatManager::ApplyDamage");
    pSubtract = resolve(game, SYM_SubtractLife, "Character::SubtractLife");
    g_fnGetAttackerIdCM = (GetUInt_t)resolve(game,
        SYM_GetAttackerIdCM, "CombatManager::GetAttackerId");
    g_fnGetObjectId = (GetUInt_t)resolve(game,
        SYM_GetObjectId, "Object::GetObjectId");
    g_fnGetMasterAttacker = (GetMasterAttacker_t)resolve(game,
        SYM_GetMasterAttacker, "GameEngine::GetMasterAttacker");
    g_fnLife = (GetDouble_t)resolve(game,
        SYM_GetCurrentLife, "Character::GetCurrentLife");
    g_fnXP = (GetUInt_t)resolve(game,
        SYM_GetExperiencePoints, "Character::GetExperiencePoints");

    g_identity_symbols_ready = g_fnGetAttackerIdCM &&
        g_fnGetObjectId && g_fnGetMasterAttacker;
    if (!pGMP || !pApply || !pSubtract) {
        log_msg("FAIL: required DPS hook symbols missing");
        return 1;
    }

    if (MH_Initialize() != MH_OK) {
        log_msg("FAIL: MH_Initialize");
        return 1;
    }
    if (MH_CreateHook(pGMP, (LPVOID)&HK_GetMainPlayer,
        (LPVOID *)&g_OrigGetMainPlayer) != MH_OK ||
        MH_EnableHook(pGMP) != MH_OK) {
        log_msg("FAIL: hook GetMainPlayer");
        return 1;
    }
    if (MH_CreateHook(pApply, (LPVOID)&HK_ApplyDamageCM,
        (LPVOID *)&g_OrigApplyDamageCM) != MH_OK ||
        MH_EnableHook(pApply) != MH_OK) {
        log_msg("FAIL: hook CombatManager::ApplyDamage");
        return 1;
    }
    if (MH_CreateHook(pSubtract, (LPVOID)&HK_SubtractLife,
        (LPVOID *)&g_OrigSubtractLife) != MH_OK ||
        MH_EnableHook(pSubtract) != MH_OK) {
        log_msg("FAIL: hook Character::SubtractLife");
        return 1;
    }

    log_msg("hooks enabled: context classification + selectable raw/mitigated stream");
    ui_run((HINSTANCE)GetModuleHandleW(NULL));
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID reserved) {
    unsigned i;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_log_cs);
        InitializeCriticalSection(&g_dps_cs);
        g_context_tls = TlsAlloc();
        for (i = 0; i < DPS_TYPE_COUNT; ++i)
            _snwprintf(g_unknown_type_names[i], 16, L"类型%u", i);
        reset_stats();
        DisableThreadLibraryCalls(hMod);
        {
            HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
            if (thread) CloseHandle(thread);
        }
    }
    return TRUE;
}
