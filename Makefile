# Midnight Club 3 recomp: the development loop, from the repo root.
#
#   make            this help
#   make lift       first lift (or after reinstalling the disc)
#   make build run  the everyday loop
#
# Two games (ADR 0006): GAME=dub (DUB Edition, the default) or GAME=remix
# (DUB Edition Remix), e.g. make lift build run GAME=remix. Each has its own
# config/<game>/, src/games/<game>/, discs (game[-remix][-<edition>]/) and
# work folder (work-dub/, work-remix/).
#
# Targets wrap the scripts in tools/, switch/ and vm/ (which also work alone).
# Settings pass straight through, e.g.
#   make run SECS=60 RECOMP_KEYBOARD=1
#   make build VULKAN=1 JOBS=4
# Logs go to work-<game>/logs/.

SHELL := /bin/bash
.SHELLFLAGS := -eo pipefail -c
# macOS ships make 3.81, which ignores .SHELLFLAGS: recipes that log through
# tee say set -o pipefail themselves, or a failure ends as success (a failed
# Switch build reported exit 0, 2026-10-24).
.DEFAULT_GOAL := help

GAME ?= dub
# work/ was the DUB Edition's before the games were split: moved once, before
# anything makes work-dub/logs (tools/env.sh would then leave it be).
ifeq ($(GAME)$(MC3_WORK),dub)
  ifneq ($(wildcard work/.),)
    ifeq ($(wildcard work-dub/.),)
      $(shell mv work work-dub && echo "note: work/ moved to work-dub/ (one work folder per game now)" >&2)
    endif
  endif
endif
WORK := $(or $(MC3_WORK),$(CURDIR)/work-$(GAME))
LOGS := $(WORK)/logs
NOW   = $(shell date +%Y%m%d_%H%M%S)

# Command-line settings that the scripts read from the environment.
export VULKAN DEBUG GEN_DEBUGINFO JOBS SECS HEADLESS GDB LTO LTO_JOBS NO_GAME WINDOW VIDEO KEEP EVERY NATIVE KEYS TRACE \
       MC3_WORK MC3_GAME_DIR MC3_MMIO_SECTIONS MC3_RECOMP_ARGS MC3_DISASM_ARGS FACTOR MIN FILES HD SCALE UI_SCALE SETTINGS GAME \
       RELEASE_DIR MC3_BUILD_ID EDITION WIN_ARCH DISC

.PHONY: help setup nsp check-vm extract lift regen relift \
        xbe-edition build build-vk build-debug windows mac-app run run-gdb run-headless snapshot report hoststacks profile record difftest trap \
        switch switch-vk switch-lto send \
        hd-dump hd-tools hd-upscale import-save save-sync-up save-sync-down \
        seeds lift-report log du clean-build clean-gen clean toolkit-test

help:  ## Show this help
	@awk 'BEGIN {FS = ":.*## "} /^## / {printf "\n%s\n", substr($$0, 4)} \
	     /^[a-z-]+:.*## / {printf "  make %-14s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@echo

## Setup (once)
setup:  ## Install build deps and the Python venv (Ubuntu: also Docker for the Switch; macOS: Homebrew packages; Windows: MSYS2 packages)
	@case "$$(uname -s)" in \
	   Darwin) bash tools/setup_mac.sh ;; \
	   MINGW*|MSYS*|CYGWIN*) bash tools/setup_windows.sh ;; \
	   *) bash vm/setup-ubuntu.sh ;; \
	 esac

check-vm:  ## Print the VM checks (CPU, page size, RAM, disk, OpenGL)
	@uname -m; echo "page size: $$(getconf PAGESIZE)"; echo "cores: $$(nproc)"
	@free -g | head -2; df -h . | tail -1
	@glxinfo -B 2>/dev/null | grep -E 'renderer|core profile version' || echo "glxinfo: install mesa-utils"

extract:  ## Unpack an ISO into game/ (EDITION=eur: game-eur/; GAME=remix: game-remix[-<edition>]/): make extract ISO=<file.iso>
	@test -n "$(ISO)" || { echo "usage: make extract ISO=<file.iso>"; exit 1; }
	bash tools/extract_disc.sh "$(ISO)"

