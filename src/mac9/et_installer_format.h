// The payload of the one-file installer: scripts/installer_pack.py writes it after the PEF in the
// application's data fork, and et_installer.c unpacks it. No Toolbox here, so that
// tests/package/test_installer_format.c can run this parser under the sanitizers.
//
// All numbers are big-endian.
//   header:  'ETIN', version, entry count, then ET_ENTRY_BYTES per entry
//   entry:   Pascal name[32], parent, type, creator, Finder flags, data length, resource length,
//            data CRC, resource CRC
//   forks:   each file's data then resource fork, in entry order, straight after the header
//   trailer: 'ETIT', header offset, header length, CRC of the header
//
// A parent is the index of an earlier folder, or -1 for the top of the install. A folder has type
// 'fold' and no forks. CRCs are CRC-32 as in zlib.
#ifndef ET_INSTALLER_FORMAT_H
#define ET_INSTALLER_FORMAT_H

#define ET_ENTRY_BYTES 64
#define ET_HEADER_BYTES 12
#define ET_TRAILER_BYTES 16
#define ET_MAX_ENTRIES 16
#define ET_FORMAT_VERSION 1
#define ET_FOLDER_TYPE 0x666f6c64u      // 'fold'

typedef struct {
	unsigned char name[32];     // Pascal string, never holding a colon
	long parent;
	unsigned long type, creator, flags;
	unsigned long dataLength, rsrcLength, dataCrc, rsrcCrc;
} etEntry_t;

typedef struct {
	unsigned long headerOffset, headerLength, headerCrc;
} etTrailer_t;

static unsigned long ET_Be32( const unsigned char *p ) {
	return ( (unsigned long)p[0] << 24 ) | ( (unsigned long)p[1] << 16 ) | ( (unsigned long)p[2] << 8 ) | p[3];
}

static unsigned long ET_Crc( unsigned long crc, const unsigned char *p, unsigned long length ) {
	static unsigned long table[256];
	unsigned long i, k, c;

	if ( !table[1] ) {
		for ( i = 0; i < 256; i++ ) {
			c = i;
			for ( k = 0; k < 8; k++ ) {
				c = ( c & 1 ) ? 0xedb88320UL ^ ( c >> 1 ) : c >> 1;
			}
			table[i] = c;
		}
	}
	crc = ~crc & 0xffffffffUL;
	while ( length-- ) {
		crc = table[( crc ^ *p++ ) & 255] ^ ( crc >> 8 );
	}
	return ~crc & 0xffffffffUL;
}

// 0 when the trailer can describe a header inside a file of fileLength bytes
static int ET_ParseTrailer( const unsigned char *p, unsigned long fileLength, etTrailer_t *t ) {
	if ( fileLength < ET_TRAILER_BYTES || ET_Be32( p ) != 0x45544954u ) {
		return -1;
	}
	t->headerOffset = ET_Be32( p + 4 );
	t->headerLength = ET_Be32( p + 8 );
	t->headerCrc = ET_Be32( p + 12 );
	if ( t->headerLength < ET_HEADER_BYTES
		 || t->headerLength > ET_HEADER_BYTES + ET_MAX_ENTRIES * ET_ENTRY_BYTES
		 || t->headerOffset > fileLength - ET_TRAILER_BYTES
		 || t->headerLength > fileLength - ET_TRAILER_BYTES - t->headerOffset ) {
		return -1;
	}
	return 0;
}

// Returns the entry count, or -1 when the header is not whole and consistent. The forks must end
// exactly at the trailer, so that a truncated transfer is refused before anything is written.
static int ET_ParseHeader( const unsigned char *p, const etTrailer_t *t, unsigned long fileLength, etEntry_t *entries ) {
	unsigned long count, i, room;
	long long total = 0;
	const unsigned char *e;

	if ( ET_Crc( 0, p, t->headerLength ) != t->headerCrc || ET_Be32( p ) != 0x4554494eu
		 || ET_Be32( p + 4 ) != ET_FORMAT_VERSION ) {
		return -1;
	}
	count = ET_Be32( p + 8 );
	if ( count == 0 || count > ET_MAX_ENTRIES || t->headerLength != ET_HEADER_BYTES + count * ET_ENTRY_BYTES ) {
		return -1;
	}
	for ( i = 0; i < count; i++ ) {
		etEntry_t *n = &entries[i];
		unsigned long c;

		e = p + ET_HEADER_BYTES + i * ET_ENTRY_BYTES;
		for ( c = 0; c < 32; c++ ) {
			n->name[c] = e[c];
		}
		if ( n->name[0] == 0 || n->name[0] > 31 ) {
			return -1;
		}
		for ( c = 1; c <= n->name[0]; c++ ) {
			if ( n->name[c] == ':' ) {
				return -1;
			}
		}
		n->parent = (long)(int)ET_Be32( e + 32 );
		n->type = ET_Be32( e + 36 );
		n->creator = ET_Be32( e + 40 );
		n->flags = ET_Be32( e + 44 );
		n->dataLength = ET_Be32( e + 48 );
		n->rsrcLength = ET_Be32( e + 52 );
		n->dataCrc = ET_Be32( e + 56 );
		n->rsrcCrc = ET_Be32( e + 60 );
		if ( n->parent < -1 || n->parent >= (long)i
			 || ( n->parent >= 0 && entries[n->parent].type != ET_FOLDER_TYPE ) ) {
			return -1;
		}
		if ( n->type == ET_FOLDER_TYPE && ( n->dataLength || n->rsrcLength ) ) {
			return -1;
		}
		total += (long long)n->dataLength + n->rsrcLength;
	}
	room = fileLength - ET_TRAILER_BYTES - t->headerOffset - t->headerLength;
	return total == (long long)room ? (int)count : -1;
}

#endif
