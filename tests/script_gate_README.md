# Scripting validation

`script_tests.cpp` uses only the frozen contract/public APIs and real managed
fixtures from `managed/Mirabilis.TestScripts`. The test fixture assembly must be
beside ScriptCore and its generated runtimeconfig/deps files; CMake defines
`MIRABILIS_TEST_MANAGED_DIR` for this deployment. SampleScripts must also be
present for the authored demo check. These are headless tests, with no Vulkan.

`managed/Mirabilis.ScriptTests` is an isolated .NET console check. Its independent
ABI mirrors verify public layouts, malformed native-table rejection, UTF-8 buffer
ownership, safe API failures, collectible user/default API contexts, and actual
managed handles through successful construction, callback faults and cleanup.

`managed/Mirabilis.BadBridge` intentionally produces a different
`Mirabilis.ScriptCore.dll`. Keep it in a separate output directory. The isolated
`script_abi_probe` executable loads this fake bridge with argument `version`,
`size`, or `null`, then requires native initialization to disable scripting and
preserve saved Script data. Never deploy this DLL beside the engine.

After a native build, run CTest for Debug and Release. For the serial GPU gate:

```powershell
python tests/script_gate.py --repo C:/dev/mirabilis-work/scripting --output C:/dev/mirabilis-work/script-gate-fresh
```

Use a fresh output directory. This copies every authored scene into scratch,
launches a bounded 280-frame Debug run with validation enabled, and checks actual
counts/epochs/Force values through save/reopen, floor contact, reset, leaving Play
and sandbox replacement. The existing ECS runner rejects fatal/Vulkan/VUID lines.
The script gate preserves raw output, flags, engine hash and before/after hashes
for all authored scenes and the user's scene selection marker. It never saves an
authored scene. `test_script_gate.py` checks parser rejection of missing or invalid
runtime evidence. All GPU runs must be sequential; timing measurements additionally
require the established external Computer Use focus workflow.
