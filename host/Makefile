# Desktop (Linux/WSL) build of the NXPlex core: CLI test harness + unit tests.
# mbedTLS 2.28 (same version as devkitPro's switch-mbedtls) is downloaded to
# host/third_party/mbedtls by tools/fetch-mbedtls.sh on first build.

CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CXXFLAGS += -std=c++17
MBED     := third_party/mbedtls
INC      := -I$(MBED)/include -I../core -I../common
LIBS     := $(MBED)/library/libmbedtls.a $(MBED)/library/libmbedx509.a $(MBED)/library/libmbedcrypto.a

CORE_SRC := $(wildcard ../core/*.cpp)
CORE_OBJ := $(patsubst ../core/%.cpp,build/core/%.o,$(CORE_SRC))

all: nxplex-cli json_test avtest apptest

$(MBED)/include/mbedtls/ssl.h:
	../tools/fetch-mbedtls.sh

$(LIBS): | $(MBED)/include/mbedtls/ssl.h
	$(MAKE) -C $(MBED) lib CFLAGS=-O2

build/core/%.o: ../core/%.cpp $(wildcard ../core/*.hpp) ../common/nxplex_ipc.h ../core/cacerts.inc | $(LIBS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

nxplex-cli: cli.cpp $(CORE_OBJ) $(LIBS)
	$(CXX) $(CXXFLAGS) $(INC) cli.cpp $(CORE_OBJ) $(LIBS) -lpthread -o $@

avtest: avtest.cpp sim.hpp $(CORE_OBJ) $(LIBS)
	$(CXX) $(CXXFLAGS) $(INC) avtest.cpp $(CORE_OBJ) $(LIBS) -lpthread -o $@

apptest: apptest.cpp sim.hpp $(CORE_OBJ) $(LIBS)
	$(CXX) $(CXXFLAGS) $(INC) apptest.cpp $(CORE_OBJ) $(LIBS) -lpthread -o $@

json_test: tests/json_test.cpp build/core/json.o
	$(CXX) $(CXXFLAGS) $(INC) $^ -o $@

test: json_test
	./json_test

clean:
	rm -rf build nxplex-cli json_test avtest apptest

.PHONY: all test clean
