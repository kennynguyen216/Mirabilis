# Vendored core dependencies

These sources are committed; configuration performs no network downloads.

| Dependency | Exact release | License | Runtime files |
| --- | --- | --- | --- |
| EnTT | v3.15.0 | MIT | None (header only) |
| doctest | v2.4.12 | MIT | None (test header only) |
| simdjson | v3.3.0 | Apache-2.0 or MIT | None (statically linked) |

EnTT and doctest use the upstream amalgamated headers and licenses with small
local CMake interface targets. simdjson reuses the exact release already required
by fastgltf 0.6.1; its sources now live here instead of being downloaded at
configuration time. Existing renderer dependencies retain their existing setup.

Sources: https://github.com/skypjack/entt/releases/tag/v3.15.0,
https://github.com/doctest/doctest/releases/tag/v2.4.12,
https://github.com/simdjson/simdjson/tree/v3.3.0.

Jolt Physics **v5.6.0** (MIT), tag commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`, is vendored from
https://github.com/jrouwe/JoltPhysics/releases/tag/v5.6.0 (Jolt/, Build/, LICENSE).
It is statically linked with CPU rigid-body simulation, shared MSVC runtime,
no renderer/profiler/compute backends and no additional runtime files.
The engine uses its existing SDL/runtime DLL deployment. .NET remains outside Milestone 2.

