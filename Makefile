# Scribodo build
#
#   make            build the app (build/scribodo)
#   make run        build and launch with the sample document
#   make test       build and run unit tests
#   make sanitize   run the tests under AddressSanitizer + UBSan
#   make llama      build the pinned llama-server (third_party/llama.cpp)
#   make model      download the pinned LFM3-1.7B BF16 GGUF (3.5 GB, SHA-256 checked)
#   make llmtest    end-to-end test against the real model (needs llama + model)
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
INCLUDES   := -Isrc -I$(RAYLIB_DIR) -Ithird_party/raygui/src -Ithird_party/clay -Ithird_party/stb -Ithird_party/cJSON \
              -Ithird_party/mariadb-connector-c/include -Ibuild/mariadb-cc/include

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
CORE_SRCS := src/core/utf8.c src/core/text_buffer.c src/core/undo.c src/core/wrap.c src/core/doc_io.c src/core/diff.c src/core/file_text.c src/app/frame_view_model.c
STORE_SRCS := src/app/db.c src/app/store.c src/app/project_store.c src/app/observer_store.c src/app/store_revisions.c src/app/store_records.c src/app/store_chats.c src/app/store_chat_history.c src/app/store_import.c src/app/store_entities.c src/app/backup.c src/app/backup_job.c src/app/service_supervisor.c src/app/entity_backfill_job.c src/app/name_detection_job.c src/app/project_session.c src/app/explorer_snapshot.c src/app/profile.c src/app/model_settings.c
LLM_SRCS  := src/core/json.c src/llm/llm_text.c src/llm/http.c src/llm/server.c src/llm/chat.c src/llm/chat_history.c src/app/chat_store.c src/app/revisions.c src/app/records.c src/app/recent.c src/app/renames.c src/llm/observer.c src/llm/observer_text.c src/llm/observer_fake.c src/llm/observer_model.c src/llm/observer_job.c src/llm/semantic.c src/llm/remote.c src/llm/names.c src/llm/memfit.c $(STORE_SRCS)
LLM_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(LLM_SRCS)) $(BUILD)/third_party/cJSON/cJSON.o
APP_SRCS  := src/main.c src/app/script.c src/app/chat_controller.c src/app/reviewed_edit_controller.c src/ui/font.c src/ui/clay_render.c src/ui/editor.c src/ui/input.c \
             src/ui/chat_view.c src/ui/review_view.c src/ui/record_view.c src/ui/findings_view.c src/ui/prompt_view.c src/ui/menu_view.c src/ui/vendor_impl.c
CORE_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRCS))
APP_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(APP_SRCS))
# Native dialogs (Objective-C on macOS).
APP_OBJS  += $(BUILD)/src/platform/dialogs_mac.o
APP   := $(BUILD)/scribodo
TESTS := $(BUILD)/test_core $(BUILD)/test_doc_io $(BUILD)/test_editor $(BUILD)/test_editor_faults $(BUILD)/test_db_rows $(BUILD)/test_observer_text $(BUILD)/test_observer_fake $(BUILD)/test_chat_history $(BUILD)/test_chat_view $(BUILD)/test_chat_view_faults $(BUILD)/test_components $(BUILD)/test_remote $(BUILD)/test_llm $(BUILD)/test_persistence_faults $(BUILD)/test_semantic_faults

.PHONY: all run test uitest uitest-real sanitize tsan cstest dbtest uitest-db backuptest remotetest offlinetest dependency-report clean distclean llama model llmtest

all: $(APP)

$(APP): $(CORE_OBJS) $(LLM_OBJS) $(APP_OBJS) $(RAYLIB_LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS) $(MARIADB_LIBS) -lm -lpthread


# cJSON is vendored; compile it without our warning flags.
$(BUILD)/third_party/cJSON/cJSON.o: src/core/json_vendor.c third_party/cJSON/cJSON.c third_party/cJSON/cJSON.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(ARCH_FLAGS) -g -w -c $< -o $@

