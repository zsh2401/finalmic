NAME      := finalmic
BIN_DIR   := bin
BIN       := $(BIN_DIR)/$(NAME)
GO        ?= go
DRIVER_BUNDLE := build/finalmic.driver
HAL_DIR       := /Library/Audio/Plug-Ins/HAL
ARGS      ?=
VERSION   ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
LDFLAGS   := -s -w -X github.com/zsh2401/finalmic/internal/cli.Version=$(VERSION)

.PHONY: all build driver test-driver install-driver uninstall-driver run setup devices doctor test fmt vet clean install help

all: build

## build: 构建当前平台的 bin/finalmic
build:
	@if [ "$$(uname -s)" != "Darwin" ]; then echo "finalmic 只支持 macOS"; exit 1; fi
	@mkdir -p $(BIN_DIR)
	CGO_ENABLED=1 $(GO) build -trimpath -ldflags "$(LDFLAGS)" -o $(BIN) ./cmd/$(NAME)
	@echo "  ✓ $(BIN)  ($(VERSION))"

## driver: 构建 finalmic.driver（虚拟声卡驱动）
driver:
	@mkdir -p $(DRIVER_BUNDLE)/Contents/MacOS
	@cp driver/Info.plist $(DRIVER_BUNDLE)/Contents/Info.plist
	clang -bundle -O2 -Wall -mmacosx-version-min=11.0 -o $(DRIVER_BUNDLE)/Contents/MacOS/finalmic \
		driver/finalmic.c -framework CoreAudio -framework CoreFoundation
	@codesign --force --sign - $(DRIVER_BUNDLE)
	@echo "  ✓ $(DRIVER_BUNDLE)"

## test-driver: 离线验证驱动（不需要安装）
test-driver: driver
	@clang -O2 -Wall -o build/test_host driver/test_host.c \
		-framework CoreAudio -framework CoreFoundation -ldl
	@./build/test_host $(DRIVER_BUNDLE)/Contents/MacOS/finalmic

## install-driver: 安装虚拟声卡到系统（会要一次管理员密码）
install-driver: driver
	@sudo mkdir -p $(HAL_DIR)
	@sudo rm -rf $(HAL_DIR)/finalmic.driver
	@sudo cp -R $(DRIVER_BUNDLE) $(HAL_DIR)/finalmic.driver
	@sudo chown -R root:wheel $(HAL_DIR)/finalmic.driver
	@echo "  → 重启 coreaudiod…"
	@sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || \
		sudo killall -9 coreaudiod 2>/dev/null || true
	@sleep 1
	@echo "  ✓ 已安装 /Library/Audio/Plug-Ins/HAL/finalmic.driver"
	@echo "    运行 make devices 应该能看到 finalmic"

## uninstall-driver: 卸载虚拟声卡
uninstall-driver:
	@sudo rm -rf $(HAL_DIR)/finalmic.driver
	@sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || \
		sudo killall -9 coreaudiod 2>/dev/null || true
	@sleep 1
	@echo "  ✓ 已卸载"

## run: 启动混音（首次运行进入配置向导）
run: build
	@./$(BIN) $(ARGS)

## setup: 重新选择麦克风 / 采集卡 / 虚拟声卡
setup: build
	@./$(BIN) setup

## devices: 列出所有 CoreAudio 设备
devices: build
	@./$(BIN) devices

## doctor: 检查设备、权限和电平
doctor: build
	@./$(BIN) doctor

## test: 运行测试
test:
	CGO_ENABLED=1 $(GO) test ./...

## fmt: 格式化 Go 代码
fmt:
	@gofmt -w $$(find . -name '*.go' -not -path './bin/*')

## vet: 静态检查
vet:
	CGO_ENABLED=1 $(GO) vet ./...

## install: 安装到 $GOBIN（或 $GOPATH/bin）
install:
	CGO_ENABLED=1 $(GO) install -trimpath -ldflags "$(LDFLAGS)" ./cmd/$(NAME)

## clean: 清理构建产物
clean:
	@rm -rf $(BIN_DIR) build/

## help: 显示可用目标
help:
	@echo "finalmic $(VERSION)"
	@echo
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  make /'
