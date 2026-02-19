#pragma once
#include "ntfsvolume.h"

VolumeLock::VolumeLock(HANDLE hVolume)
{
	DWORD bytesReturned;
	if (!DeviceIoControl(hVolume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
		throwLastOsError("FSCTL_LOCK_VOLUME");
	this->m_hVolume = hVolume;
	std::cerr << "Volume locked." << std::endl;
}

VolumeLock::~VolumeLock()
{
	DWORD bytesReturned = 0;
	if (this->m_hVolume != INVALID_HANDLE_VALUE) {
		if (!DeviceIoControl(this->m_hVolume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
			std::cerr << "WARNING: Cannot unlock volume. Error " << GetLastError() << "." << std::endl; //But ignore
		else
			std::cerr << "Volume unlocked." << std::endl;
		this->m_hVolume = INVALID_HANDLE_VALUE;
	}
}

Volume::~Volume() {
	this->close();
}

void Volume::open(const std::string& path, DWORD dwOpenMode)
{
	this->close();

	this->m_path = path;
	std::cerr << "Opening " << path << "..." << std::endl;

	// Open Handles
	this->m_h = CreateFileA(path.c_str(), dwOpenMode, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (this->m_h == INVALID_HANDLE_VALUE)
		throwLastOsError(std::string{ "Error opening " }+path + std::string{ ". Ensure you're running as Administrator." });

	// Get Volume Geometry (to know cluster size)
	DWORD bytesReturned;
	if (!DeviceIoControl(this->m_h, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &this->m_volumeData, sizeof(this->m_volumeData), &bytesReturned, NULL))
		throwLastOsError("FSCTL_GET_NTFS_VOLUME_DATA");
	assert(bytesReturned == sizeof(this->m_volumeData));

	verifyNtfsVersion();
}

void Volume::close()
{
	if (this->m_h != INVALID_HANDLE_VALUE) {
		CloseHandle(this->m_h);
		this->m_h = INVALID_HANDLE_VALUE;
	}
}

void Volume::verifyNtfsVersion()
{
	assert(extendedVolumeData().MajorVersion == 3);
	assert(extendedVolumeData().MinorVersion >= 0);
	assert(extendedVolumeData().MinorVersion <= 1);
}

VOLUME_BITMAP_BUFFER* Volume::getVolumeBitmap()
{
	// Get the Occupancy Bitmap
	// STARTING_LCN_INPUT_BUFFER contains the starting cluster (0)
	STARTING_LCN_INPUT_BUFFER startLcn = { 0 };

	// We need a buffer for the bitmap. A large volume needs a large buffer.
	// For simplicity, we'll allocate 1MB for the bitmap structure which covers a huge disk.
	DWORD bitmapBufferSize = 1024 * 1024;
	VOLUME_BITMAP_BUFFER* bitmapBuffer = nullptr;
	DWORD bytesReturned;

	DWORD err = 0;
	do {
		bitmapBuffer = (VOLUME_BITMAP_BUFFER*)realloc(bitmapBuffer, bitmapBufferSize);
		if (!DeviceIoControl(this->m_h, FSCTL_GET_VOLUME_BITMAP, &startLcn, sizeof(startLcn), bitmapBuffer, bitmapBufferSize, &bytesReturned, NULL))
			err = GetLastError();
		else
			err = 0;
		if (err == 0) {
			//pass
		}
		else if (err == ERROR_MORE_DATA || err == ERROR_INSUFFICIENT_BUFFER)
			bitmapBufferSize *= 4;
		else
			throwOsError(err, "FSCTL_GET_VOLUME_BITMAP");
	} while (err != 0);
	return bitmapBuffer;
}
