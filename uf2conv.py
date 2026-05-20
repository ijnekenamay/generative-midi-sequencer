#!/usr/bin/env python3
"""
Minimal UF2 converter: converts a flat binary to a UF2 file for RP2040.
Usage: python uf2conv.py <input.bin> <output.uf2> [base_address]
Default base_address = 0x10000000 (RP2040 flash start)
"""
import sys
import struct
import os

UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END    = 0x0AB16F30
UF2_FLAG_FAMILYID_PRESENT = 0x00002000
RP2040_FAMILY_ID = 0xe48bff56

BLOCK_SIZE = 256  # data bytes per block

def convert(bin_path, uf2_path, base_addr):
    with open(bin_path, 'rb') as f:
        data = f.read()

    # Pad to multiple of BLOCK_SIZE
    pad = (BLOCK_SIZE - len(data) % BLOCK_SIZE) % BLOCK_SIZE
    data += b'\x00' * pad

    num_blocks = len(data) // BLOCK_SIZE
    with open(uf2_path, 'wb') as out:
        for i in range(num_blocks):
            block_data = data[i*BLOCK_SIZE:(i+1)*BLOCK_SIZE]
            target_addr = base_addr + i * BLOCK_SIZE
            flags = UF2_FLAG_FAMILYID_PRESENT
            # UF2 block: 32 bytes header + 256 bytes data + 4 bytes magic end = 476 bytes padding + 4 = 512 total
            header = struct.pack('<IIIIIIII',
                UF2_MAGIC_START0,
                UF2_MAGIC_START1,
                flags,
                target_addr,
                BLOCK_SIZE,
                i,
                num_blocks,
                RP2040_FAMILY_ID
            )
            payload = block_data + b'\x00' * (476 - BLOCK_SIZE)
            footer = struct.pack('<I', UF2_MAGIC_END)
            out.write(header + payload + footer)

    print(f"Wrote {num_blocks} blocks to {uf2_path}")
    print(f"Base address: 0x{base_addr:08X}")
    print(f"File size: {os.path.getsize(uf2_path)} bytes")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python uf2conv.py <input.bin> <output.uf2> [base_hex]")
        sys.exit(1)
    base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x10000000
    convert(sys.argv[1], sys.argv[2], base)
