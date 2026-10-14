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
| `rigidBody` | `{motion: Static/Dynamic, mass: float, friction: float, restitution: float, gravityFactor: float}` (optional) |
| `timeTrialRole` | `{role: none/spawn/start/finish}` |
| `portalPlaceable` | `{}` |
| `transformDrivenExternally` | `{}` |

Vectors are JSON arrays. Transform rotation uses Euler radians in Y * X * Z
order. Collider shapes are Box, GroundPlane and SurfRamp respectively. Basic
value components include disabled/default values. Missing component blocks use
defaults. Unknown component blocks retain their JSON values and are written in
name order; this preserves future component data, including scripts, without executing it.
GPU resources are reconstructed separately; unavailable assets retain their
authored paths.

Rigid-body defaults are Static motion, mass 1, friction 0.5, restitution 0 and
gravity factor 1. Every numeric value must be finite. Friction must be
nonnegative and restitution must be in [0, 1]; gravity factor may be negative.
Dynamic mass and collider half extents must be positive, and Dynamic bodies
require an enabled Box collider and nonzero scale. Static settings persist when
collision is disabled. Enabled colliders without a rigid-body block use default
Static settings. Transient sleeping state and native body IDs are never saved.

Dynamic entities are roots. When a file contains a parented Dynamic entity,
staging captures its original world matrix before any detachment, rejects a
matrix that cannot be represented exactly as TRS, then detaches the entity and
appends it to the ordered roots with a warning. World position, rotation and
scale are preserved; descendants retain their local transforms. The detached
root order follows the original hierarchy traversal. Saving uses the same
physics validation as the loader and rejects invalid Dynamic parenting.

Collider and Static rigid-body settings on descendants of a Dynamic entity are
retained and saved but dormant: descendants are visual-only and create no
independent body or player collider. Static sheared colliders outside Dynamic
subtrees use the best-fit oriented shape, with inspector and load warnings.
Authored portal hosts must be enabled, placeable Box colliders outside every
Dynamic subtree; invalid references fail transactionally. Editor parenting and
body-motion changes enforce this restriction before changing authored state.

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

Engine harness saves must set `MIRABILIS_TEST_SCENE_DIRECTORY` to an absolute
scratch directory containing scene copies. Scene load/save paths use that
directory; model and texture paths keep their existing interpretation. Bounded
`MIRABILIS_TEST_FRAMES` runs never write `assets/scenes/.last_scene`.