## Lift (XBE -> C in work-<game>/gen)
lift:  ## Parse the XBE, write config/<game>/xbe_facts.h, full lift
	@mkdir -p $(LOGS)
	set -o pipefail; bash tools/first_lift.sh 2>&1 | tee $(LOGS)/lift_$(NOW).log

regen:  ## Full regen: after changing config/<game>/seed_functions.json
	@mkdir -p $(LOGS)
	set -o pipefail; bash tools/regen.sh 2>&1 | tee $(LOGS)/regen_$(NOW).log

relift:  ## Lift only: after adding/removing an override, or a translator change (TRACE=XACTENG,DSOUND: failure trace)
	@mkdir -p $(LOGS)
	set -o pipefail; LIFT_ONLY=1 bash tools/regen.sh 2>&1 | tee $(LOGS)/relift_$(NOW).log

seeds:  ## Propose seeds from a run log: make seeds LOG=work-<game>/logs/run_x.log (review, then make regen)
	@test -n "$(LOG)" || { echo "usage: make seeds LOG=<run log>"; exit 1; }
	source tools/env.sh && mc3_python && cd xboxrecomp && "$$MC3_PYTHON" -m tools.seed_from_log "$(abspath $(LOG))" \
	    "$$MC3_XBE" --functions "$$MC3_ANALYSIS_DIR/disasm/functions.json" --seeds "$$MC3_SEEDS"
	@echo "review the additions: git diff config/$(GAME)/seed_functions.json"

xbe-edition:  ## Can another disc run on this game's lift? make xbe-edition GAME=remix XBE=game-remix-usa/default.xbe (prints what to add)
	@test -n "$(XBE)" || { echo "usage: make xbe-edition [GAME=remix] XBE=<another disc's default.xbe> [EDITION=<name>]"; exit 1; }
	@bash -c 'unset EDITION; source tools/env.sh && python3 tools/xbe_edition.py "$(XBE)" "$$MC3_XBE" $(EDITION)'

lift-report:  ## Summarise the last lift (functions, stubs, unimplemented)
	@grep -E '^(Poll loops|MMIO|[0-9]+/[0-9]+ functions|[0-9]+ unresolved|[0-9]+ instruction)' $(WORK)/analysis/recomp.log \
	    || echo "no lift yet: make lift"

## Linux (the debug loop)
build:  ## Build the desktop binary (OpenGL): Linux, macOS, or Windows in MSYS2 (Windows: SCALE=5, UI_SCALE=5, SETTINGS= as defaults, like mac-app)
	@mkdir -p $(LOGS)
	@set -o pipefail; case "$$(uname -s)" in \
	   MINGW*|MSYS*|CYGWIN*) bash tools/build_windows.sh ;; \
	   *) bash tools/build_linux.sh ;; \
	 esac 2>&1 | tee $(LOGS)/build_$(NOW).log

windows:  ## Cross-build the Windows exe from macOS/Linux (llvm-mingw in work-<game>/tools; WIN_ARCH=aarch64 for ARM)
	@mkdir -p $(LOGS)
	set -o pipefail; bash tools/build_windows.sh 2>&1 | tee $(LOGS)/build_windows_$(NOW).log

build-vk:  ## Build with the Vulkan renderer (needs glslang 12+)
	$(MAKE) build VULKAN=1

build-debug:  ## Build hand-written code at -O0 for gdb (+ GEN_DEBUGINFO=1 for lifted C)
	$(MAKE) build DEBUG=1

run:  ## Run in a window; keyboard = pad 1 (SECS=n stops after n s; WATCH=1: F12 logs every draw of that frame)
	$(if $(WATCH),RECOMP_GL_WATCH=all RECOMP_GL_WATCH_FRAME=key) bash tools/run_linux.sh

run-gdb:  ## Run under gdb (bt = host chain; call print_guest_state())
	GDB=1 bash tools/run_linux.sh

run-headless:  ## Run without a window (pair with RECOMP_GL_DUMP / RECOMP_PAD_SCRIPT)
	HEADLESS=1 bash tools/run_linux.sh

snapshot:  ## Diagnose a hang: headless run, guest stack dumped after 30 s (WD=n to change)
	@HEADLESS=1 SECS=$${SECS:-90} RECOMP_WATCHDOG_SECS=$(or $(WD),30) bash tools/run_linux.sh; \
	 rc=$$?; if [ $$rc -eq 3 ]; then echo "snapshot taken (watchdog exit 3 is expected): make report"; \
	 elif [ $$rc -ne 0 ]; then echo "run ended with exit code $$rc (not the watchdog): make report"; exit $$rc; fi

