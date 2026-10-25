#!/usr/bin/env bash
# Regenerate the lifted C from the XBE:
#   xbe_parser -> disasm -> func_id -> abi_analysis -> recomp
#
#   tools/regen.sh                  full run
#   LIFT_ONLY=1 tools/regen.sh      recomp only: enough after editing
#                                   src/games/<game>/recomp_manual.c or the translator;
#                                   seed changes need the full run
#
# Analysis goes to $MC3_ANALYSIS_DIR and the C to $MC3_GEN_DIR, both outside
# the repo (tools/env.sh), so the vendored toolkit tree stays clean.
#
# Seeds (config/<game>/seed_functions.json) are entry points the static pass cannot
# see: thread start routines and indirect-call targets found at runtime. Grow
# them from a run log with the toolkit's tools.seed_from_log and review every
# addition -- a seed inside a function splits it.
#
# MC3_MMIO_SECTIONS: XBE sections whose code drives hardware registers
# (NFSU2 used DSOUND,XPP). Lifted with MMIO accessors so devices work without
# fault trapping. Set after reading the section list from the first parse.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mc3_python

# The game's seeds and overrides (tools/env.sh: config/<game>/,
# src/games/<game>/; MC3_SEEDS / MC3_MANUAL point elsewhere).
SEEDS="$MC3_SEEDS"
MANUAL="$MC3_MANUAL"
# TRACE=<sections>: failure-tracing wrappers for every function in them
# (tools/trace_wrappers.py), lifted and compiled with the game; empty: none.
TRACE_C="$MC3_GEN_DIR/zz_trace_wrappers.c"
MMIO="${MC3_MMIO_SECTIONS:-DSOUND,XPP}"
# Extra arguments for the disassembler / recompiler steps, as words: split on
# spaces once into arrays, never globbed (e.g. MC3_RECOMP_ARGS="--a --b 2").
read -ra DISASM_ARGS <<< "${MC3_DISASM_ARGS:-}"
read -ra RECOMP_ARGS <<< "${MC3_RECOMP_ARGS:-}"
A="$MC3_ANALYSIS_DIR"

mc3_need_game
mkdir -p "$A/disasm" "$A/func_id" "$A/abi" "$A/recomp" "$MC3_GEN_DIR"
cd "$XBOXRECOMP_DIR"

# disasm reads <xbe stem>_analysis.json beside the XBE.
XBE_JSON="${MC3_XBE%.xbe}_analysis.json"

if [ "${LIFT_ONLY:-0}" != "1" ]; then
    "$MC3_PYTHON" -m tools.xbe_parser "$MC3_XBE" --json "$XBE_JSON" --quiet
    cp "$XBE_JSON" "$A/xbe_analysis.json"
    # config/<game>/not_functions.json: addresses that are no function start
    # (false starts that split a function; the seeds' mirror image).
    NOT_FUNCS=()
    [ -f "${MC3_NOT_FUNCTIONS:-$MC3_CONFIG/not_functions.json}" ] \
        && NOT_FUNCS=(--exclude-functions "${MC3_NOT_FUNCTIONS:-$MC3_CONFIG/not_functions.json}")
    "$MC3_PYTHON" -m tools.disasm "$MC3_XBE" -o "$A/disasm" \
        --analysis-json "$XBE_JSON" --seed-functions "$SEEDS" \
        ${NOT_FUNCS[@]+"${NOT_FUNCS[@]}"} ${DISASM_ARGS[@]+"${DISASM_ARGS[@]}"}
    "$MC3_PYTHON" -m tools.func_id "$MC3_XBE" -o "$A/func_id" \
        --functions "$A/disasm/functions.json" \
        --strings "$A/disasm/strings.json" \
        --xrefs "$A/disasm/xrefs.json"
    "$MC3_PYTHON" -m tools.abi_analysis "$MC3_XBE" --output-dir "$A/abi" \
        --disasm-dir "$A/disasm" --func-id-dir "$A/func_id"
fi

# config/<game>/edition_variants.json: branches whose condition differs
# between editions of the disc (the Remix's USA disc: one), lifted with both
# and chosen at run time (main.c sets g_recomp_xbe_variant from the disc).
VARIANTS=()
[ -f "$MC3_CONFIG/edition_variants.json" ] \
    && VARIANTS=(--edition-variants "$MC3_CONFIG/edition_variants.json")

# recomp_types.h is written only when absent, so a stale copy would hide
# register-model changes in the toolkit. Nothing edits it by hand.
rm -f "$MC3_GEN_DIR/recomp_types.h"
EXCLUDE="$MANUAL"
rm -f "$TRACE_C"
if [ -n "${TRACE:-}" ]; then
    mkdir -p "$MC3_WORK/trace"
    "$MC3_PYTHON" "$MC3_REPO/tools/trace_wrappers.py" --sections "$TRACE" \
        --functions "$A/disasm/functions.json" --manual "$MANUAL" \
        --out "$MC3_WORK/trace/trace_wrappers.c"
    # One file for the lift to scan: the hand-written overrides + the trace.
    cat "$MANUAL" "$MC3_WORK/trace/trace_wrappers.c" > "$MC3_WORK/trace/manual_all.c"
    EXCLUDE="$MC3_WORK/trace/manual_all.c"
fi
"$MC3_PYTHON" -m tools.recomp "$MC3_XBE" --game-name "Midnight Club 3: DUB Edition" \
    --all --split 250 --gen-dir "$MC3_GEN_DIR" -o "$A/recomp" \
    --disasm-dir "$A/disasm" --func-id-dir "$A/func_id" --abi-dir "$A/abi" \
    --exclude-manual "$EXCLUDE" --mmio-sections "$MMIO" \
    ${VARIANTS[@]+"${VARIANTS[@]}"} \
    ${RECOMP_ARGS[@]+"${RECOMP_ARGS[@]}"} 2>&1 | tee "$A/recomp.log"

# The trace wrappers are compiled with the lifted code (CMake globs gen/*.c).
[ -n "${TRACE:-}" ] && cp "$MC3_WORK/trace/trace_wrappers.c" "$TRACE_C"
# What the build checks the overrides against (mc3_check_lift_fresh).
mc3_manual_overrides | tr -d '\r' > "$MC3_GEN_DIR/.manual_overrides" || true   # LF everywhere

echo
echo "lifted C in $MC3_GEN_DIR: $(find "$MC3_GEN_DIR" -name '*.c' | wc -l) files, $(du -sh "$MC3_GEN_DIR" | cut -f1)"
# The translator ends with its tally of instructions it could not lift (the
# honest coverage signal); the whole run is kept in recomp.log.
echo "recomp log: $A/recomp.log"
