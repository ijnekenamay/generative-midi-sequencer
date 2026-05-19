#pragma once
#include <stdint.h>
#include "track.hpp" // To get access to TrackParams structure

struct SaveData {
    uint32_t magic;          // 'SEQM' magic signature (0x5345514D)
    TrackParams params[4];   // Parameters for all 4 tracks
    uint32_t checksum;       // Simple additive checksum for validation
};

static_assert(sizeof(SaveData) <= 256, "SaveData exceeds one flash page size of 256 bytes");

class StorageManager {
private:
    // We target the very last 4KB sector of the 16MB flash to avoid conflict with the code binary
    static const uint32_t FLASH_TARGET_OFFSET = (16 * 1024 * 1024) - 4096; // 0xFFF000
    static const uint32_t MAGIC_SIGNATURE = 0x5345514D;

    uint32_t calculate_checksum(const TrackParams* params);

public:
    StorageManager();

    /**
     * Loads the saved parameters from flash.
     * Verifies the magic signature and checksum before applying.
     * 
     * @param out_params Pointer to the 4-track params array to fill.
     * @return true if loading was successful and valid, false otherwise (defaults will be kept).
     */
    bool load(TrackParams* out_params);

    /**
     * Saves the current parameters to flash.
     * Erases the sector and writes the parameterized struct.
     * Note: Core 1 interrupts must be safely suspended during writes to prevent execution crashes!
     * 
     * @param in_params Pointer to the 4-track params array to write.
     * @return true if successful, false otherwise.
     */
    bool save(const TrackParams* in_params);
};
