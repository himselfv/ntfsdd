#pragma once
#include <windows.h>
#include <winioctl.h>
#include <assert.h>
#include <iostream>
#include <vector>
#include "ntfs.h"
#include "ntfsutil.h"


class VolumeLock {
protected:
	HANDLE m_hVolume = INVALID_HANDLE_VALUE;
public:
	VolumeLock(HANDLE hVolume)
	{
		DWORD bytesReturned;
		if (!DeviceIoControl(hVolume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
			throwLastOsError("FSCTL_LOCK_VOLUME");
		this->m_hVolume = hVolume;
		std::cerr << "Volume locked." << std::endl;
	}
	~VolumeLock()
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
};


#pragma pack(push, 1)
struct CombinedVolumeData {
	NTFS_VOLUME_DATA_BUFFER volumeData;
	NTFS_EXTENDED_VOLUME_DATA extendedVolumeData;
};
#pragma pack(pop)

class Volume {
protected:
	std::string m_path{};
	HANDLE m_h = INVALID_HANDLE_VALUE;
	CombinedVolumeData m_volumeData;
public:
	inline HANDLE h() { return this->m_h; }
	inline NTFS_VOLUME_DATA_BUFFER& volumeData() { return this->m_volumeData.volumeData; }
	inline NTFS_EXTENDED_VOLUME_DATA& extendedVolumeData() { return this->m_volumeData.extendedVolumeData; }

public:
	virtual ~Volume() {
		this->close();
	}

	virtual void open(const std::string& path, DWORD dwOpenMode)
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
	virtual void close()
	{
		if (this->m_h != INVALID_HANDLE_VALUE) {
			CloseHandle(this->m_h);
			this->m_h = INVALID_HANDLE_VALUE;
		}
	}

	void verifyNtfsVersion()
	{
		assert(extendedVolumeData().MajorVersion == 3);
		assert(extendedVolumeData().MinorVersion >= 0);
		assert(extendedVolumeData().MinorVersion <= 1);
	}

	VOLUME_BITMAP_BUFFER* getVolumeBitmap()
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
};