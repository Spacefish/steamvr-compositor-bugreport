# Workaround: stop the vrcompositor `gfxhub` page fault

**This is a SteamVR-side hack, not a proper fix.** The real fix belongs to the
SteamVR compositor (or RADV/Valve), which is binding a descriptor set whose
`Set 0 / Binding 0` UBO is not valid for the draw — see the root-cause section
in the top-level `README.md`.

## What it does

`resources/shaders/vulkan/unlit_vs.spv` (entry `VS_MAIN`) does:

```spir-v
%107 = OpAccessChain %_ptr_Uniform_uint %49 %int_19   ; Set 0 / Binding 0 (UBO)
%108 = OpLoad %uint %107                              ; faulting dereference
       OpStore %gl_Layer %108
```

The bundled `unlit_vs.spv` replaces that load with a constant 0
(`%108 = OpCopyObject %uint %uint_0`). With it installed there is no `gfxhub`
page fault, no RADV hang dump and no watchdog abort. `gl_Layer` is forced to 0.

Evidence that the UBO read is the culprit (all run against SteamVR):

| Variant | UBO read kept | `gl_Layer` | Result |
|---|---|---|---|
| shipped | yes | from UBO | crash |
| bundled workaround | no | 0 | no crash |
| keep load, store `%108 >> 31` | yes | ~0 | crash |
| no load, constant 1 | no | 1 | no crash |

## Usage

```sh
./apply.sh    [SteamVR dir]   # default ~/.local/share/Steam/steamapps/common/SteamVR
./restore.sh  [SteamVR dir]
```

`apply.sh` backs the shipped shader up next to itself as `unlit_vs.spv.orig`
before overwriting it.

## Hashes

| File | md5 |
|---|---|
| shipped `unlit_vs.spv` | `cc85c34c275ffe34462753a36abb47a3` |
| bundled workaround `unlit_vs.spv` | `4feb612cac1a8b052ec6e0d44f47070e` |

## Caveats

- SteamVR updates / "verify files" will restore the shipped shader; re-apply.
- It disables whatever the layer value was used for (it is 0 now). On this
  setup rendering is unaffected because the framebuffer is effectively
  single-layer, but that may not hold for other configurations.
- It does not address the underlying question of *why* the app hands the GPU an
  invalid UBO descriptor address.
