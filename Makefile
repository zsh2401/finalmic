NAME      := finalmic
BIN_DIR   := bin
BIN       := $(BIN_DIR)/$(NAME)
GO        ?= go
ARGS      ?=
VERSION   ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
LDFLAGS   := -s -w -X github.com/zsh2401/finalmic/internal/cli.Version=$(VERSION)

.PHONY: all build run setup devices doctor test fmt vet clean install help

all: build

## build: 构建当前平台的 bin/finalmic
build:
	@if [ "$$(uname -s)" != "Darwin" ]; then echo "finalmic 只支持 macOS"; exit 1; fi
	@mkdir -p $(BIN_DIR)
	CGO_ENABLED=1 $(GO) build -trimpath -ldflags "$(LDFLAGS)" -o $(BIN) ./cmd/$(NAME)
	@echo "  ✓ $(BIN)  ($(VERSION))"

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
	@rm -rf $(BIN_DIR)

## help: 显示可用目标
help:
	@echo "finalmic $(VERSION)"
	@echo
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  make /'
