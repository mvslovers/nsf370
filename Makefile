# nsf370 -- Network Services Facility for MVS 3.8j
# Build orchestration via MBT V2 (MVS Build Tools).
#
# Targets come from mbt/mk/mbt.mk (run `make help` for the full list):
#   make test-host    build + run the portable tests natively (no MVS)
#   make deps         resolve deps (cc370 sysroot, macros), allocate datasets
#   make test-mvs     deploy + run the tests on MVS
#   make modules      cross-compile + assemble the load modules on MVS
#   make package      TRANSMIT/XMIT the load library for download
#   make deploy       upload modules + RECV370 on MVS
# (no bootstrap/build/link target; deps is the former bootstrap)
#
# First-time setup:  git submodule update --init  &&  cp .env.example .env
MBT_ROOT := mbt
include $(MBT_ROOT)/mk/mbt.mk

# --- Local MVS for `make test-mvs` (project convenience) --------------
# Spins up an MVS/CE instance in Docker (port 1080 = MBT_MVS_PORT). Not
# required for `make test-host`, which runs the portable tests natively.
DOCKER_NETWORK ?= mvs-net
MVS_CONTAINER  ?= mvs
MVS_IMAGE      ?= ghcr.io/mvslovers/mvsce-builder

run-mvs:
	@if ! command -v docker >/dev/null 2>&1; then \
		echo "ERROR: docker not found"; exit 1; \
	fi; \
	docker network inspect $(DOCKER_NETWORK) >/dev/null 2>&1 || \
		docker network create $(DOCKER_NETWORK) >/dev/null; \
	if docker inspect $(MVS_CONTAINER) >/dev/null 2>&1; then \
		docker start $(MVS_CONTAINER); \
	else \
		echo "Creating $(MVS_CONTAINER) from $(MVS_IMAGE) (this may take a while)..."; \
		docker run -d --name $(MVS_CONTAINER) --network $(DOCKER_NETWORK) \
			-p 1080:1080 -p 3270:3270 -p 8888:8888 $(MVS_IMAGE); \
	fi
.PHONY: run-mvs

stop-mvs:
	@docker inspect $(MVS_CONTAINER) >/dev/null 2>&1 && docker stop $(MVS_CONTAINER) || echo "$(MVS_CONTAINER) not running"
.PHONY: stop-mvs

