SHELL := /bin/bash

# Compiler & Flags
CXX ?= clang++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor
INCLUDES := -Isrc
LDFLAGS := -lsqlite3 -lpthread -lcurl

# Directories & Target
BUILD_DIR := build
BIN_DIR := bin
TARGET := $(BIN_DIR)/muteq_sensor

# Sources and headers
SRCS := src/main.cpp
OBJS := $(SRCS:%.cpp=$(BUILD_DIR)/%.o)
HEADERS := $(wildcard src/*.hpp)
ALL_CODE := $(SRCS) $(HEADERS)

# ── AWS & CloudFormation Configuration ────────────────────────────────────────
STACK_NAME    ?= muteq-dashboard
DOMAIN        ?= www.hoongram.com
TEMPLATE_FILE ?= $(shell test -f cloudformation.yml && echo cloudformation.yml || echo ../muteq_server/infra/cloudformation.yml)
EC2_USER      ?= ec2-user
EC2_KEY       ?= ~/.ssh/muteq.pem
EC2_HOST      ?= $(shell aws cloudformation describe-stacks --stack-name $(STACK_NAME) --region us-east-1 --query 'Stacks[0].Outputs[?OutputKey==`ElasticIP`].OutputValue' --output text 2>/dev/null || echo 32.195.148.160)

.PHONY: all build debug release sanitize clean format format-check run help cross-pi build-pi mock-meter mock aws-deploy aws-status aws-delete aws-ssl-renew aws-ssl-status

all: build

build: release

# Debug build with symbols
debug: CXXFLAGS += -O0 -g
debug: $(TARGET)

# Release build with optimizations
release: CXXFLAGS += -O3 -DNDEBUG
release: $(TARGET)

# Build with Address and Undefined Behavior Sanitizers
sanitize: CXXFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Build mock SPL meter utility
mock-meter: $(BIN_DIR)/mock_spl_meter

$(BIN_DIR)/mock_spl_meter: tools/mock_spl_meter.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@

mock: mock-meter
	./$(BIN_DIR)/mock_spl_meter

# In-place code formatting using .clang-format
format:
	clang-format -i $(ALL_CODE)
	@echo "Formatted source and header files."

# Check code formatting without modifying files
format-check:
	clang-format --dry-run --Werror $(ALL_CODE)
	@echo "All files conform to clang-format rules."

# Remove build artifacts
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts."

# Build and execute
run: build
	./$(TARGET)

# Build Linux ARM64 ELF binary for Raspberry Pi (using macOS container or Docker)
cross-pi build-pi:
	@rm -rf bin/linux_arm64
	@mkdir -p bin/linux_arm64
	@if command -v container >/dev/null 2>&1; then \
		echo "Building with macOS container..."; \
		container build --platform linux/arm64 --target export -o type=local,dest=bin/linux_arm64 -f Dockerfile . && \
		BIN_FOUND=$$(find bin/linux_arm64 -type f -name muteq_sensor | head -n 1); \
		if [ -n "$$BIN_FOUND" ] && [ "$$BIN_FOUND" != "bin/linux_arm64/muteq_sensor" ]; then \
			mv -f "$$BIN_FOUND" bin/linux_arm64/muteq_sensor && \
			find bin/linux_arm64 -mindepth 1 -maxdepth 1 ! -name muteq_sensor -exec rm -rf {} +; \
		fi; \
	elif command -v docker >/dev/null 2>&1; then \
		echo "Building with Docker BuildKit..."; \
		DOCKER_BUILDKIT=1 docker build --platform linux/arm64 --target export --output type=local,dest=bin/linux_arm64 -f Dockerfile .; \
	else \
		echo "Error: neither 'container' nor 'docker' CLI found" >&2; exit 1; \
	fi
	@echo "Successfully built Raspberry Pi Linux ARM64 binary: bin/linux_arm64/muteq_sensor"

# ── AWS ───────────────────────────────────────────────────────────────────────
# Deploy the EC2/nginx/Let's Encrypt stack to us-east-1.
# Required:
#   HostedZoneId  — Route53 hosted zone ID for hoongram.com
#   KeyPairName   — EC2 key pair name (must exist in us-east-1)
#   HmacSecret    — shared secret; must match server_hmac_secret in client_config.json
#   AcmeEmail     — email for Let's Encrypt cert notifications
# Optional:
#   GitRepoUrl    — git repo to clone on first boot (e.g. https://github.com/you/mute.git)
#
#   make aws-deploy HostedZoneId=ZXXX KeyPairName=my-key HmacSecret=... AcmeEmail=you@example.com

.PHONY: aws-deploy
aws-deploy: ## Deploy (or update) the EC2 dashboard CloudFormation stack
	@: $${HostedZoneId:?'HostedZoneId is required'}
	@: $${KeyPairName:?'KeyPairName is required'}
	@: $${HmacSecret:?'HmacSecret is required'}
	@: $${AcmeEmail:?'AcmeEmail is required'}
	aws cloudformation deploy \
	  --stack-name $(STACK_NAME) \
	  --template-file $(TEMPLATE_FILE) \
	  --region us-east-1 \
	  --capabilities CAPABILITY_NAMED_IAM \
	  --parameter-overrides \
	    DomainName=$(DOMAIN) \
	    HostedZoneId=$(HostedZoneId) \
	    KeyPairName=$(KeyPairName) \
	    HmacSecret=$(HmacSecret) \
	    AcmeEmail=$(AcmeEmail) \
	    $(if $(GitRepoUrl),GitRepoUrl=$(GitRepoUrl),)
	@echo ""
	@echo "✅ Stack deployed. Outputs:"
	@aws cloudformation describe-stacks \
	  --stack-name $(STACK_NAME) \
	  --region us-east-1 \
	  --query 'Stacks[0].Outputs' \
	  --output table

.PHONY: aws-status
aws-status: ## Show current CloudFormation stack status and outputs
	@aws cloudformation describe-stacks \
	  --stack-name $(STACK_NAME) \
	  --region us-east-1 \
	  --query 'Stacks[0].{Status:StackStatus,Outputs:Outputs}' \
	  --output table

.PHONY: aws-delete
aws-delete: ## Tear down the CloudFormation stack (prompts for confirmation)
	@read -p "Delete stack '$(STACK_NAME)'? This is irreversible. [y/N] " confirm; \
	  [[ "$$confirm" == "y" || "$$confirm" == "Y" ]] || (echo "Aborted."; exit 1)
	aws cloudformation delete-stack --stack-name $(STACK_NAME) --region us-east-1
	@echo "Waiting for deletion..."
	aws cloudformation wait stack-delete-complete --stack-name $(STACK_NAME) --region us-east-1
	@echo "✅ Stack deleted."

.PHONY: aws-ssl-renew
aws-ssl-renew: ## Renew Let's Encrypt SSL certificate on EC2 (optional: FORCE=1) and reload nginx
	@echo "Renewing SSL certificate on EC2 ($(EC2_HOST))..."
	ssh -i $(EC2_KEY) -o StrictHostKeyChecking=no $(EC2_USER)@$(EC2_HOST) \
	  "sudo /usr/local/bin/certbot renew $(if $(FORCE),--force-renewal,) && sudo systemctl reload nginx"
	@echo "✅ SSL certificate renewed and nginx reloaded."

.PHONY: aws-ssl-status
aws-ssl-status: ## Check Let's Encrypt SSL certificate status and expiration on EC2
	@ssh -i $(EC2_KEY) -o StrictHostKeyChecking=no $(EC2_USER)@$(EC2_HOST) \
	  "sudo /usr/local/bin/certbot certificates"

help:
	@echo "Available make targets:"
	@echo "  make build          Build macOS binary ($(TARGET))"
	@echo "  make debug          Build with debug symbols (-g -O0)"
	@echo "  make release        Build optimized release binary (-O3 -DNDEBUG)"
	@echo "  make cross-pi       Cross-compile Linux ARM64 ELF for Raspberry Pi via Docker"
	@echo "  make sanitize       Build with ASan and UBSan (-fsanitize=address,undefined)"
	@echo "  make mock           Build and run the mock SPL hardware meter"
	@echo "  make format         Format all .cpp and .hpp files with clang-format"
	@echo "  make format-check   Verify formatting without modifying files"
	@echo "  make clean          Remove build and binary directories"
	@echo "  make run            Build and run the binary"
	@echo "  make aws-deploy     Deploy (or update) the EC2 dashboard CloudFormation stack"
	@echo "  make aws-status     Show current CloudFormation stack status and outputs"
	@echo "  make aws-delete     Tear down the CloudFormation stack"
	@echo "  make aws-ssl-renew  Renew Let's Encrypt SSL certificate on EC2 (FORCE=1 to force)"
	@echo "  make aws-ssl-status Show Let's Encrypt SSL certificate expiry and details on EC2"
