# Scribodo build
#
#   make            build the app (build/scribodo)
#   make run        build and launch with the sample document
#   make test       build and run unit tests
#   make sanitize   run the tests under AddressSanitizer + UBSan
#   make clean      remove build outputs (keeps the raylib build)
#   make distclean  remove everything under build/
#
# raylib is compiled out-of-tree from the pinned submodule, so the
# submodule checkout stays clean.

CC      ?= cc
.DEFAULT_GOAL := all
BUILD   ?= build
UNAME_S := $(shell uname -s)

# -mcpu=native targets the host CPU (NEON and newer AArch64 extensions on
# Apple Silicon). Override with `make ARCH_FLAGS=` for portable binaries.
ARCH_FLAGS ?= -mcpu=native
OPT        ?= -O2
CSTD       := -std=c11
WARN       := -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
CFLAGS     := $(CSTD) $(OPT) $(ARCH_FLAGS) -g $(WARN)

RAYLIB_DIR := third_party/raylib/src
INCLUDES   := -Isrc -I$(RAYLIB_DIR) -Ithird_party/raygui/src -Ithird_party/clay -Ithird_party/stb

# --- raylib (static, GLFW desktop backend, OpenGL 3.3) ----------------------
RAYLIB_SRCS  := rcore rshapes rtextures rtext rmodels raudio rglfw
RAYLIB_OBJS  := $(addprefix $(BUILD)/raylib/,$(addsuffix .o,$(RAYLIB_SRCS)))
RAYLIB_LIB   := $(BUILD)/raylib/libraylib.a
RAYLIB_FLAGS := -std=gnu99 $(OPT) $(ARCH_FLAGS) -DPLATFORM_DESKTOP_GLFW -DGRAPHICS_API_OPENGL_33 \
                -I$(RAYLIB_DIR) -I$(RAYLIB_DIR)/external/glfw/include -w

ifeq ($(UNAME_S),Darwin)
  GLFW_LANG := -x objective-c
  LDLIBS    := -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio \
               -framework CoreVideo -framework QuartzCore
else
  GLFW_LANG :=
  LDLIBS    := -lGL -lm -lpthread -ldl -lrt -lX11
endif

$(BUILD)/raylib/rglfw.o: $(RAYLIB_DIR)/rglfw.c | $(BUILD)/raylib
	$(CC) $(GLFW_LANG) $(RAYLIB_FLAGS) -c $< -o $@

$(BUILD)/raylib/%.o: $(RAYLIB_DIR)/%.c | $(BUILD)/raylib
	$(CC) $(RAYLIB_FLAGS) -c $< -o $@

$(RAYLIB_LIB): $(RAYLIB_OBJS)
	ar rcs $@ $^

# --- Scribodo ----------------------------------------------------------------
CORE_SRCS := src/core/utf8.c src/core/text_buffer.c src/core/undo.c src/core/wrap.c src/core/doc_io.c
APP_SRCS  := src/main.c src/ui/font.c src/ui/clay_render.c src/ui/editor.c src/ui/keys.c src/ui/vendor_impl.c
CORE_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRCS))
APP_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(APP_SRCS))
APP   := $(BUILD)/scribodo
TESTS := $(BUILD)/test_core $(BUILD)/test_doc_io $(BUILD)/test_editor

.PHONY: all run test sanitize clean distclean

all: $(APP)

$(APP): $(CORE_OBJS) $(APP_OBJS) $(RAYLIB_LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS) -lm

# Vendored single-header implementations are compiled without our warnings.
$(BUILD)/src/ui/vendor_impl.o: src/ui/vendor_impl.c | $(BUILD)/src/ui
	$(CC) $(CSTD) $(OPT) $(ARCH_FLAGS) -g -w $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD)/test_core: $(CORE_OBJS) $(BUILD)/tests/test_core.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_doc_io: $(CORE_OBJS) $(BUILD)/tests/test_doc_io.o
	$(CC) $(CFLAGS) $^ -o $@

# The real editor, linked against headless raylib stubs.
$(BUILD)/test_editor: $(CORE_OBJS) $(BUILD)/src/ui/editor.o $(BUILD)/src/ui/keys.o $(BUILD)/tests/raylib_stubs.o $(BUILD)/tests/test_editor.o
	$(CC) $(CFLAGS) $^ -o $@ -lm

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; echo; done

SAN_FLAGS := -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
sanitize:
	$(MAKE) --no-print-directory BUILD=build/asan OPT="$(SAN_FLAGS)" test

run: $(APP)
	./$(APP) assets/sample/sample.md

$(BUILD)/raylib $(BUILD)/src/ui:
	@mkdir -p $@

clean:
	rm -rf $(BUILD)/src $(BUILD)/tests $(APP) $(TESTS) build/asan

distclean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
