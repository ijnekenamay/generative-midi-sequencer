#include "storage_manager.hpp"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include <string.h>
#include <cstdio>

StorageManager::StorageManager() {
    // Constructor
}

uint32_t StorageManager::calculate_checksum(const TrackParams* params) {
    uint32_t checksum = 0;
    const uint8_t* byte_ptr = reinterpret_cast<const uint8_t*>(params);
    size_t size = sizeof(TrackParams) * 4;
    
    for (size_t i = 0; i < size; ++i) {
        checksum += byte_ptr[i];
    }
    
    return checksum;
}

bool StorageManager::load(TrackParams* out_params) {
    // Point directly to XIP flash mapped memory
    // XIP_BASE is defined in Pico SDK as 0x10000000
    const SaveData* flash_data = reinterpret_cast<const SaveData*>(XIP_BASE + FLASH_TARGET_OFFSET);
    
    // 1. Verify Magic Signature
    if (flash_data->magic != MAGIC_SIGNATURE) {
        printf("[Storage] No valid save data found in Flash (Magic mismatch).\n");
        return false;
    }
    
    // 2. Validate Checksum
    uint32_t computed = calculate_checksum(flash_data->params);
    if (computed != flash_data->checksum) {
        printf("[Storage] Save data corrupted (Checksum mismatch: computed %lu, expected %lu).\n", 
               computed, flash_data->checksum);
        return false;
    }
    
    // 3. Copy validated parameters to shared memory
    // Cast volatile pointers to non-volatile for standard memcpy
    memcpy(const_cast<TrackParams*>(out_params), flash_data->params, sizeof(TrackParams) * 4);
    
    printf("[Storage] Successfully loaded 4 track settings from Flash!\n");
    return true;
}

bool StorageManager::save(const TrackParams* in_params) {
    printf("[Storage] Saving settings to Flash...\n");
    
    // 1. Prepare SaveData structure
    SaveData data;
    data.magic = MAGIC_SIGNATURE;
    
    // Safely copy volatile parameters to our local struct
    memcpy(data.params, const_cast<TrackParams*>(in_params), sizeof(TrackParams) * 4);
    data.checksum = calculate_checksum(data.params);
    
    // 2. Disable interrupts during flash write
    // Erasing or writing to flash pauses the XIP cache. If Core 0 or Core 1 
    // attempts to execute code from flash during this window, the Pico will crash.
    // Disabling interrupts protects Core 0.
    uint32_t saved_interrupts = save_and_disable_interrupts();
    
    // Erase the target 4KB sector (must be sector aligned, FLASH_SECTOR_SIZE = 4096)
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    
    // Write data (must be page aligned, sizeof(SaveData) is padded to a multiple of FLASH_PAGE_SIZE = 256)
    // We write a full page of 256 bytes representing our data
    uint8_t write_buffer[FLASH_PAGE_SIZE];
    memset(write_buffer, 0, FLASH_PAGE_SIZE);
    memcpy(write_buffer, &data, sizeof(SaveData));
    
    flash_range_program(FLASH_TARGET_OFFSET, write_buffer, FLASH_PAGE_SIZE);
    
    // 3. Restore interrupts
    restore_interrupts(saved_interrupts);
    
    printf("[Storage] Settings saved successfully!\n");
    return true;
}
