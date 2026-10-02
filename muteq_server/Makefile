SHELL := /bin/bash

CXX ?= clang++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor
INCLUDES := -Isrc
LDFLAGS := -lsqlite3 -lpthread

# On Linux, link OpenSSL for HMAC verification
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lssl -lcrypto
endif

BUILD_DIR := build
BIN_DIR := bin
TARGET := $(BIN_DIR)/muteq_server

SRCS := src/main.cpp
OBJS := $(SRCS:src/%.cpp=$(BUILD_DIR)/%.o)
HEADERS := $(wildcard src/*.hpp) $(wildcard src/*.h)

# ── AWS & CloudFormation Configuration ────────────────────────────────────────
STACK_NAME    ?= muteq-dashboard
DOMAIN        ?= www.hoongram.com
TEMPLATE_FILE ?= $(shell test -f cloudformation.yml && echo cloudformation.yml || echo infra/cloudformation.yml)
EC2_USER      ?= ec2-user
EC2_KEY       ?= ~/.ssh/muteq.pem
EC2_HOST      ?= $(shell aws cloudformation describe-stacks --stack-name $(STACK_NAME) --region us-east-1 --query 'Stacks[0].Outputs[?OutputKey==`ElasticIP`].OutputValue' --output text 2>/dev/null || echo 32.195.148.160)

.PHONY: all build debug release clean run help docker-build export-bin export-bin-arm64 aws-deploy aws-status aws-delete aws-ssl-renew aws-ssl-status

all: build

build: release

debug: CXXFLAGS += -O0 -g
debug: $(TARGET)

release: CXXFLAGS += -O3 -DNDEBUG
release: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts."

run: build
	./$(TARGET)

# Build Amazon Linux 2023 container image for EC2
docker-build:
	docker build -t muteq-server:latest .

# Build and export standalone Linux x86_64 ELF binary
export-bin:
	@mkdir -p bin/linux_x86_64
	@if command -v container >/dev/null 2>&1; then \
		echo "Building with macOS container..."; \
		container build --platform linux/amd64 --target export -o type=local,dest=bin/linux_x86_64 -f Dockerfile . && \
		find bin/linux_x86_64 -type f -name muteq_server -exec mv -f {} bin/linux_x86_64/muteq_server \; && \
		rm -rf bin/linux_x86_64/out.tar bin/linux_x86_64/linux_amd64 bin/linux_x86_64/linux_x86_64 2>/dev/null || true; \
	elif command -v docker >/dev/null 2>&1; then \
		DOCKER_BUILDKIT=1 docker build --platform linux/amd64 --target export --output type=local,dest=bin/linux_x86_64 -f Dockerfile .; \
	fi
	@echo "Exported binary: bin/linux_x86_64/muteq_server"

# Build and export standalone Linux ARM64 (Graviton) ELF binary
export-bin-arm64:
	@mkdir -p bin/linux_arm64
	@if command -v container >/dev/null 2>&1; then \
		echo "Building with macOS container..."; \
		container build --platform linux/arm64 --target export -o type=local,dest=bin/linux_arm64 -f Dockerfile . && \
		find bin/linux_arm64 -type f -name muteq_server -exec mv -f {} bin/linux_arm64/muteq_server \; && \
		rm -rf bin/linux_arm64/out.tar bin/linux_arm64/linux_arm64 2>/dev/null || true; \
	elif command -v docker >/dev/null 2>&1; then \
		DOCKER_BUILDKIT=1 docker build --platform linux/arm64 --target export --output type=local,dest=bin/linux_arm64 -f Dockerfile .; \
	fi
	@echo "Exported binary: bin/linux_arm64/muteq_server"

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
	@echo "Available make targets for muteq_server:"
	@echo "  make build             Build release server binary ($(TARGET))"
	@echo "  make debug             Build with debug symbols (-g -O0)"
	@echo "  make run               Build and run the server locally"
	@echo "  make docker-build      Build Amazon Linux 2023 container image"
	@echo "  make export-bin        Export Linux x86_64 ELF binary via Docker"
	@echo "  make export-bin-arm64  Export Linux ARM64 (Graviton) ELF binary via Docker"
	@echo "  make clean             Remove build and binary directories"
	@echo "  make aws-deploy        Deploy (or update) the EC2 dashboard CloudFormation stack"
	@echo "  make aws-status        Show current CloudFormation stack status and outputs"
	@echo "  make aws-delete        Tear down the CloudFormation stack"
	@echo "  make aws-ssl-renew     Renew Let's Encrypt SSL certificate on EC2 (FORCE=1 to force)"
	@echo "  make aws-ssl-status    Show Let's Encrypt SSL certificate expiry and details on EC2"
