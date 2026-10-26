#pragma once

namespace skeetsdk::config_files
{
    // Configs as files: Counter-Strike Global Offensive\skeet\configs\<name>.cfg, one file per config holding that
    // config's data as the cheat saves it (it starts with the config signature 0xDEC00D60), so a config can be
    // copied, shared or deleted like any file.
    //
    // The cheat keeps every config in one store file (its path is the UNICODE_STRING at 0x434798CC, for example
    // csgo\cfg\6626462449.cfg): 0xAF, then for each config a string field (0x01, u32 length, bytes) with the name
    // (UTF-8), one with the data and a byte field (0x03, byte) with its key slot (0x1B = none). It reads the store
    // again for every load, and rewrites it whole on every save or delete. Nothing in the cheat is patched; the
    // store and the folder are kept the same:
    //   - the cheat saved or deleted a config (the store's size or write time changed): the files follow, and a
    //     file is only deleted when it was there for a config the store had;
    //   - a file in the folder was added, changed, renamed or deleted: the store is rebuilt from the folder and the
    //     Config tab's list is reloaded (0x4335C747, what the Save button runs after saving);
    //   - first run: an empty folder gets the store's configs; otherwise the folder wins.
    // Characters a file name cannot have are written as %XX. Key slots other than "none" are kept in keys.txt.
    // Files that do not start with the signature are left out of the store.
    //
    // Call it every frame on the game's thread (renderer_blur does, right before paint_ui).
    void Tick() noexcept;
}