# Vendored single-header implementations are compiled without our warnings.
$(BUILD)/src/ui/vendor_impl.o: src/ui/vendor_impl.c | $(BUILD)/src/ui
	$(CC) $(CSTD) $(OPT) $(ARCH_FLAGS) -g -w $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(ARCH_FLAGS) -g -fobjc-arc -Wall -Wextra -Isrc -MMD -MP -c $< -o $@

$(BUILD)/test_core: $(CORE_OBJS) $(BUILD)/tests/test_core.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_doc_io: $(CORE_OBJS) $(BUILD)/tests/test_doc_io.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_llm: $(CORE_OBJS) $(LLM_OBJS) $(BUILD)/tests/test_state_fixture.o $(BUILD)/tests/test_llm.o
	$(CC) $(CFLAGS) $^ -o $@ $(MARIADB_LIBS) -lpthread

$(BUILD)/test_remote: $(CORE_OBJS) $(LLM_OBJS) $(BUILD)/tests/test_state_fixture.o $(BUILD)/tests/test_remote.o
	$(CC) $(CFLAGS) $^ -o $@ $(MARIADB_LIBS) -lpthread

$(BUILD)/test_components: $(CORE_OBJS) $(LLM_OBJS) $(BUILD)/tests/test_state_fixture.o $(BUILD)/tests/test_components.o
	$(CC) $(CFLAGS) $^ -o $@ $(MARIADB_LIBS) -lpthread

$(BUILD)/test_db_rows: $(BUILD)/src/app/db.o $(BUILD)/tests/test_db_rows.o
	$(CC) $(CFLAGS) $^ -o $@ $(MARIADB_LIBS)

$(BUILD)/test_observer_text: $(BUILD)/src/llm/observer_text.o $(BUILD)/tests/test_observer_text.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_chat_history: $(BUILD)/src/llm/chat_history.o $(BUILD)/tests/test_chat_history.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_chat_view: $(CORE_OBJS) $(LLM_OBJS) $(BUILD)/src/ui/chat_view.o $(BUILD)/src/ui/editor.o $(BUILD)/src/ui/input.o $(BUILD)/tests/raylib_stubs.o $(BUILD)/tests/test_chat_view.o
	$(CC) $(CFLAGS) $^ -o $@ $(MARIADB_LIBS) -lpthread -lm

