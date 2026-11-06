#!/usr/bin/env python3
import sys
import struct
import os

def create_appledouble(resource_fork_path, output_path):
    # AppleDouble Version 2 Header
    # Magic: 0x00051607
    # Version: 0x00020000
    # Filler (16 bytes)
    # NumEntries: 2 (Finder Info, Resource Fork)
    
    # We will just write the Resource Fork entry for simplicity, 
    # but valid AppleDouble usually requires FinderInfo?
    # let's try just Resource Fork first, or empty FinderInfo.
    
    # Entry Structure: ID (4), Offset (4), Limit/Length (4)
    # ID 1 = Data Fork (Empty in AppleDouble usually, used in AppleSingle)
    # ID 2 = Resource Fork
    # ID 9 = Finder Info (Standard 32 bytes)
    
    try:
        with open(resource_fork_path, 'rb') as f:
            rsrc_data = f.read()
    except FileNotFoundError:
        print(f"Error: Resource file {resource_fork_path} not found.")
        sys.exit(1)
        
    rsrc_len = len(rsrc_data)
    
    header = bytearray(26)
    struct.pack_into('>I', header, 0, 0x00051607) # Magic
    struct.pack_into('>I', header, 4, 0x00020000) # Version
    # Filler 16 bytes zero
    struct.pack_into('>H', header, 24, 2) # NumEntries (FinderInfo + Rsrc)
    
    # Calculate Offsets
    # Header: 26 (header) + 12*2 (entries) = 50 bytes?
    # AppleDouble usually aligns instructions?
    # Let's verify spec. header is 26 bytes.
    # Entries follow immediately. 12 bytes each.
    # header_len = 26 + 2*12 = 50.
    
    finder_info_offset = 50
    finder_info_len = 32
    
    rsrc_offset = finder_info_offset + finder_info_len
    
    entries = bytearray()
    
    # Entry 1: Finder Info (ID 9)
    entries += struct.pack('>I', 9)
    entries += struct.pack('>I', finder_info_offset)
    entries += struct.pack('>I', finder_info_len)
    
    # Entry 2: Resource Fork (ID 2)
    entries += struct.pack('>I', 2)
    entries += struct.pack('>I', rsrc_offset)
    entries += struct.pack('>I', rsrc_len)
    
    # Data Construction
    data = bytearray()
    data += header
    data += entries
    
    # Write Finder Info (32 bytes zeros is fine, or set Type/Creator?)
    # Type: APPL (0x4150504C), Creator: IDQ3 (0x49445133) are in FinderInfo!
    # FinderInfo format:
    # 0x00: Type (4)
    # 0x04: Creator (4)
    # 0x08: Flags (2)
    # ...
    
    finder_info = bytearray(32)
    # APPL
    struct.pack_into('>I', finder_info, 0, 0x4150504C)
    # IDQ3 (MacQuake3 Original)
    struct.pack_into('>I', finder_info, 4, 0x49445133)
    # Flags: kHasBundle (0x2000)
    # This is critical for Finder to read BNDL and SIZE resources!
    struct.pack_into('>H', finder_info, 8, 0x2000)
    
    data += finder_info
    data += rsrc_data
    
    with open(output_path, 'wb') as f:
        f.write(data)
        
    print(f"Created AppleDouble file {output_path} (Type: APPL, Creator: IDQ3 )")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: create_appledouble.py <rsrc_path> <output_path>")
        sys.exit(1)
        
    create_appledouble(sys.argv[1], sys.argv[2])
