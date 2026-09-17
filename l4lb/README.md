# L4 TCP Load Balancer

This directory contains a Linux, IPv4 and TCP user-space full proxy extracted from the historical tutorial snapshots under `code/dayNN`. It is not an LVS, NAT, DR or kernel-bypass implementation. The data plane forwards opaque TCP byte streams and does not parse HTTP.

## Build

Install a C++14 compiler, CMake, pthreads and the jsoncpp development package, then run:

```sh
cmake -S l4lb -B l4lb/build -DL4LB_REQUIRE_JSONCPP=ON
cmake --build l4lb/build
```

During the initial network-foundation stage, jsoncpp-dependent configuration sources are not built yet, so the dependency check can be left non-strict:

```sh
cmake -S l4lb -B l4lb/build
cmake --build l4lb/build
```

## Test

```sh
ctest --test-dir l4lb/build --output-on-failure
```

AddressSanitizer and UndefinedBehaviorSanitizer can be enabled together:

```sh
cmake -S l4lb -B l4lb/build-asan \
  -DL4LB_ENABLE_ASAN=ON -DL4LB_ENABLE_UBSAN=ON
cmake --build l4lb/build-asan
ctest --test-dir l4lb/build-asan --output-on-failure
```

ThreadSanitizer uses a separate build:

```sh
cmake -S l4lb -B l4lb/build-tsan -DL4LB_ENABLE_TSAN=ON
cmake --build l4lb/build-tsan
ctest --test-dir l4lb/build-tsan --output-on-failure
```

## Run

The executable is currently an implementation scaffold:

```sh
./l4lb/build/l4lb_server
```

The completed server will accept a JSON configuration path. See `config/example.json` for the evolving configuration shape.

