#!/usr/bin/env python3
import sys
import os
import struct
import binascii

MAC_EPOCH_OFFSET = 2082844800


def calc_crc(data):
    # CCITT CRC-16 (XMODEM)
    crc = 0
    for byte in data:
        crc = crc ^ (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc = crc << 1
            crc &= 0xFFFF
    return crc

def encode_macbinary(input_path, output_path, file_type, file_creator, rsrc_path=None):
    filename = os.path.basename(input_path)
    try:
        filename_bytes = filename.encode('mac_roman')
    except UnicodeEncodeError as exc:
        raise ValueError(f"filename is not representable in MacRoman: {filename!r}") from exc
    filename_bytes = filename_bytes[:63]
    
    data_len = os.path.getsize(input_path)
    rsrc_len = 0
    if rsrc_path and os.path.exists(rsrc_path):
        rsrc_len = os.path.getsize(rsrc_path)
    if data_len > 0xFFFFFFFF or rsrc_len > 0xFFFFFFFF:
        raise ValueError("MacBinary II fork lengths must fit in 32 bits")
    
    # SOURCE_DATE_EPOCH makes release metadata reproducible. Otherwise preserve
    # the input image's modification time for both MacBinary date fields.
    unix_timestamp = int(os.environ.get(
        "SOURCE_DATE_EPOCH", os.path.getmtime(input_path)))
    mac_timestamp = unix_timestamp + MAC_EPOCH_OFFSET
    if not 0 <= mac_timestamp <= 0xFFFFFFFF:
        raise ValueError("timestamp is outside the MacBinary II date range")
    
    header = bytearray(128)
    
    # 0: Old Version (0)
    header[0] = 0
    # 1: Filename Length
    header[1] = len(filename_bytes)
    # 2-64: Filename
    header[2:2+len(filename_bytes)] = filename_bytes
    
    # 65-68: Type
    header[65:69] = file_type.encode('mac_roman').ljust(4)[:4]
    
    # 69-72: Creator
    header[69:73] = file_creator.encode('mac_roman').ljust(4)[:4]
    
    # 83-86: Data Fork Length (Big Endian)
    struct.pack_into('>I', header, 83, data_len)
    
    # 87-90: Resource Fork Length
    struct.pack_into('>I', header, 87, rsrc_len)

    # 91-94: Creation date; 95-98: Modification date (Mac epoch).
    struct.pack_into('>I', header, 91, mac_timestamp)
    struct.pack_into('>I', header, 95, mac_timestamp)
    
    # 122: Version
    header[122] = 129
    # 123: Min Version
    header[123] = 129
    
    # 124-125: CRC of first 124 bytes
    crc = calc_crc(header[:124])
    struct.pack_into('>H', header, 124, crc)
    
    with open(output_path, 'wb') as outfile:
        outfile.write(header)
        
        # Write Data Fork
        with open(input_path, 'rb') as infile:
            while True:
                chunk = infile.read(65536)
                if not chunk: break
                outfile.write(chunk)
        
        # Pad Data Fork to 128 bytes
        pad_len = (128 - (data_len % 128)) % 128
        outfile.write(b'\x00' * pad_len)
        
        # Write Resource Fork
        if rsrc_len > 0:
            with open(rsrc_path, 'rb') as infile:
                while True:
                    chunk = infile.read(65536)
                    if not chunk: break
                    outfile.write(chunk)
            
            # Pad Resource Fork to 128 bytes
            pad_len = (128 - (rsrc_len % 128)) % 128
            outfile.write(b'\x00' * pad_len)

    print(f"Encoded {input_path} to {output_path} ({file_type}/{file_creator})")

if __name__ == "__main__":
    if len(sys.argv) < 5:
        print("Usage: macbinary_encode.py <input> <output> <type> <creator> [rsrc_input]")
        sys.exit(1)
        
    rsrc = None
    if len(sys.argv) > 5:
        rsrc = sys.argv[5]
        
    encode_macbinary(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], rsrc)
