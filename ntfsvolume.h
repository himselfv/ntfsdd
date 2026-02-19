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
	VolumeLock(HANDLE hVolume);
	~VolumeLock();
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
	virtual ~Volume();

	virtual void open(const std::string& path, DWORD dwOpenMode);
	virtual void close();

	void verifyNtfsVersion();

	VOLUME_BITMAP_BUFFER* getVolumeBitmap();
};