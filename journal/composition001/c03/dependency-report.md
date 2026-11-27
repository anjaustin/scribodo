# Scribodo dependency report

## Git submodules

| Path | Pin | Upstream HEAD | Status |
|---|---|---|---|
| third_party/raylib | `6.2` / `02566453880e` | `7aeef5775435` | update available |
| third_party/clay | `v0.14` / `b25a31c1a152` | `e6cc36941ab2` | update available |
| third_party/stb | `d14d46f` / `d14d46f6b444` | `6b5799bae8a1` | update available |
| third_party/cJSON | `v1.7.19-6-g6d9f244` / `6d9f2443ab07` | `6d9f2443ab07` | current |
| third_party/llama.cpp | `b12111-5-g5851a8e50` / `5851a8e505f4` | `fa9f928f25a5` | update available |
| third_party/mariadb-connector-c | `v3.4.11` / `be67a4fc1e04` | `592b6a0c4863` | update available |

## Container images

| Image | Pinned digest | Status |
|---|---|---|
| `mariadb:11.4.13` | `sha256:5e73baf8179b…` | tag moved (`sha256:a8992b5e5404…`) |
| `qdrant/qdrant:v1.20.0` | `sha256:9bfc89f9914f…` | verified (`sha256:9bfc89f9914f…`) |

## OSV commit advisories

| Dependency | Advisory | Summary |
|---|---|---|
| third_party/llama.cpp | [CVE-2026-133710](https://osv.dev/vulnerability/CVE-2026-133710) | llama.cpp b11003 before b12556 Heap Out-of-Bounds Write via hrm_text Model Loader |
| third_party/llama.cpp | [CVE-2026-134540](https://osv.dev/vulnerability/CVE-2026-134540) | ggml-org llama.cpp llama-server sampling.cpp common_sampler_init input validation |
| third_party/llama.cpp | [CVE-2026-137057](https://osv.dev/vulnerability/CVE-2026-137057) | llama.cpp b7118 before b12604 Integer Underflow in Grammar Parser parse_sequence |
| third_party/llama.cpp | [CVE-2026-137567](https://osv.dev/vulnerability/CVE-2026-137567) | llama.cpp b11450 before b12658 Out-of-Bounds Write via RPC copy_tensor |

## Report health

- **Error:** mariadb:11.4.13 resolves to sha256:a8992b5e5404d11e03c54a31d0c1bdea3fd116d283aa6d1a627551f29045309b, expected sha256:5e73baf8179b29f115101bb102763a47a017361f3e9792daccba6646c3ab1532

> Update availability is informational. This report fails only for invalid, mismatched, modified or moved pins, lookup failures, or confirmed advisories.
