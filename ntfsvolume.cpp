#pragma once
#include "ntfsvolume.h"

VolumeLock::VolumeLock(Volume& volume)
	: volume(&volume)
{
	DWORD bytesReturned;
	if (!volume.ioctl(FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
		throwLastOsError("FSCTL_LOCK_VOLUME");
	std::cerr << "Volume locked." << std::endl;
}

VolumeLock::~VolumeLock()
{
	DWORD bytesReturned = 0;
	if (this->volume && this->volume->h() != INVALID_HANDLE_VALUE) {
		if (!this->volume->ioctl(FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
			std::cerr << "WARNING: Cannot unlock volume. Error " << GetLastError() << "." << std::endl; //But ignore
		else
			std::cerr << "Volume unlocked." << std::endl;
		this->volume = nullptr;
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
	this->m_h = CreateFileA(path.c_str(), dwOpenMode, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (this->m_h == INVALID_HANDLE_VALUE)
		throwLastOsError(std::string{ "Error opening " }+path + std::string{ ". Ensure you're running as Administrator." });

	// Get Volume Geometry (to know cluster size)
	DWORD bytesReturned;
	this->ioctl(FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &this->m_volumeData, sizeof(this->m_volumeData), &bytesReturned, NULL);
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

/*
¬се функции, которые принимают overlapped, будут работать и если его не передать.
—о внешним overlapped все они возвращают TRUE, если получили ERROR_IO_PENDING, то есть,
если вы передали свой overlapped, то должны при TRUE всегда делать GetOverlappedResult.
Ёто не идеально, если функци€ выполнилась сразу же, но так проще дл€ вызывающих.
*/

BOOL Volume::ioctl(_In_ DWORD dwIoControlCode,
	_In_reads_bytes_opt_(nInBufferSize) LPVOID lpInBuffer,
	_In_ DWORD nInBufferSize,
	_Out_writes_bytes_to_opt_(nOutBufferSize, *lpBytesReturned) LPVOID lpOutBuffer,
	_In_ DWORD nOutBufferSize,
	_Out_opt_ LPDWORD lpBytesReturned,
	_Inout_opt_ LPOVERLAPPED lpOverlapped
)
{
	auto overlapped = lpOverlapped;
	if (overlapped == nullptr)
		overlapped = &this->m_overlapped;

	if (DeviceIoControl(this->m_h, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, overlapped))
		return TRUE;

	auto err = GetLastError();
	if (err != ERROR_IO_PENDING)
		return FALSE;

	if (lpOverlapped == nullptr)
		return GetOverlappedResult(this->m_h, overlapped, lpBytesReturned, TRUE);
	return TRUE;
}

BOOL Volume::setFilePointer(
	_In_ LARGE_INTEGER liPointer
	)
{
	this->m_overlapped.Offset = liPointer.LowPart;
	this->m_overlapped.OffsetHigh = liPointer.HighPart;
	return TRUE;
}

BOOL Volume::read(
	_Out_writes_bytes_to_opt_(nNumberOfBytesToRead, *lpNumberOfBytesRead) __out_data_source(FILE) LPVOID lpBuffer,
	_In_ DWORD nNumberOfBytesToRead,
	_Out_opt_ LPDWORD lpNumberOfBytesRead,
	_Inout_opt_ LPOVERLAPPED lpOverlapped
)
{
	auto overlapped = lpOverlapped;
	if (overlapped == nullptr)
		overlapped = &this->m_overlapped;

	if (ReadFile(this->m_h, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, overlapped))
		return TRUE;

	auto err = GetLastError();
	if (err != ERROR_IO_PENDING)
		return FALSE;

	if (lpOverlapped == nullptr)
		return GetOverlappedResult(this->m_h, overlapped, lpNumberOfBytesRead, TRUE);
	return TRUE;
}

BOOL Volume::getOverlappedResult(
	_In_ LPOVERLAPPED lpOverlapped,
	_Out_ LPDWORD lpNumberOfBytesTransferred,
	_In_ BOOL bWait
)
{
	return GetOverlappedResult(this->m_h, lpOverlapped, lpNumberOfBytesTransferred, bWait);
}



void Volume::verifyNtfsVersion()
{
	assert(extendedVolumeData().MajorVersion == 3);
	assert(extendedVolumeData().MinorVersion >= 0);
	assert(extendedVolumeData().MinorVersion <= 1);
}


/*
https://community.osr.com/t/locking-ntfs-volume-dismounts-it/16419/9
> FSCTL_GET_VOLUME_BITMAP for NTFS volumes succeeds only if either
> - you donТt lock the volume at all
> - or set dwShareMode in CreateFile to FILE_SHARE_READ or zero and lock it
> However, this doesnТt look right. The docs say that for FSCTL_LOCK_VOLUME,
> you must set dwShareMode in CreateFile to FILE_SHARE_READ | FILE_SHARE_WRITE.
From my experience, this also applies to shadow copies.

People advise:
> If you open volume with share access 0 file system will effectively lock the
> volume for you - you donТt need to issue FSCTL_LOCK_VOLUME.
> This open fails if there are some handles opened on the volume and
> subsequent open requests will fail while volume is opened.
Might not be quite right. Kernel mode actors might still consider it unlocked? Idk.

Another option is to read the bitmap manually. But this won't work for FAT.
Not that we handle FAT here.
*/
VOLUME_BITMAP_BUFFER* Volume::queryVolumeBitmap()
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
		if (!this->ioctl(FSCTL_GET_VOLUME_BITMAP, &startLcn, sizeof(startLcn), bitmapBuffer, bitmapBufferSize, &bytesReturned, NULL))
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


AsyncSlot::AsyncSlot(size_t buffer_size) {
	ZeroMemory(&ovl, sizeof(OVERLAPPED));
	ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	buffer = (uint8_t*)_aligned_malloc(buffer_size, 4096);
	is_pending = false;
}

AsyncSlot::~AsyncSlot() {
	CloseHandle(ovl.hEvent);
	_aligned_free(buffer);
}

AsyncSlotProcessor::AsyncSlotProcessor(HANDLE file, size_t queue_depth, size_t chunk_size)
	: hFile(file), max_chunk_size(chunk_size) {
	for (size_t i = 0; i < queue_depth; ++i) {
		slots.push_back(new AsyncSlot(max_chunk_size));
	}
}

AsyncSlotProcessor::~AsyncSlotProcessor() {
	for (auto slot : slots) delete slot;
}

// Try to queue a new read request
bool AsyncFileReader::try_push_back(uint64_t offset, uint32_t size) {
	if (pending_count >= slots.size()) return false;
	assert(size <= max_chunk_size);

	AsyncSlot* slot = slots[head];

	// Setup OVERLAPPED offset
	slot->ovl.Offset = (DWORD)(offset & 0xFFFFFFFF);
	slot->ovl.OffsetHigh = (DWORD)(offset >> 32);

	// Reset the event before issuing the read
	ResetEvent(slot->ovl.hEvent);

#ifdef AFR_ZEROMEM
	memset(slot->buffer, 0, max_chunk_size);
#endif

	BOOL result = ReadFile(hFile, slot->buffer, size, NULL, &slot->ovl);

	if (!result && GetLastError() != ERROR_IO_PENDING)
		throwLastOsError();

	slot->bytesUsed = size;
	slot->is_pending = true;
	head = (head + 1) % slots.size();
	pending_count++;
	return true;
}

// Wait for the oldest read and return its buffer
uint8_t* AsyncFileReader::finalize_front(uint32_t* bytes_read, uint64_t* offset) {
	if (pending_count == 0) return nullptr;

	AsyncSlot* slot = slots[tail];
	DWORD transferred = 0;

	// This blocks efficiently if the I/O isn't done.
	// If it's already done, it returns immediately.
	BOOL result = GetOverlappedResult(hFile, &slot->ovl, &transferred, TRUE);
	if (!result)
		throwLastOsError();

	assert(transferred == slot->bytesUsed);
	if (bytes_read) *bytes_read = transferred;
	if (offset != nullptr)
		*offset = slot->ovl.Offset + slot->ovl.OffsetHigh << (sizeof(uint32_t) * 8);

	return slot->buffer;
}

// Release the slot after processing
void AsyncFileReader::pop_front() {
	if (pending_count == 0) return;

	slots[tail]->is_pending = false;
	tail = (tail + 1) % slots.size();
	pending_count--;
}


// Try to queue a new read request
void AsyncFileWriter::push_back(uint64_t offset, uint32_t size, void* data) {
	assert(size <= max_chunk_size);

	if (pending_count >= slots.size()) {
		//Pop at least one slot
		assert(this->try_pop_front(nullptr));
		assert(pending_count < slots.size());
	}

	AsyncSlot* slot = slots[head];

	// Setup OVERLAPPED offset
	slot->ovl.Offset = (DWORD)(offset & 0xFFFFFFFF);
	slot->ovl.OffsetHigh = (DWORD)(offset >> 32);

	// Reset the event before issuing the read
	ResetEvent(slot->ovl.hEvent);

	memcpy(slot->buffer, data, size);

	BOOL result = WriteFile(hFile, slot->buffer, size, NULL, &slot->ovl);

	if (!result && GetLastError() != ERROR_IO_PENDING)
		throwLastOsError();

	slot->bytesUsed = size;
	slot->is_pending = true;
	head = (head + 1) % slots.size();
	pending_count++;
}

// Wait for the oldest read and return its buffer
bool AsyncFileWriter::try_pop_front(uint32_t* bytes_written, uint64_t* offset) {
	if (pending_count == 0) return false;

	AsyncSlot* slot = slots[tail];
	DWORD transferred = 0;

	// This blocks efficiently if the I/O isn't done.
	// If it's already done, it returns immediately.
	BOOL result = GetOverlappedResult(hFile, &slot->ovl, &transferred, TRUE);
	if (!result)
		throwLastOsError();

	// Release the slot after processing
	slots[tail]->is_pending = false;
	tail = (tail + 1) % slots.size();
	pending_count--;

	assert(transferred == slot->bytesUsed);
	if (bytes_written) *bytes_written = transferred;
	if (offset != nullptr)
		*offset = slot->ovl.Offset + slot->ovl.OffsetHigh << (sizeof(uint32_t) * 8);
}
