# Each demo builds two executables: the dev build and the packed release. The
# objects and metadata are just intermediates.
# PACK=quick skips the compression model search, for faster iteration.
.DEFAULT_GOAL := all
CC     ?= gcc
PYTHON ?= python3
PACK   ?= full
INC    := -Iinclude
WARN   := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes
DEMO     ?= hello-world
DEMO_DIR := demos/$(DEMO)
DEMO_SRC := $(DEMO_DIR)/main.c
DEMO_SH  := $(DEMO_DIR)/shaders
DEMOS    := $(patsubst demos/%/main.c,%,$(wildcard demos/*/main.c))
BUILD    := build
OUT      := $(BUILD)/$(DEMO)
ENGINE_CORE := src/math.c src/gl_loader.c src/platform_common.c src/mesh.c src/render.c src/scene.c
ENGINE_AUDIO := src/synth.c src/audio.c
SOURCES := $(ENGINE_CORE) $(ENGINE_AUDIO) src/platform_x11_egl.c $(DEMO_SRC)
LIBS_SCREEN := -lEGL -lX11 -lm -lpthread -lasound
DEV_FLAGS := -std=gnu11 -g -O0 -DMC_DEV $(WARN) $(INC) -I$(OUT)/.dev/include
# -Oz, with no inlining of small or called-once functions. That makes the
# stream smaller at the cost of a little more synth CPU. The second to fourth
# lines (in both) were picked by a greedy search, the fifth and sixth by an
# earlier size pass. Each flag was measured on the coded stream, because what
# helps is code the packer's models predict better, and that isn't always less
# code. All of them were also checked to leave every picture and the audio
# unchanged. tools/verify.py reads REL_FLAGS literally, so don't put any
# variables in it other than $(INC) and $(OUT).
REL_FLAGS := -std=gnu11 -Oz -fno-inline-functions-called-once -fno-inline-small-functions \
             -fno-caller-saves -mtune=k8 -mstringop-strategy=libcall -fno-ira-hoist-pressure -fno-if-conversion \
             -fno-cse-follow-jumps -fno-shrink-wrap -fno-tree-sra -fno-tree-scev-cprop \
             -fno-gcse-lm -fno-tree-coalesce-vars -fno-ipa-pure-const -fno-ipa-ra -fno-thread-jumps -fno-lra-remat -fno-jump-tables \
             -fno-schedule-insns2 -fno-peephole2 -fno-optimize-sibling-calls \
             -fno-tree-tail-merge -fno-move-loop-invariants -fno-tree-dse -fno-tree-pre \
             -ffast-math -fno-exceptions -fno-plt -fno-stack-protector \
             -fno-unwind-tables -fno-asynchronous-unwind-tables -fomit-frame-pointer \
             -fcf-protection=none -Wa,-mx86-used-note=no \
             -ffunction-sections -fdata-sections -fno-pic -fno-pie -fvisibility=hidden \
             -flto -DMC_RELEASE $(INC) -I$(OUT)
REL_LDFLAGS := -no-pie -flto -flto-partition=one -Oz -fno-inline-functions-called-once -fno-inline-small-functions \
               -fno-caller-saves -mtune=k8 -mstringop-strategy=libcall -fno-ira-hoist-pressure -fno-if-conversion \
               -fno-cse-follow-jumps -fno-shrink-wrap -fno-tree-sra -fno-tree-scev-cprop \
               -fno-gcse-lm -fno-tree-coalesce-vars -fno-ipa-pure-const -fno-ipa-ra -fno-thread-jumps -fno-lra-remat -fno-jump-tables \
               -fno-schedule-insns2 -fno-peephole2 -fno-optimize-sibling-calls \
               -fno-tree-tail-merge -fno-move-loop-invariants -fno-tree-dse -fno-tree-pre \
               -Wl,--gc-sections -Wl,--build-id=none \
               -Wl,-z,norelro -Wl,-z,noseparate-code -Wl,--hash-style=sysv -s
# Flags for the payload only, since REL_FLAGS also builds tools/verify.py's
# normally linked fixtures. Every library call goes through a table that is
# filled in by name hash (mc_imports.h), and src/mc_start.c replaces crt1 and
# __libc_start_main. The ELF keeps its DT_NEEDED list but imports no symbols
# and no symbol versions.
PAYLOAD_DEFS := -DMC_IMPORT_HASH
PAYLOAD_LDFLAGS := -nostartfiles -Wl,--no-as-needed -B$(OUT)/.release/fpround/
# The payload link assembles its code through tools/fpround.py (that's what the
# gcc -B above is for). It rounds the float and double constants to the
# coarsest values within FPROUND_POLICY and sorts their pool. FPROUND=0 links
# them as written.
FPROUND ?= 1
FPROUND_POLICY := --rel 16
# tools/importcheck.py fails hello-world's link if there's a GL entry point or
# import the payload never calls, because the shared lists are cut down to what
# hello-world uses. Other demos may need less of the engine, so for them the
# unused ones are only listed (IMPORTCHECK=warn). IMPORTCHECK=error makes those
# strict too.
IMPORTCHECK ?= $(if $(filter hello-world,$(DEMO)),error,warn)
DEV_OBJS := $(addprefix $(OUT)/.dev/,$(SOURCES:.c=.o))
REL_OBJS := $(addprefix $(OUT)/.release/,$(SOURCES:.c=.o) src/mc_start.o)
SHADER_FILES := $(sort $(wildcard $(DEMO_SH)/*.vert $(DEMO_SH)/*.frag $(DEMO_SH)/*.comp $(DEMO_SH)/*.glsl))
IMPORTS_H := include/microcosm/mc_imports.h
# The engine's own post-pass shaders, baked into mc_engine_shaders.h. The
# release minifies them together with the demo's, and dev keeps them verbatim.
ENGINE_SH := src/shaders
ENGINE_SHADER_FILES := $(sort $(wildcard $(ENGINE_SH)/*.vert $(ENGINE_SH)/*.frag $(ENGINE_SH)/*.glsl))
GEN_RELEASE := $(PYTHON) tools/genshaders.py $(DEMO_SH) -o $(OUT)/shaders_gen.h --allow-empty \
               --engine $(ENGINE_SH) --engine-output $(OUT)/mc_engine_shaders.h
GEN_DEV := $(PYTHON) tools/genshaders.py --raw --engine $(ENGINE_SH) --engine-output $(OUT)/.dev/include/mc_engine_shaders.h
PACK_TOOLS := tools/mkelf.sh tools/pack.py tools/textrange.py tools/cm.h tools/cmpack.c tools/mcstub.c tools/s2dec.S \
              tools/launcher.ld tools/stage2.ld tools/s1.h tools/s1pack.c tools/s1dec.S tools/elfshrink.py

.PHONY: all clean $(DEMOS) dev dist capture shaders shader-report code-report check-demo FORCE
all: $(DEMOS)
$(DEMOS):
	@$(MAKE) --no-print-directory DEMO=$@ dev dist
check-demo:
	@test -f "$(DEMO_SRC)" || { echo "No main.c for demo $(DEMO)" >&2; exit 2; }
FORCE:

$(OUT)/.dev/config: FORCE tools/buildconfig.py
	@$(PYTHON) tools/buildconfig.py $@ --tool '$(CC)' --value '$(DEV_FLAGS) $(LIBS_SCREEN)' --value '$(abspath $(DEMO_SH))' --value '$(SOURCES)'
$(OUT)/.release/config: FORCE tools/buildconfig.py
	@$(PYTHON) tools/buildconfig.py $@ --tool '$(CC)' --value '$(REL_FLAGS) $(REL_LDFLAGS) $(LIBS_SCREEN)' --value '$(PAYLOAD_DEFS) $(PAYLOAD_LDFLAGS)' --value '$(SOURCES)'
$(OUT)/.shader-config: FORCE tools/buildconfig.py
	@$(PYTHON) tools/buildconfig.py $@ --tool '$(PYTHON)' --value '$(SHADER_FILES) $(ENGINE_SHADER_FILES)'
$(OUT)/.pack-config: FORCE tools/buildconfig.py
	@$(PYTHON) tools/buildconfig.py $@ --tool '$(CC)' --tool '$(PYTHON)' --value '$(PACK)'

$(OUT)/.dev/%.o: %.c $(OUT)/.dev/config
	@mkdir -p $(@D)
	$(CC) $(DEV_FLAGS) -DMC_SHADER_ROOT='"$(abspath $(DEMO_SH))"' -MMD -MP -c $< -o $@
$(OUT)/.release/%.o: %.c $(OUT)/.release/config
	@mkdir -p $(@D)
	$(CC) $(REL_FLAGS) $(PAYLOAD_DEFS) -MMD -MP -c $< -o $@
$(OUT)/.release/$(DEMO_SRC:.c=.o): $(OUT)/shaders_gen.h
# The entry point stays out of LTO so that its memcpy and friends still exist
# when code generation emits calls to them.
$(OUT)/.release/src/mc_start.o: src/mc_start.c $(OUT)/mc_imports_gen.h $(OUT)/.release/config
	@mkdir -p $(@D)
	$(CC) $(REL_FLAGS) $(PAYLOAD_DEFS) -fno-lto -MMD -MP -c $< -o $@
$(OUT)/mc_imports_gen.h: $(IMPORTS_H) tools/genimports.py
	@mkdir -p $(@D)
	$(PYTHON) tools/genimports.py $(IMPORTS_H) -o $@
$(OUT)/.release/src/render.o: $(OUT)/mc_engine_shaders.h
$(OUT)/.dev/src/render.o: $(OUT)/.dev/include/mc_engine_shaders.h

dev: check-demo $(OUT)/$(DEMO)-dev.bin
$(OUT)/$(DEMO)-dev.bin: $(DEV_OBJS)
	$(CC) $(DEV_FLAGS) $^ $(LIBS_SCREEN) -o $@.tmp
	@mv $@.tmp $@

shaders: check-demo $(OUT)/shaders_gen.h $(OUT)/mc_engine_shaders.h
# genshaders.py rewrites a header only when its content changes. A comment-only
# shader edit reruns it (through the stamp) but rebuilds no objects. The
# manifests are inputs too. programs.def and the files it includes (scene3.def,
# s3_*.def) define the release's program table, mc_program_shaders, in the
# order the C X-macros number the programs. If you reorder, add or repoint a
# program, the table has to change along with the objects that include the
# manifests, or the release draws with the wrong shaders. The dev header only
# holds the engine's shaders, and the dev build's objects follow the manifests
# through their own dependencies.
$(OUT)/.shader-stamp: $(SHADER_FILES) $(ENGINE_SHADER_FILES) $(wildcard $(DEMO_DIR)/*.def) \
                      tools/genshaders.py tools/glslmin.py $(OUT)/.shader-config
	@$(GEN_RELEASE)
	@touch $@
$(OUT)/shaders_gen.h $(OUT)/mc_engine_shaders.h: $(OUT)/.shader-stamp
	@if [ ! -f $@ ]; then $(GEN_RELEASE); fi
$(OUT)/.dev/.shader-stamp: $(ENGINE_SHADER_FILES) tools/genshaders.py $(OUT)/.shader-config
	@$(GEN_DEV)
	@touch $@
$(OUT)/.dev/include/mc_engine_shaders.h: $(OUT)/.dev/.shader-stamp
	@if [ ! -f $@ ]; then $(GEN_DEV); fi

# fpround's assembler. It's only rewritten when its text changes, so it also
# serves as this step's stamp.
$(OUT)/.release/fpround/as: FORCE tools/fpround.py
	@$(PYTHON) tools/fpround.py --install-as $(@D) --python '$(PYTHON)' --report $(OUT)/.release/fpround.txt \
	    $(if $(filter 0,$(FPROUND)),--off,$(FPROUND_POLICY))
$(OUT)/.release/payload.elf: $(OUT)/.release/fpround/as

# The payload is linked twice. The first link shows which functions there are,
# and the second lays them out by similarity (tools/funcorder.py), which the
# packer's models prefer. Before elfshrink drops the section headers, we read
# the .text range for the packer's x86 coder and check that every GL and import
# table slot is used. The second link's map is kept as payload.map, for
# tools/codecost.py.
$(OUT)/.release/payload.elf: $(REL_OBJS) tools/elfshrink.py tools/textrange.py tools/funcorder.py tools/importcheck.py
	$(CC) $(REL_FLAGS) $(REL_OBJS) $(REL_LDFLAGS) $(PAYLOAD_LDFLAGS) $(LIBS_SCREEN) -Wl,-Map=$@.map -o $@.tmp
	$(PYTHON) tools/funcorder.py $@.tmp $@.map > $(OUT)/.release/payload.order
	$(CC) $(REL_FLAGS) $(REL_OBJS) $(REL_LDFLAGS) $(PAYLOAD_LDFLAGS) $(LIBS_SCREEN) \
	    -Wl,--section-ordering-file=$(OUT)/.release/payload.order -Wl,-Map=$@.map -o $@.tmp
	CC='$(CC)' $(PYTHON) tools/importcheck.py $@.tmp $@.map $(if $(filter warn,$(IMPORTCHECK)),--warn)
	$(PYTHON) tools/textrange.py $@.tmp > $(OUT)/.release/payload.range
	$(PYTHON) tools/elfshrink.py $@.tmp -o $@.verified --verify --golf
	@mv $@.map $(OUT)/.release/payload.map
	@mv $@.verified $@
	@rm -f $@.tmp
dist: check-demo $(OUT)/$(DEMO).bin
# What each shader costs in the packed file, found by replaying the packer's
# model over the payload. tools/shadercost.py --save/--against compares two
# builds.
shader-report: dist
	$(PYTHON) tools/shadercost.py $(OUT)
# The same for each function, array and source file (tools/codecost.py, which
# reads payload.map).
code-report: dist
	$(PYTHON) tools/codecost.py $(OUT)
$(OUT)/$(DEMO).bin: $(OUT)/.release/payload.elf $(PACK_TOOLS) $(OUT)/.pack-config
	@case '$(PACK)' in full|quick) ;; *) echo 'PACK must be full or quick' >&2; exit 2;; esac
	CC='$(CC)' PYTHON='$(PYTHON)' tools/mkelf.sh $< $@ $(if $(filter quick,$(PACK)),--quick,) --cache-dir $(OUT)/.pack-cache --manifest $(OUT)/.release/pack.json --code-range $(OUT)/.release/payload.range

# Record the show as the release video (tools/capture.sh): 1920x1080 at 60
# fps with its soundtrack, every frame rendered offline from 2160 lines. The
# result is VIDEO, media/<demo>-1080p60.mp4 by default, which is the file the
# README links to. It sits outside build/, so make clean doesn't throw away an
# hour of rendering, but the 2.5 GB master is made in build/<demo>/capture/.
# MC_CAP_DUR=<s> records only the start and MC_CAP_SCALE=<25-200> renders at
# another internal resolution, on make's command line too. The capture uses
# the release flags, so its pictures match the .bin's (Scene 1's collision sim
# included), apart from fpround's rounding. It's linked normally and runs
# surfaceless, with no window and no sound.
VIDEO ?= media/$(DEMO)-1080p60.mp4
CAPTURE_SRC := $(ENGINE_CORE) src/synth.c src/platform_surfaceless.c $(DEMO_SRC)
capture: check-demo $(OUT)/$(DEMO)-capture tools/capture.sh
	tools/capture.sh $(OUT)/$(DEMO)-capture '$(VIDEO)' $(OUT)/capture
$(OUT)/$(DEMO)-capture: $(CAPTURE_SRC) $(wildcard include/microcosm/*.h $(DEMO_DIR)/*.c $(DEMO_DIR)/*.def) \
                        $(OUT)/shaders_gen.h $(OUT)/mc_engine_shaders.h $(OUT)/.release/config
	$(CC) $(REL_FLAGS) -DMC_HEADLESS -DMC_CAPTURE $(CAPTURE_SRC) -no-pie -Wl,--gc-sections -lEGL -lm -o $@.tmp
	@mv $@.tmp $@

-include $(DEV_OBJS:.o=.d) $(REL_OBJS:.o=.d)
clean:
	rm -rf build
