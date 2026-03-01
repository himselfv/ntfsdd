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
	VolumeLock(Volume& volume, bool ignoreErrors = false);
	~VolumeLock();
};


struct Overlapped : public OVERLAPPED {
public:
	Overlapped() {
		this->hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	}
	~Overlapped() {
		CloseHandle(this->hEvent);
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
	Overlapped m_overlapped;
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

	//Read* functions read the data directly
	bool readVolumeData(CombinedVolumeData* data);

	//Query* functions use FSCTL-type codes to ask this information from the system
	bool queryVolumeData(CombinedVolumeData* data);
	VOLUME_BITMAP_BUFFER* queryVolumeBitmap();
};



/*
Overlapped reader/writer queues.
Accept up to N simultaneous outstanding requests, each up to max_block_size in size.
The slot memory is limited, please slice your requests according to the limits you set.
*/

#define AFR_ZEROMEM

struct AsyncSlot {
	OVERLAPPED ovl;
	uint8_t* buffer;
	size_t bytesUsed;
	bool is_pending;
	AsyncSlot(size_t buffer_size);
	~AsyncSlot();
};

class AsyncSlotProcessor {
public:
	HANDLE hFile;
	size_t max_chunk_size;
	std::vector<AsyncSlot*> slots;

	size_t head = 0; // Where we push new reads
	size_t tail = 0; // Where we finalize/process
	size_t pending_count = 0;

public:
	AsyncSlotProcessor(HANDLE file, size_t queue_depth, size_t chunk_size);
	~AsyncSlotProcessor();
};

/*
Overlapped reader queue.

Usage:
- Push as many read requests as the queue accepts.
- Wait for the first to complete (blocking).
- Extract the data, process it.
- Release the slot.
*/
class AsyncFileReader : public AsyncSlotProcessor {
public:
	using AsyncSlotProcessor::AsyncSlotProcessor;

	// Try to queue a new read request
	bool try_push_back(uint64_t offset, uint32_t size);

	// Wait for the oldest read and return its buffer
	uint8_t* finalize_front(uint32_t* bytes_read, uint64_t* offset = nullptr);

	// Release the slot after processing
	void pop_front();
};


/*
Overlapped writer queue.

Similar to the reader queue but push_back() blocks when there are no free slots
and auto-finalizes completed requests.

Separate try_finalize_front() which is also blocking and returns false if all slots are free.
Needed to finalize the requests remaining when there's nothing left to push_back().

Usage:
- Push all write requests to the queue (blocking).
- No need to pop manually, push auto-pops auto-verifies success.
- On exit: Pop the remainder of the requests until false.

Why not an endless queue which accepts infinite requests and quietly works through them
in the background?
We would still have to limit it bc the data sizes here can easily surpass RAM.
*/
class AsyncFileWriter : public AsyncSlotProcessor {
public:
	using AsyncSlotProcessor::AsyncSlotProcessor;

	// Queue a new write request
	void push_back(uint64_t offset, uint32_t size, void* data);

	// Block until the next queued write completes. False if no queued writes remains.
	bool try_pop_front(uint32_t* bytes_written, uint64_t* offset = nullptr);
};