import struct
import sys

# UF2 Magic Numbers
UF2_MAGIC_START0 = 0x0A324655 # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END    = 0x0AB16F30

# RP2040 Family ID
RP2040_FAMILY_ID = 0xe48bff56

def bin_to_uf2(bin_path, uf2_path):
    with open(bin_path, 'rb') as f:
        data = f.read()
    
    # 256 bytes of payload per block
    block_size = 256
    num_blocks = (len(data) + block_size - 1) // block_size
    
    with open(uf2_path, 'wb') as out:
        for block_no in range(num_blocks):
            # Header fields
            flags = 0x00002000 # Family ID present
            target_addr = 0x10000000 + block_no * block_size
            payload_size = min(block_size, len(data) - block_no * block_size)
            
            # Payload data padded to 476 bytes with zeros
            chunk = data[block_no * block_size : block_no * block_size + payload_size]
            chunk_padded = chunk + b'\x00' * (476 - len(chunk))
            
            # Pack header
            # <IIIIIIII - 8 unsigned 32-bit ints (little endian)
            header = struct.pack(
                '<IIIIIIII',
                UF2_MAGIC_START0,
                UF2_MAGIC_START1,
                flags,
                target_addr,
                payload_size,
                block_no,
                num_blocks,
                RP2040_FAMILY_ID
            )
            
            # Pack footer
            footer = struct.pack('<I', UF2_MAGIC_END)
            
            # Write 512 bytes block
            out.write(header + chunk_padded + footer)

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python uf2conv.py input.bin output.uf2")
        sys.exit(1)
    bin_to_uf2(sys.argv[1], sys.argv[2])
    print(f"Successfully converted {sys.argv[1]} to {sys.argv[2]}")
