# Frozen Milestone 1 contracts

The canonical typed API is `mirabilis::Scene` in `scene_ecs.h`; the caller migration
replaces the legacy global `Scene` with an alias to it. The legacy vector exists
only through the building extraction/contract checkpoints, and is removed in the
single validated registry/caller integration group. No worker adds a SceneObject
adapter or a second authoritative entity aggregate.

## Ownership and integration

Lead owns all CMake files, scene types/components/wrapper, snapshots, queues,
`vk_engine.h`, `vk_engine.cpp`, debug views, scene material runtime code and
unassigned files. Editor specialist owns editor, scene IO, `scene_serialization.cpp`
and `scene_format.md`. Gameplay specialist owns gameplay, scene emission, portals,
`scene_queries.cpp` and listed renderer eligibility callers. Validation owns tests
and fixtures after the contract checkpoint (including the existing core smoke test).
Worker branches start at the frozen commit and integrate one at a time on
`integration/ecs`; only the building, tested combined group merges to `feature/ecs`.

## Identity and access

`PersistentID` is uint64_t. EntityRef is `{epoch, full entt::entity, persistentId}`;
its truth conversion means a reference is present, not that it is still valid.
Every operation checks Scene::valid. Persistent IDs start at zero; zero is valid.
Final engine SceneObjectID is a PersistentID alias; UINT64_MAX is only the
engine's missing-ID marker and cannot be allocated (exhaustion is an error).
Hierarchy parent and Portal::hostWallObject use optional<PersistentID>.
These last two legacy signature changes land in the integration group.

Creation has an owner=false argument after optional parent, allowing the native
runtime-object constructor to create the protected player's child with owner=true.

`get(id)` resolves a fresh reference; `read<T>(ref)` returns a const component
pointer, including while pending. `edit<T>` is for native/editor component fields;
it refuses protected runtime entities, pending entities and destruction callbacks.
Identity, order, pending, Transform and Hierarchy cannot be edited through it.
`set<T>` validates values and access; `remove<T>` has the same access validation.
Identity/order/pending are internal. Transform/hierarchy mutations use their
explicit setters. Setters during a destruction callback queue live targets for
the next apply_commands; setters on dying targets fail. Owners pass owner=true
only from native systems to manage runtime-only entities.

Scene creates default Name, Transform, Hierarchy, Visibility, SceneMeshAsset,
SceneMaterial, Collider, TimeTrial and UnknownComponents. PortalPlaceable,
TransformDrivenExternally and RuntimeOnly are tags. GPU primitive/model data
lives in VulkanEngine::_sceneRenderRuntimes keyed by PersistentID.

`roots` is ordered IDs; hierarchy_order is ordered pre-order references.
creation_order keeps baseline collision/spawn/nearest-portal ties independent of
registry storage order. `set_parent` appends to the new sibling/root list;
`set_sibling_index` reorders it. World-to-local keeps the extracted existing
best-fit Euler decomposition. All transform values must be finite.

## Lifecycle

destroy_object marks a validated subtree pending immediately; public requests
that contain protected runtime entities fail without partial changes. Repeated
requests against a valid pending reference return true without a new diagnostic.
drain_destruction calls the supplied callback children before parents, keeps
reads available, and then removes the entity. Work requested during the callback
joins this drain. At the cap, remaining pending work stays queued with an error.
Queued commands carry EntityRef, owning copies of arguments, and a native action;
pending/stale targets are discarded. apply_commands consumes one batch; commands
created while applying it wait for the next batch.

take_snapshot captures eligible saved components, persistent hierarchy links,
creation/root/sibling order, and the IDs of preexisting runtime-only entities.
SceneDocumentSnapshot adds settings and authored portal pairs. restore_snapshot
removes play-created entities, recreates deleted saved entities, restores surviving
saved entities in place, preserves preexisting runtime entities, clears old work,
increments the epoch, and never lowers the allocator high-water mark. Engine-held
IDs are rebound and previous transforms reset; unchanged GPU resources survive.
replace installs only validated staging and advances the old scene's epoch.

Engine APIs frozen in vk_engine.h: typed assign_scene_asset/resolve_scene_material,
capture_scene_settings/apply_scene_settings, install_staged_scene,
take_play_snapshot/restore_play_snapshot/reset_play, drain_scene_destruction and
cleanup_scene_entity. Cleanup invalidates player portals/authored pairs and ID-keyed
history before collision/render; glTF resources retire only after GPU completion.

## Core queries and IO

scene_queries.h freezes collision filling/return overloads, find_scene_spawn_point,
collect_time_trial_overlaps, scene_emission_order and contributes_to_static_caches.
Milestone 1 eligibility is true for valid live renderables; dynamic sleep inputs
are frozen at Milestone 2. Portal carving keeps blue/orange/authored endpoint order.

scene_serialization.h freezes SceneSettings, StagedScene, SceneDocumentSnapshot,
parse_scene_json, validate_authored_scene, serialize_scene_json and save_scene_atomic.
Parsing leaves the output unchanged on failure. Loader defaults preserve existing
scene behavior; the v1 gi_ collision fallback is applied before staging validation.
UnknownComponents retains ordered name/raw JSON blocks. Settings include optional
SSAO and the requested skybox identity even if its runtime asset is unavailable.
Seven authored pairs is the existing renderer limit (16 surfaces minus player pair).
Serialization/save validate before output, then write/close a sibling temporary file
and atomically replace the destination. Play saves serialize the document snapshot
and current allocator high-water mark. Tests always use scratch destinations.

The v2 component payloads are frozen as follows; tags are present as empty objects.

| Key | Payload |
| --- | --- |
| name | {value: string} |
| transform | {position: vec3, rotation: vec3 Euler radians, scale: vec3} |
| visibility | {visible: bool, layer: integer 0..1} |
| mesh | {asset: empty/floor/cube/ramp/gltf, modelPath: string} |
| material | {enabled, baseColorTexture, normalTexture, metallicRoughnessTexture, tint: vec4, uvScale: vec2, metallic, roughness, debugChecker, emissionColor: vec3, emissionStrength, transmission, ior} |
| collider | {enabled: bool, shape: integer 0..2, center: vec3, halfExtents: vec3} |
| portalPlaceable | {} |
| timeTrialRole | {role: none/spawn/start/finish} |
| transformDrivenExternally | {} |

Basic value components serialize their default disabled/empty values too. RuntimeOnly
is excluded with its descendants. Unknown component blocks remain raw JSON values.
Object id/parent/siblingIndex live outside components; null is no parent/portal host.