difftest:  ## Replay call CALL of wrapped function FUNC on x86 (Unicorn) vs the lifted C (FUNC default 0037CCD0)
	@test -n "$(CALL)" || { echo "usage: make difftest CALL=<n> [FUNC=0037CCD0] [WITHIN=<outer func>:<n>]"; exit 1; }
	@mkdir -p $(LOGS)
	MC3_DIFFTEST_CALL=$(CALL) MC3_DIFFTEST_FUNC=$(or $(FUNC),0037CCD0) MC3_DIFFTEST_WITHIN=$(WITHIN) \
	    HEADLESS=1 SECS=$${SECS:-300} bash tools/run_linux.sh || true
	@set -o pipefail; py=; for p in $(WORK)/venv/bin/python3 python3; do $$p -c 'import numpy, unicorn, capstone' 2>/dev/null && { py=$$p; break; }; done; \
	 if [ -z "$$py" ]; then echo "captured in $(WORK)/difftest; no Python with numpy+unicorn here:"; \
	   echo "  $(WORK)/venv/bin/pip install numpy unicorn   (or replay on another machine)"; exit 0; fi; \
	 $$py tools/difftest.py $(WORK)/difftest/$(or $(FUNC),0037CCD0)_$(CALL)_before.bin 2>&1 | tee $(LOGS)/difftest_$(NOW).log

record:  ## Record frames + audio into work-<game>/capture/<time>/ (SECS, EVERY, KEEP=1, WINDOW=1, VIDEO=0)
	bash tools/record.sh $(or $(SECS),60)

profile:  ## Where the time goes: perf sampling of a headless run (SECS, default 40)
	bash tools/profile.sh $(or $(SECS),40)

hoststacks:  ## Every thread's host backtrace after SECS (default 45): who waits on what
	bash tools/host_stacks.sh $(or $(SECS),45)

report:  ## Explain the snapshot in the newest run log (LOG=<file> for another)
	@source tools/env.sh && python3 tools/snapshot_report.py $(LOG)

trap:  ## Run, stopping at the first untranslated instruction reached
	RECOMP_UNIMPL_TRAP=1 bash tools/run_linux.sh

## macOS app

mac-app:  ## Package the Mac build as work-<game>/mac_app/Midnight Club 3[ Remix].app (SCALE=5, UI_SCALE=5 by default, SETTINGS="K=V ...", HD=1, DISC=<disc dir>)
	@mkdir -p $(LOGS)
	$(MAKE) build
	set -o pipefail; bash tools/mac_app.sh 2>&1 | tee $(LOGS)/mac_app_$(NOW).log

## Texture packs (hd/ beside game/; xboxrecomp/src/nv2a_gl/hd_pack.c)

hd-dump:  ## Run with every texture written once to hd/dump/<hash>.png (options as for run)
	RECOMP_GL_TEX_DUMP=1 $(MAKE) run

hd-tools:  ## Fetch the upscaler (Real-ESRGAN) for this OS into work-<game>/tools (macOS, x86-64 Linux)
	bash tools/hd_tools.sh

hd-upscale:  ## hd/dump -> hd/textures, new ones only (FACTOR=2|3|4, MIN=px, FILES="... " to redo some)
	@mkdir -p $(LOGS)
	set -o pipefail; bash tools/hd_upscale.sh $(or $(FACTOR),2) 2>&1 | tee $(LOGS)/hd_upscale_$(NOW).log

## Switch
switch:  ## Build the NRO in devkitPro's pinned container (switch/devkitpro-image) and stage work-<game>/switch_sd; HD=1: with hd/textures; NATIVE=1: local devkitPro
	@mkdir -p $(LOGS)
	set -o pipefail; bash switch/$(if $(filter 1,$(NATIVE)),build.sh,docker_build.sh) 2>&1 | tee $(LOGS)/switch_$(NOW).log

