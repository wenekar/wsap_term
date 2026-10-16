ifeq ($(shell uname -s),Darwin)
BUILD_DIR ?= build-homebrew
LLVM_BIN ?= $(shell brew --prefix llvm)/bin
MESON_ENV ?= CC=$(LLVM_BIN)/clang OBJC=$(LLVM_BIN)/clang AR=$(LLVM_BIN)/llvm-ar NM=$(LLVM_BIN)/llvm-nm
else
BUILD_DIR ?= build
MESON_ENV ?=
endif
PREFIX ?= $(HOME)/.local
MESON_SETUP_FLAGS ?= --prefix=$(PREFIX) --buildtype=release -Doptimization=3 -Db_lto=true -Disa=native
TEST_BUILD_DIR ?= $(BUILD_DIR)-test
TEST_SETUP_FLAGS ?= $(MESON_SETUP_FLAGS) -Dtests=true -Dbenchmarks=false -Db_sanitize=none
DIST_BUILD_DIR ?= $(BUILD_DIR)-dist
DIST_ISA ?= $(if $(filter x86_64 amd64,$(shell uname -m)),x86-64-v2,)
DIST_SETUP_FLAGS ?= $(filter-out -Disa=%,$(MESON_SETUP_FLAGS)) \
	-Dtests=true -Dbenchmarks=false -Disa=$(DIST_ISA)
ifeq ($(shell uname -s),Linux)
TEST_SETUP_FLAGS += -Dintegration_tests=true -Dkitty_tests=true
endif

SAN_SETUP_FLAGS ?= --buildtype=debugoptimized -Db_lto=false -Disa=native

.PHONY: all release test test-fuzz install dist dist-local clean test-asan test-tsan test-msan test-sanitizers analyze analyze-deep

all: $(BUILD_DIR)/build.ninja
	meson compile -C $(BUILD_DIR)

$(BUILD_DIR)/build.ninja:
	$(MESON_ENV) meson setup $(BUILD_DIR) $(MESON_SETUP_FLAGS)

test: $(TEST_BUILD_DIR)/build.ninja
	meson test -C "$(TEST_BUILD_DIR)" --print-errorlogs

test-fuzz: $(TEST_BUILD_DIR)/build.ninja
	meson compile -C "$(TEST_BUILD_DIR)" write-fuzz-test
	env -u LD_PRELOAD -u DYLD_INSERT_LIBRARIES WSAP_NO_PRELOAD=1 "$(TEST_BUILD_DIR)/tests/write-fuzz-test" 50000

$(TEST_BUILD_DIR)/build.ninja:
	$(MESON_ENV) meson setup "$(TEST_BUILD_DIR)" $(TEST_SETUP_FLAGS)

release: $(BUILD_DIR)/build.ninja
	meson compile -C $(BUILD_DIR) install-targets

install: release
	@printf "Install wsap_term locally to %s? [y/N] " "$(PREFIX)"; \
	read confirm || confirm=; \
	case "$$confirm" in \
		[yY]|[yY][eE][sS]) meson install -C "$(BUILD_DIR)" ;; \
		*) echo "Installation cancelled."; exit 1 ;; \
	esac

dist: $(DIST_BUILD_DIR)/build.ninja
	meson compile -C "$(DIST_BUILD_DIR)" install-targets
	meson dist -C "$(DIST_BUILD_DIR)"

$(DIST_BUILD_DIR)/build.ninja:
	$(MESON_ENV) meson setup "$(DIST_BUILD_DIR)" $(DIST_SETUP_FLAGS)

dist-local: $(DIST_BUILD_DIR)/build.ninja
	meson compile -C "$(DIST_BUILD_DIR)" install-targets
	meson dist -C "$(DIST_BUILD_DIR)" --allow-dirty --no-tests

test-asan: $(BUILD_DIR)-asan/build.ninja
	env -u LD_PRELOAD -u DYLD_INSERT_LIBRARIES meson test -C $(BUILD_DIR)-asan --setup=asan

$(BUILD_DIR)-asan/build.ninja:
	$(MESON_ENV) meson setup "$(BUILD_DIR)-asan" $(SAN_SETUP_FLAGS) -Db_sanitize=address,undefined

test-tsan: $(BUILD_DIR)-tsan/build.ninja
	env -u LD_PRELOAD -u DYLD_INSERT_LIBRARIES meson test -C $(BUILD_DIR)-tsan --setup=tsan

$(BUILD_DIR)-tsan/build.ninja:
	$(MESON_ENV) meson setup "$(BUILD_DIR)-tsan" $(SAN_SETUP_FLAGS) -Db_sanitize=thread

test-msan: $(BUILD_DIR)-msan/build.ninja
	env -u LD_PRELOAD -u DYLD_INSERT_LIBRARIES meson test -C $(BUILD_DIR)-msan --setup=msan

$(BUILD_DIR)-msan/build.ninja:
	$(MESON_ENV) meson setup "$(BUILD_DIR)-msan" $(SAN_SETUP_FLAGS) -Db_sanitize=memory

test-sanitizers: test-asan test-msan test-tsan

analyze:
	./tools/analyze.sh

analyze-deep:
	./tools/analyze.sh --deep

clean:
	rm -rf $(BUILD_DIR) $(TEST_BUILD_DIR) $(DIST_BUILD_DIR) \
		$(BUILD_DIR)-asan $(BUILD_DIR)-tsan $(BUILD_DIR)-msan dist
