#pragma once
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include "ntfs.h"
#include "ntfsutil.h"


class Volume;

class VolumeLock {
protected:
	Volume* volume;
public:
	VolumeLock(Volume& volume);
	~VolumeLock();
};


struct OverlappedWithEvent {
public:
	OVERLAPPED ol{ 0 };
	OverlappedWithEvent() {
		ol.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	}
	~OverlappedWithEvent() {
		CloseHandle(ol.hEvent);
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
	OverlappedWithEvent m_overlapped;
	CombinedVolumeData m_volumeData;
public:
	inline HANDLE h() { return this->m_h; }
	inline NTFS_VOLUME_DATA_BUFFER& volumeData() { return this->m_volumeData.volumeData; }
	inline NTFS_EXTENDED_VOLUME_DATA& extendedVolumeData() { return this->m_volumeData.extendedVolumeData; }

public:
	virtual ~Volume();

	virtual void open(const std::string& path, DWORD dwOpenMode);
	virtual void close();
	BOOL ioctl(_In_ DWORD dwIoControlCode,
		_In_reads_bytes_opt_(nInBufferSize) LPVOID lpInBuffer,
		_In_ DWORD nInBufferSize,
		_Out_writes_bytes_to_opt_(nOutBufferSize, *lpBytesReturned) LPVOID lpOutBuffer,
		_In_ DWORD nOutBufferSize,
		_Out_opt_ LPDWORD lpBytesReturned,
		_Inout_opt_ LPOVERLAPPED lpOverlapped
	);
	BOOL setFilePointer(
		_In_ LARGE_INTEGER liDistanceToMove
	);
	BOOL read(
		_Out_writes_bytes_to_opt_(nNumberOfBytesToRead, *lpNumberOfBytesRead) __out_data_source(FILE) LPVOID lpBuffer,
		_In_ DWORD nNumberOfBytesToRead,
		_Out_opt_ LPDWORD lpNumberOfBytesRead,
		_Inout_opt_ LPOVERLAPPED lpOverlapped
	);
	BOOL getOverlappedResult(
		_In_ LPOVERLAPPED lpOverlapped,
		_Out_ LPDWORD lpNumberOfBytesTransferred,
		_In_ BOOL bWait
	);

	void verifyNtfsVersion();

	VOLUME_BITMAP_BUFFER* queryVolumeBitmap();
};