this project was made with AI. its basic skeet crack from https://github.com/sdkmasteri/skeet/tree/master but with debug lib. only thing i didnt not add was debug.debug bc i think its too dangerous and useless ( i mean its dangerous bc it could help crack some luas but idc, if yall will want i can add it)

## Verifying a release

Release builds are reproducible: the same commit built with the same Visual Studio toolset gives a byte-identical `skeet.dll`, no matter which folder you build in. So you don't have to trust the DLL from the releases page, you can check it:

1. Check out the commit or tag the release was built from.
2. Build `Release | x86` with Visual Studio 2026 (toolset v145).
3. Compare hashes:

```
certutil -hashfile Release\skeet.dll SHA256
```

The hash should match the one published with the release. If your toolset version differs from the one used for the release, the hash can differ too.

What makes this work (see `Directory.Build.targets`): `/Brepro` replaces timestamps with content hashes, `/pathmap` and `/PDBALTPATH` keep your folder paths out of the binary, and `skCrypt` derives its keys from `__LINE__` instead of `__TIME__`.

## skeetsdk.build

Scripts can see which build they are running on:

| Field | Value |
|---|---|
| `skeetsdk.build.commit` | git commit the DLL was built from (`"unknown"` when built without git) |
| `skeetsdk.build.dirty` | `true` if tracked files had uncommitted changes |
| `skeetsdk.build.tag` | release tag on that commit, or `nil` |
| `skeetsdk.build.id` | the commit, plus `-dirty` for modified builds |

A script that only wants to run on official releases:

```lua
local official = {
    -- ["<commit of a release>"] = true,
}

local build = skeetsdk and skeetsdk.build
if not build or build.dirty or not official[build.commit] then
    error("this script needs an official skeet-debug release")
end
```

This identifies honest builds, it does not prove anything: a modified build can report the same values. It stops people from running your scripts on random forks by accident, not on purpose.