CHAT_VIEW_FAULT_SRCS := $(CORE_SRCS) $(LLM_SRCS) src/core/json_vendor.c src/ui/chat_view.c src/ui/editor.c src/ui/input.c tests/raylib_stubs.c tests/test_chat_view.c
$(BUILD)/test_chat_view_faults: $(CHAT_VIEW_FAULT_SRCS) $(wildcard src/core/*.h src/app/*.h src/llm/*.h src/ui/*.h) tests/fault_alloc.h $(BUILD)/tests/fault_alloc.o | $(MARIADB_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -DCHAT_VIEW_FAULT_TEST -include tests/fault_alloc.h $(CHAT_VIEW_FAULT_SRCS) $(BUILD)/tests/fault_alloc.o -o $@ $(MARIADB_LIBS) -lpthread -lm

$(BUILD)/test_observer_fake: $(BUILD)/src/llm/observer_text.o $(BUILD)/src/llm/observer_fake.o $(BUILD)/third_party/cJSON/cJSON.o $(BUILD)/tests/test_observer_fake.o
	$(CC) $(CFLAGS) $^ -o $@

# The real editor, linked against headless raylib stubs.
$(BUILD)/test_editor: $(CORE_OBJS) $(BUILD)/src/ui/editor.o $(BUILD)/src/ui/input.o $(BUILD)/src/ui/review_view.o $(BUILD)/src/app/reviewed_edit_controller.o $(BUILD)/tests/raylib_stubs.o $(BUILD)/tests/test_editor.o
	$(CC) $(CFLAGS) $^ -o $@ -lm

# Every test runs under tools/watchdog.sh: a hang is killed after a time limit
# and its stacks are saved to build/hangs/ instead of stalling the run.
WATCHDOG := tools/watchdog.sh
TEST_TIMEOUT ?= 300

# Separate binary: deterministic allocation faults never affect other suites.
FAULT_SRCS := $(CORE_SRCS) $(LLM_SRCS) src/core/json_vendor.c tests/test_state_fixture.c tests/test_persistence_faults.c
$(BUILD)/test_persistence_faults: $(FAULT_SRCS) $(wildcard src/core/*.h src/app/*.h src/llm/*.h) tests/fault_alloc.h tests/test_state_fixture.h $(BUILD)/tests/fault_alloc.o third_party/cJSON/cJSON.c third_party/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INCLUDES) -include tests/fault_alloc.h $(FAULT_SRCS) $(BUILD)/tests/fault_alloc.o -o $@ $(MARIADB_LIBS) -lpthread

EDITOR_FAULT_SRCS := $(CORE_SRCS) src/ui/editor.c src/ui/input.c tests/raylib_stubs.c tests/test_editor_faults.c
$(BUILD)/test_editor_faults: $(EDITOR_FAULT_SRCS) $(wildcard src/core/*.h src/ui/*.h) src/app/frame_view_model.h tests/fault_alloc.h $(BUILD)/tests/fault_alloc.o third_party/stb/stb_textedit.h
	$(CC) $(CFLAGS) $(INCLUDES) -include tests/fault_alloc.h $(EDITOR_FAULT_SRCS) $(BUILD)/tests/fault_alloc.o -o $@ -lm

SEMANTIC_FAULT_SRCS := $(CORE_SRCS) $(LLM_SRCS) src/core/json_vendor.c tests/test_state_fixture.c tests/test_semantic_faults.c
$(BUILD)/test_semantic_faults: $(SEMANTIC_FAULT_SRCS) $(wildcard src/core/*.h src/app/*.h src/llm/*.h) tests/fault_alloc.h tests/test_state_fixture.h $(BUILD)/tests/fault_alloc.o third_party/cJSON/cJSON.c third_party/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INCLUDES) -include tests/fault_alloc.h $(SEMANTIC_FAULT_SRCS) $(BUILD)/tests/fault_alloc.o -o $@ $(MARIADB_LIBS) -lpthread

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $(WATCHDOG) $(TEST_TIMEOUT) $$(basename $$t) ./$$t; echo; done
	$(WATCHDOG) $(TEST_TIMEOUT) test_backup python3 tests/test_backup.py

# Verify pinned dependency inventory, upstream refs, container digests and OSV
# advisories. The scheduled workflow publishes the same report as an artifact.
dependency-report:
	python3 tools/dependency_report.py

# Scripted UI tests: each script drives the real app in a hidden window against
# its own copy of tests/ui/fixtures in build/ui/<script>/work, where its
# screenshots also land. build/ui is wiped (a fixed path) at the start.
UI_SCRIPTS := $(filter-out tests/ui/real-% tests/ui/case_sensitive.scribo,$(wildcard tests/ui/*.scribo))
# tests/ui/real-*.scribo drive the real model through the UI (make uitest-real).
REAL_UI_SCRIPTS := $(wildcard tests/ui/real-*.scribo)
uitest: $(APP)
	@chmod -R u+w build/ui 2>/dev/null; rm -rf build/ui
	@fail=0; for s in $(UI_SCRIPTS); do \
	  name=$$(basename $$s .scribo); work=build/ui/$$name/work; \
	  mkdir -p $$work && cp -R tests/ui/fixtures/. $$work/; \
	  SCRIBODO_SCRIPT=$$s SCRIBODO_SCRIPT_DIR=$$PWD/$$work SCRIBODO_SHOT_DIR=$$PWD/build/ui/$$name \
	    $(WATCHDOG) 120 ui-$$name ./$(APP) 2>build/ui/$$name/stderr.log || fail=1; \
	  echo; \
	done; exit $$fail

# --- Project store (MariaDB client) ---------------------------------------------
# Connector/C from the pinned submodule, static, against Homebrew's static
# OpenSSL (the binary then needs only system libraries).
OPENSSL_DIR ?= /opt/homebrew/opt/openssl@3
# Built once under build/ (shared by the sanitizer builds).
MARIADB_LIB := build/mariadb-cc/libmariadb/libmariadbclient.a
MARIADB_LIBS := $(MARIADB_LIB) $(OPENSSL_DIR)/lib/libssl.a $(OPENSSL_DIR)/lib/libcrypto.a -lz -liconv -lcurl # the system libcurl, for the optional remote model
# Everything that may include <mysql.h> needs the client built first.
$(LLM_OBJS) $(APP_OBJS) $(BUILD)/tests/test_llm.o $(BUILD)/tests/test_db.o: | $(MARIADB_LIB)
$(BUILD)/test_persistence_faults $(BUILD)/test_semantic_faults: | $(MARIADB_LIB)
$(MARIADB_LIB):
	@mkdir -p $(dir $@)
	$(CMAKE) -S third_party/mariadb-connector-c -B build/mariadb-cc -DCMAKE_BUILD_TYPE=Release \
	  -DWITH_UNIT_TESTS=OFF -DWITH_SSL=OPENSSL -DOPENSSL_ROOT_DIR=$(OPENSSL_DIR) -DOPENSSL_USE_STATIC_LIBS=TRUE \
	  -DCLIENT_PLUGIN_DIALOG=STATIC -DCLIENT_PLUGIN_AUTH_GSSAPI_CLIENT=OFF -DCLIENT_PLUGIN_REMOTE_IO=OFF \
	  -DWITH_CURL=OFF -DWITH_EXTERNAL_ZLIB=ON > build/mariadb-cc.log
	$(CMAKE) --build build/mariadb-cc --target mariadbclient -j8 >> build/mariadb-cc.log

$(BUILD)/test_db: $(CORE_OBJS) $(LLM_OBJS) $(BUILD)/tests/test_db_core.o $(BUILD)/tests/test_db.o
	$(CC) $(CFLAGS) $^ -o $@ $(MARIADB_LIBS) -lpthread

# Against the running local services (tools/services.sh up).
dbtest: $(BUILD)/test_db
	$(WATCHDOG) 120 dbtest ./$(BUILD)/test_db

# The app against the project store (needs tools/services.sh up): three runs
# on one throwaway database and project folder: move and use, resume after
# a restart, and the database unreachable.
uitest-db: $(APP)
	@db=scribodo_test_ui$$$$; work=$$PWD/build/ui/store/work; chmod -R u+w build/ui/store 2>/dev/null; rm -rf build/ui/store; \
	  mkdir -p $$work; tools/services.sh testdb create $$db >/dev/null || exit 1; fail=0; \
	  for s in tests/ui/store/1_move_and_use.scribo tests/ui/store/2_resume.scribo; do \
	    SCRIBODO_STORE=db SCRIBODO_DB_NAME=$$db SCRIBODO_SCRIPT=$$s SCRIBODO_SCRIPT_DIR=$$work SCRIBODO_SHOT_DIR=$$PWD/build/ui/store \
	      $(WATCHDOG) 180 ui-store ./$(APP) 2>>build/ui/store/stderr.log || fail=1; echo; \
	  done; \
	  python3 tests/seed_chat_window.py $$work/windowbook || fail=1; \
	  SCRIBODO_STORE=db SCRIBODO_DB_NAME=$$db SCRIBODO_SCRIPT=tests/ui/store/7_chat_windows.scribo SCRIBODO_SCRIPT_DIR=$$work \
	    SCRIBODO_SHOT_DIR=$$PWD/build/ui/store $(WATCHDOG) 180 ui-store-windows ./$(APP) 2>>build/ui/store/stderr.log || fail=1; \
	  SCRIBODO_STORE=db SCRIBODO_DB_NAME=$$db SCRIBODO_BACKUPS=$$work/backups SCRIBODO_SCRIPT=tests/ui/store/6_backup.scribo SCRIBODO_SCRIPT_DIR=$$work \
	    SCRIBODO_SHOT_DIR=$$PWD/build/ui/store $(WATCHDOG) 240 ui-store ./$(APP) 2>>build/ui/store/stderr.log || fail=1; echo; \
	  SCRIBODO_STORE=db SCRIBODO_DB_NAME=$$db SCRIBODO_SCRIPT=tests/ui/store/5_entities.scribo SCRIBODO_SCRIPT_DIR=$$work \
	    SCRIBODO_SHOT_DIR=$$PWD/build/ui/store $(WATCHDOG) 180 ui-store ./$(APP) 2>>build/ui/store/stderr.log || fail=1; echo; \
	  cp -R $$work/book $$work/book-copy; \
	  SCRIBODO_STORE=db SCRIBODO_DB_NAME=$$db SCRIBODO_SCRIPT=tests/ui/store/4_copy.scribo SCRIBODO_SCRIPT_DIR=$$work \
	    SCRIBODO_SHOT_DIR=$$PWD/build/ui/store $(WATCHDOG) 180 ui-store ./$(APP) 2>>build/ui/store/stderr.log || fail=1; echo; \
	  SCRIBODO_STORE=db SCRIBODO_DB_PORT=1 SCRIBODO_SCRIPT=tests/ui/store/3_paused.scribo SCRIBODO_SCRIPT_DIR=$$work \
	    SCRIBODO_SHOT_DIR=$$PWD/build/ui/store $(WATCHDOG) 180 ui-store ./$(APP) 2>>build/ui/store/stderr.log || fail=1; \
	  tools/services.sh testdb drop $$db >/dev/null; exit $$fail

# Acceptance 8: the store suite and the real models inside a sandbox with
# no network but this machine. Needs the real model to itself.
offlinetest: $(APP) $(BUILD)/test_llm $(LLAMA_BIN)
	tools/offlinetest.sh

# The optional remote model, against a stand-in provider on 127.0.0.1.
remotetest: $(APP)
	tools/remotetest.sh

# Backup and recovery: back up, lose everything, restore, resume.
backuptest: $(APP)
	tools/backuptest.sh

# Rename safety on a case-sensitive volume (builds a temporary disk image).
cstest: $(APP)
	tools/cstest.sh

# --- Local model runtime -------------------------------------------------------
# llama-server from the pinned submodule, built static so build/llama/bin is
# self-contained. Uses a system cmake, or uvx's if there is none.
CMAKE ?= $(shell command -v cmake 2>/dev/null || echo "uvx --from cmake cmake")
LLAMA_BIN := $(BUILD)/llama/bin/llama-server
llama: $(LLAMA_BIN)
$(LLAMA_BIN):
	$(CMAKE) -S third_party/llama.cpp -B $(BUILD)/llama -DCMAKE_BUILD_TYPE=Release \
	  -DGGML_METAL=ON -DGGML_NATIVE=ON -DBUILD_SHARED_LIBS=OFF -DLLAMA_CURL=OFF \
	  -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF \
	  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON \
	  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(abspath $(BUILD)/llama/bin)
	$(CMAKE) --build $(BUILD)/llama --target llama-server -j 10

# The pinned BF16 GGUF (docs/MODEL-001). The app also finds an existing copy
# in the Hugging Face cache, so this only downloads when neither exists.
MODEL_DIR    := $(HOME)/Library/Application Support/Scribodo/models
MODEL_FILE   := $(MODEL_DIR)/LFM3-1.7B-BF16.gguf
MODEL_URL    := https://huggingface.co/LiquidAI/LFM3-1.7B-GGUF/resolve/cd0c74c575b5c416f3011b5b4c298077f26ca8c2/LFM3-1.7B-BF16.gguf
MODEL_SHA256 := 39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68
HF_MODEL     := $(HOME)/.cache/huggingface/hub/models--LiquidAI--LFM3-1.7B-GGUF/snapshots/cd0c74c575b5c416f3011b5b4c298077f26ca8c2/LFM3-1.7B-BF16.gguf
model:
	@if [ -f "$(HF_MODEL)" ]; then echo "Using the Hugging Face cache copy: $(HF_MODEL)"; \
	elif [ -f "$(MODEL_FILE)" ]; then echo "Model present: $(MODEL_FILE)"; \
	else mkdir -p "$(MODEL_DIR)" && curl -fL --progress-bar -o "$(MODEL_FILE).part" "$(MODEL_URL)" && \
	  echo "$(MODEL_SHA256)  $(MODEL_FILE).part" | shasum -a 256 -c - && mv "$(MODEL_FILE).part" "$(MODEL_FILE)"; fi

llmtest: $(BUILD)/test_llm $(LLAMA_BIN)
	SCRIBODO_LLAMA_SERVER=$(abspath $(LLAMA_BIN)) $(WATCHDOG) 600 llmtest ./$(BUILD)/test_llm --live

uitest-real: $(APP) $(LLAMA_BIN)
	@fail=0; for s in $(REAL_UI_SCRIPTS); do \
	  name=$$(basename $$s .scribo); work=build/ui/$$name/work; rm -rf build/ui/$$name; \
	  mkdir -p $$work && cp -R tests/ui/fixtures/. $$work/; \
	  SCRIBODO_LLM=real SCRIBODO_SEED=42 SCRIBODO_SCRIPT=$$s SCRIBODO_SCRIPT_DIR=$$PWD/$$work SCRIBODO_SHOT_DIR=$$PWD/build/ui/$$name \
	    $(WATCHDOG) 600 ui-$$name ./$(APP) 2>build/ui/$$name/stderr.log || fail=1; echo; \
	done; exit $$fail

SAN_FLAGS := -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
sanitize:
	$(MAKE) --no-print-directory BUILD=build/asan OPT="$(SAN_FLAGS)" test

# ThreadSanitizer over the threaded code (chat worker, observer, supervisor).
tsan: $(MARIADB_LIB) $(BUILD)/tests/fault_alloc.o
	@mkdir -p build/tsan
	$(CC) -std=c11 -O1 -g -fsanitize=thread $(INCLUDES) $(CORE_SRCS) $(LLM_SRCS) src/core/json_vendor.c \
	  tests/test_state_fixture.c tests/test_llm.c -o build/tsan/test_llm $(MARIADB_LIBS) -lpthread
	$(CC) -std=c11 -O1 -g -fsanitize=thread $(INCLUDES) $(CORE_SRCS) $(LLM_SRCS) src/core/json_vendor.c \
	  tests/test_state_fixture.c tests/test_components.c -o build/tsan/test_components $(MARIADB_LIBS) -lpthread
	$(CC) -std=c11 -O1 -g -fsanitize=thread $(INCLUDES) $(CORE_SRCS) $(LLM_SRCS) src/core/json_vendor.c \
	  tests/test_state_fixture.c tests/test_remote.c -o build/tsan/test_remote $(MARIADB_LIBS) -lpthread
	$(CC) -std=c11 -O1 -g -fsanitize=thread $(INCLUDES) -include tests/fault_alloc.h $(SEMANTIC_FAULT_SRCS) \
	  $(BUILD)/tests/fault_alloc.o -o build/tsan/test_semantic_faults $(MARIADB_LIBS) -lpthread
	@set -e; for t in test_llm test_components test_remote test_semantic_faults; do \
	  TSAN_OPTIONS=halt_on_error=1 $(WATCHDOG) 300 tsan-$$t ./build/tsan/$$t; echo; \
	done

run: $(APP)
	./$(APP) assets/sample/sample.md

$(BUILD)/raylib $(BUILD)/src/ui:
	@mkdir -p $@

clean:
	rm -rf $(BUILD)/src $(BUILD)/tests $(APP) $(TESTS) build/asan build/ui

distclean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
