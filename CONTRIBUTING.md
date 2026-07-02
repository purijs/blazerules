# Contributing to BlazeRules

Thanks for your interest in improving BlazeRules. This guide covers how to build,
test, and propose changes.

## Reporting issues

- **Bugs / features:** open a [GitHub issue](https://github.com/purijs/blazerules/issues)
  with a minimal `rules.yaml`, a sample record, and the expected vs. actual result.
- **Security vulnerabilities:** do **not** open a public issue. Use GitHub private
  vulnerability reporting at
  [github.com/purijs/blazerules/security/advisories](https://github.com/purijs/blazerules/security/advisories).

## Prerequisites

- A C++20 compiler (Clang or GCC), CMake ≥ 3.24, and Ninja.
- [vcpkg](https://github.com/microsoft/vcpkg) for dependencies (Arrow, yaml-cpp, RE2,
  Abseil, GoogleTest, and the optional ONNX/Kafka/Avro/Protobuf features).

## Building

Use the CMake presets (see [installation docs](https://blazerules.readme.io/docs/installation)):

```bash
# macOS arm64
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release -j

# Linux x86_64 (runtime SIMD dispatch)
cmake --preset linux-x86_64-release-dispatch
cmake --build --preset linux-x86_64-release-dispatch -j
```

The Python package builds with `pip install .` (scikit-build-core drives CMake).

## Running the test suite

The C++ tests are gated behind `-DBLAZERULES_TESTS=ON` and use GoogleTest:

```bash
cmake -S . -B build-tests -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DBLAZERULES_TESTS=ON
cmake --build build-tests --target blazerules_tests -j
ctest --test-dir build-tests --output-on-failure
```

Please add tests for new behavior. Rule parsing/compilation is **fail-closed**: bad
input (unknown operator/action, malformed nested condition, over-deep nesting) must
produce a clear error, never a silent fallback — cover both the happy path and the
rejection path.

## Pull requests

1. Keep changes focused; match the surrounding code style.
2. Ensure `ctest` passes and the docs (under `documentation/`) stay accurate for any
   user-visible change.
3. Describe the motivation and, for behavior changes, the migration impact.

By contributing you agree that your contributions are licensed under the project's
[Apache License 2.0](LICENSE).
