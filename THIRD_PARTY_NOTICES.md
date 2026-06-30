# Third-Party Notices

BlazeRules depends on third-party open-source libraries. This file is a
best-effort summary for source and binary distributions. The exact dependency
set depends on build flags and platform packaging.

Package managers, wheel repair tools, operating system packages, and vcpkg may
include more detailed notices. When redistributing BlazeRules, preserve the
license notices for the third-party libraries included in your distribution.

## Core Dependencies

| Component | Typical license | Purpose |
| --- | --- | --- |
| Apache Arrow / Parquet | Apache-2.0 | Arrow arrays, RecordBatch interop, Parquet/backtest IO. |
| Abseil | Apache-2.0 | Hash maps, hash sets, utility containers. |
| oneTBB | Apache-2.0 | Parallel execution primitives. |
| simdjson | Apache-2.0 | High-speed JSON/NDJSON parsing. |
| yaml-cpp | MIT | YAML rule/config parsing. |
| RE2 | BSD-3-Clause | Safe regular expression engine. |
| cpp-httplib | MIT | Local agent and dashboard HTTP server. |
| pybind11 | BSD-3-Clause | Python extension bindings. |

## Optional Dependencies

| Component | Typical license | Purpose |
| --- | --- | --- |
| ONNX Runtime | MIT | `model_score` ONNX inference. |
| librdkafka | BSD-2-Clause | Kafka consumer/producer support. |
| Protocol Buffers | BSD-3-Clause | Protobuf descriptor decoder. |
| Apache Avro C++ | Apache-2.0 | Avro decoder. |
| FlatBuffers | Apache-2.0 | vcpkg port support where used. |
| zlib | Zlib | Compression support pulled by optional IO dependencies. |
| fmt | MIT | Formatting support pulled by optional Avro builds. |

## Notes

- BlazeRules' own source is licensed under Apache-2.0; see `LICENSE`.
- Third-party components keep their own licenses.
- This notice is not legal advice and may not include every transitive
  dependency in every build profile.
