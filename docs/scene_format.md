# Scene format

The engine accepts versions 1 and 2 and saves version 2. Reading and validation
run in GPU-free staging before the engine replaces the live scene. Saving uses
the same validation, writes and closes a sibling temporary file, then atomically
replaces the destination. Failures preserve the current scene or previous file.

Version 2 has `version: 2`, `nextActor`, `nextPersistentId`, the existing
`lighting`, `referenceLighting`, `colorGrading`, optional `ssao` and
`authoredPortals` blocks, and `objects`. Persistent IDs are unsigned 64-bit JSON
integers; zero is valid. `UINT64_MAX` is reserved for counter exhaustion. A
missing parent or portal host is `null`.

Each object has `id`, `parent`, `siblingIndex`, and `components`. Objects are
written in hierarchy pre-order. Roots have their own ordered sibling list.
Loading sorts siblings by nonnegative `siblingIndex`; ties keep file order.
Invalid parents and cycles are rejected.

| Component key | Payload |
| --- | --- |
| `name` | `{value: string}` |
| `transform` | `{position: vec3, rotation: vec3, scale: vec3}` |
| `visibility` | `{visible: bool, layer: 0 or 1}` |
| `mesh` | `{asset: empty/floor/cube/ramp/gltf, modelPath: string}` |
| `material` | `{enabled, baseColorTexture, normalTexture, metallicRoughnessTexture, tint: vec4, uvScale: vec2, metallic, roughness, debugChecker, emissionColor: vec3, emissionStrength, transmission, ior}` |
| `collider` | `{enabled: bool, shape: 0/1/2, center: vec3, halfExtents: vec3}` |
| `timeTrialRole` | `{role: none/spawn/start/finish}` |
| `portalPlaceable` | `{}` |
| `transformDrivenExternally` | `{}` |

Vectors are JSON arrays. Transform rotation uses Euler radians in Y * X * Z
order. Collider shapes are Box, GroundPlane and SurfRamp respectively. Basic
value components include disabled/default values. Missing component blocks use
defaults. Unknown component blocks retain their JSON values and are written in
name order; this preserves future rigid-body or script data without executing it.
GPU resources are reconstructed separately; unavailable assets retain their
authored paths.

Version 1 uses the existing flat object fields. Its saved IDs become persistent
IDs, file order determines sibling order, negative parents mean no parent, and
portal host `UINT32_MAX` means no host. Stale PortalViewOnly entries are skipped.
The existing `gi_` filename collision fallback is applied during version 1
import. Every valid file ID is reserved before repairing duplicates: the first
occurrence keeps its ID, later occurrences receive IDs above all file IDs and
the saved counter, with a warning. References bind to the first occurrence.

RuntimeOnly entities and their descendants are excluded from saved objects.
TransformDrivenExternally only controls transform editing and remains saved.
Play saves use authored snapshot entities, settings and portal pairs, with the
current persistent-ID allocator high-water mark. Round trips compare authored
components, references, settings and hierarchy order; runtime IDs and allocator
metadata may advance when runtime entities are recreated.