nsp:  ## NSP forwarders (home-menu apps; locally only), one per edition, from config/<game>/app.env, in the container; EDITION=usa for one, KEYS=keys/prod.keys
	@mkdir -p $(LOGS)
	set -o pipefail; bash switch/nsp.sh prepare 2>&1 | tee $(LOGS)/nsp_$(NOW).log
	set -o pipefail; bash switch/$(if $(filter 1,$(NATIVE)),nsp.sh,docker_build.sh switch/nsp.sh) 2>&1 | tee -a $(LOGS)/nsp_$(NOW).log

switch-vk:  ## Switch build with Vulkan (NVK)
	$(MAKE) switch VULKAN=1

switch-lto:  ## Switch build with link-time optimisation
	$(MAKE) switch LTO=1

send:  ## Send the NRO over nxlink (hbmenu, press Y) and log the game to work-<game>/logs/switch_*.log; SWITCH_IP=a.b.c.d, NATIVE=1
	@mkdir -p $(LOGS)
	set -o pipefail; bash switch/send.sh 2>&1 | tee $(LOGS)/switch_run_$(NOW).log

import-save:  ## Import an Xbox MC3 profile: make import-save FILE=<.gsx | memory unit dump | save folder | .mc3> (UDATA=<dir>, REPLACE=1, LIST=1)
	@test -n "$(FILE)" || { echo "usage: make import-save FILE=<save file or folder>"; exit 2; }
	source tools/env.sh && python3 tools/save_import.py "$(FILE)" $(if $(UDATA),--udata "$(UDATA)") $(if $(REPLACE),--replace) $(if $(LIST),--list) $(if $(YES),--yes)

save-sync-down:  ## Switch -> here: the profiles over FTP (HOST_IP=<ip>[:port], port 21 by default; PROFILE=<folder or name>, GAME_ID=54540079, LIST=1, YES=1 to skip the question)
	@test -n "$(or $(HOST_IP),$(SWITCH_IP))" || { echo "usage: make save-sync-down HOST_IP=<the Switch's IP>[:port] (an FTP server running on it; port 21 by default)"; exit 2; }
	source tools/env.sh && python3 tools/save_sync.py down --host "$(or $(HOST_IP),$(SWITCH_IP))" $(if $(GAME_ID),--game-id $(GAME_ID)) $(if $(PROFILE),--profile $(PROFILE)) $(if $(UDATA),--udata "$(UDATA)") $(if $(FTP_USER),--user "$(FTP_USER)") $(if $(FTP_PASS),--password "$(FTP_PASS)") $(if $(LIST),--list) $(if $(YES),--yes)

save-sync-up:  ## Here -> Switch: the profiles over FTP (HOST_IP=<ip>[:port], port 21 by default; PROFILE=<folder or name>, GAME_ID=54540079, LIST=1, YES=1 to skip the question)
	@test -n "$(or $(HOST_IP),$(SWITCH_IP))" || { echo "usage: make save-sync-up HOST_IP=<the Switch's IP>[:port] (an FTP server running on it; port 21 by default)"; exit 2; }
	source tools/env.sh && python3 tools/save_sync.py up --host "$(or $(HOST_IP),$(SWITCH_IP))" $(if $(GAME_ID),--game-id $(GAME_ID)) $(if $(PROFILE),--profile $(PROFILE)) $(if $(UDATA),--udata "$(UDATA)") $(if $(FTP_USER),--user "$(FTP_USER)") $(if $(FTP_PASS),--password "$(FTP_PASS)") $(if $(LIST),--list) $(if $(YES),--yes)

## Housekeeping
log:  ## Show the end of the newest log in work-<game>/logs
	@f=$$(ls -t $(LOGS)/*.log 2>/dev/null | head -1); \
	 if [ -n "$$f" ]; then echo "== $$f"; tail -40 "$$f"; else echo "no logs yet"; fi

du:  ## Disk use of work-<game>/ and the discs
	@du -sh $(WORK)/* game game-* 2>/dev/null | sort -h

toolkit-test:  ## Run the toolkit's Python unit tests
	source tools/env.sh && mc3_python && cd xboxrecomp && "$$MC3_PYTHON" -m pytest -q tools/

clean-build:  ## Delete build trees (keeps the lift)
	rm -rf $(WORK)/build-*

clean-gen:  ## Delete the lifted C and analysis (next: make lift)
	rm -rf $(WORK)/gen $(WORK)/analysis

clean: clean-build clean-gen  ## Delete everything generated except the venv and logs
