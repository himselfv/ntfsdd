#pragma once
#include "ntfs.h"
#include <stdexcept>

/*
ULONG BpbChecksum(void* buf)
{
	ULONG checksum = 0;
	for (size_t i = 0; i < BIOS_PARAMETER_BLOCK_SIZE_FOR_CHECKSUM; ++i) {
		//Rotate the current checksum right by 1 bit
		checksum = (checksum >> 1) | (checksum << 31);
		// Skip the checksum field itself (bytes 80, 81, 82, 83)
		if (i >= 0x50 && i <= 0x53)
			continue;
		checksum += ((UCHAR*)buf)[i];
	}
	return checksum;
}


inline void __byteswap_ushort(USHORT& buf) {
	buf = _byteswap_ushort(buf);
}

inline void __byteswap_ulong(ULONG& buf) {
	buf = _byteswap_ulong(buf);
}

inline void __byteswap_uint64(UINT64& buf) {
	buf = _byteswap_uint64(buf);
}

bool BIOS_PARAMETER_BLOCK::decodeAndTestChecksum()
{
	//Verify CRC
	auto checksum = BpbChecksum(this);
	this->decodeByteOrder();
	return (this->Checksum != checksum);
}

void BIOS_PARAMETER_BLOCK::decodeByteOrder()
{
	__byteswap_ushort(this->BytesPerSector);
	__byteswap_ushort(this->ReservedSectors);
	__byteswap_ushort(this->RootEntries);
	__byteswap_ushort(this->Sectors);
	__byteswap_ushort(this->SectorsPerFat);
	__byteswap_ushort(this->SectorsPerTrack);
	__byteswap_ushort(this->Heads);
	__byteswap_ulong(this->HiddenSectors);
	__byteswap_ulong(this->LargeSectors);

	__byteswap_uint64(this->TotalSectors);
	__byteswap_uint64(this->MftStartLcn);
	__byteswap_uint64(this->Mft2StartLcn);

	__byteswap_uint64(this->VolumeSerialNumber);
	__byteswap_ulong(this->Checksum);
}

void BIOS_PARAMETER_BLOCK::encodeByteOrder()
{
	this->decodeByteOrder(); //Symmetrical
}
*/