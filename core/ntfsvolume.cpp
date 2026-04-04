#pragma once
#include "ntfsvolume.h"
#include "ntfsmft.h"
#include "mftutil.h"
#include <memory>

VolumeLock::VolumeLock(Volume& volume, bool ignoreErrors)
	: volume(&volume)
{
	DWORD bytesReturned;
	if (!volume.ioctl(FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL)) {
		if (!ignoreErrors)
			throwLastOsError("FSCTL_LOCK_VOLUME");
		qWarning() << "Cannot lock volume. Error " << GetLastError() << "." << std::endl;
	}
	else {
		qDebug() << "Volume locked." << std::endl;
	}
}

VolumeLock::~VolumeLock()
{
	DWORD bytesReturned = 0;
	if (this->volume && this->volume->h() != INVALID_HANDLE_VALUE) {
		if (!this->volume->ioctl(FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
			qWarning() << "WARNING: Cannot unlock volume. Error " << GetLastError() << "." << std::endl; //But ignore
		else
			qDebug() << "Volume unlocked." << std::endl;
		this->volume = nullptr;
	}
}

Volume::Volume(const std::string& path, DWORD dwOpenMode)
{
	this->m_path = path;
	qInfo() << "Opening " << path << "..." << std::endl;

	// Open Handles
	this->m_h = CreateFileA(path.c_str(), dwOpenMode, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (this->m_h == INVALID_HANDLE_VALUE)
		throwLastOsError(std::string{ "Error opening " }+path + std::string{ ". Ensure you're running as Administrator." });
}

Volume::~Volume()
{
	if (this->m_h != INVALID_HANDLE_VALUE) {
		CloseHandle(this->m_h);
		this->m_h = INVALID_HANDLE_VALUE;
	}
}


/*
Все функции, которые принимают overlapped, будут работать и если его не передать.
Со внешним overlapped все они возвращают TRUE, если получили ERROR_IO_PENDING, то есть,
если вы передали свой overlapped, то должны при TRUE всегда делать GetOverlappedResult.
Это не идеально, если функция выполнилась сразу же, но так проще для вызывающих.
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

BOOL Volume::setFilePointer(
	_In_ uint64_t liDistanceToMove
)
{
	this->m_overlapped.Offset = (uint32_t)liDistanceToMove;
	this->m_overlapped.OffsetHigh = (uint32_t)(liDistanceToMove >> (sizeof(uint32_t)*8));
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

	DWORD numberOfBytesRead;
	auto ret = ReadFile(this->m_h, lpBuffer, nNumberOfBytesToRead, &numberOfBytesRead, overlapped);

	if (!ret) {
		auto err = GetLastError();
		if (err != ERROR_IO_PENDING)
			return FALSE;

		//Custom overlapped => return w/o waiting
		if (lpOverlapped != nullptr)
			return TRUE;

		//Blocking emulation => wait for results
		ret = GetOverlappedResult(this->m_h, overlapped, &numberOfBytesRead, TRUE);
	}

	if (ret) { //Either from immediate or delayed result
		if (lpNumberOfBytesRead)
			*lpNumberOfBytesRead = numberOfBytesRead;

		//Advance the offset to emulate what the file pointer does
		(*(uint64_t*)(&(overlapped->Offset))) += numberOfBytesRead;
	}

	return ret;
}


BOOL Volume::getOverlappedResult(
	_In_ LPOVERLAPPED lpOverlapped,
	_Out_ LPDWORD lpNumberOfBytesTransferred,
	_In_ BOOL bWait
)
{
	return GetOverlappedResult(this->m_h, lpOverlapped, lpNumberOfBytesTransferred, bWait);
}



/*
Volume Data strategy:
We have three cases to cover:
1. Sometimes we can do both valid FSCTL_QUERY_VOLUME_DATA and manual reading.
2. Or we can only do manual reading (files) and FSCTL_QUERY_VOLUME_DATA will fail OR return garbage.
  We have limited options on distinguishing the latter cases here.
3. Or we cannot and should not do any of that (blanket copy to empty partition/file).

The best would be to read manually, but then we lack certain fields, e.g. physical sector size.
Frankly, sometimes PhysicalSectorSize from FSCTL_QUERY_VOLUME_DATA is also fake and it would be nice to be able to override it.
But at least that's a starting point.

So regarding PhysicalSectorSize, we have to cache it and allow overriding.
We have to ignore what's in EXTENDED_VOLUME_DATA apart from using it as the initial value.
Our algorithm should be:
1. Read manually. (We won't have PhysicalSectorSize). Use logical sector as physical guess.
2. FSCTL_QUERY_VOLUME_DATA. If failed, NBD. If succeeded:
- If the target is a file, ignore the results. Windows returns results for its host volume.
- If it's a volume, maybe compare the results? Though allow to override and skip this. Many ways to go wrong.
- Update PhysicalSectorSize in any case. If it's a file, it's that file's host volume's physical sector size which is good.
3. Let the caller override our PhysicalSectorSize.

Special case: blank copying.
We cannot read any data from the volume but we still have to provide some data.

The only real things are 1. The max number of everything available. 2. Desired cluster sizes etc.
Perhaps we can have two different functions:
1. readVolumeData.
2. initVolumeData(VolumeData source)
*/

void Volume::readLayout()
{
	// Query volume data, but only so that we can steal PhysicalSectorSize from there
	this->queryPhysicalVolumeParams();

	// Get Volume Geometry (to know cluster size)
	this->readVolumeData(&this->m_volumeData.volumeData, &this->m_volumeData.extendedVolumeData);
	this->PhysicalSectorSize = this->m_volumeData.extendedVolumeData.BytesPerPhysicalSector;

	verifyNtfsVersion();
}

void Volume::verifyNtfsVersion()
{
	/*
	Let's be very conservative in the types of NTFS that we handle.
	As we test against other versions we'll expand this list.
	*/
	assert(extendedVolumeData().MajorVersion == 3);
	assert(extendedVolumeData().MinorVersion >= 0);
	assert(extendedVolumeData().MinorVersion <= 1);
}

/*
If the caller is certain it's working with the volume the system recognizes and not with data in a file,
it can request us to verify that our manually read layout matches the system understanding of things.
*/
void Volume::verifyPhysicalVolumeParams()
{
	CombinedVolumeData hostVolumeData{};
	if (!this->queryVolumeData(&hostVolumeData))
		throwLastOsError("FSCTL_GET_NTFS_VOLUME_PARAMS");

	auto& vd = this->volumeData();
	auto& hvd = hostVolumeData.volumeData;

	//We're only comparing those fields that we load
	assert(vd.BytesPerCluster == hvd.BytesPerCluster);
	assert(vd.BytesPerFileRecordSegment == hvd.BytesPerFileRecordSegment);
	assert(vd.BytesPerSector == hvd.BytesPerSector);
	assert(vd.ClustersPerFileRecordSegment == hvd.ClustersPerFileRecordSegment);
	assert(vd.MftStartLcn.QuadPart == hvd.MftStartLcn.QuadPart);
	assert(vd.Mft2StartLcn.QuadPart == hvd.Mft2StartLcn.QuadPart);
	assert(vd.MftValidDataLength.QuadPart == hvd.MftValidDataLength.QuadPart);
	assert(vd.NumberSectors.QuadPart == hvd.NumberSectors.QuadPart);
	assert(vd.TotalClusters.QuadPart == hvd.TotalClusters.QuadPart);
	assert(vd.VolumeSerialNumber.QuadPart == hvd.VolumeSerialNumber.QuadPart);
}



/*
Initializes our in-memory understanding of the volume's layout based on the desired data provided.
We're not going to write this to the volume! You'll have to handle this separately, by copying the clusters.
*/
void Volume::initLayout(const NTFS_VOLUME_DATA_BUFFER& volumeData, const NTFS_EXTENDED_VOLUME_DATA& extData)
{
	this->queryPhysicalVolumeParams();

	this->m_volumeData.volumeData = volumeData;
	this->m_volumeData.extendedVolumeData = extData;

	if (this->PhysicalSectorSize < (int32_t)extData.BytesPerPhysicalSector)
		this->PhysicalSectorSize = extData.BytesPerPhysicalSector;
}



void Volume::setPhysicalSectorSize(int32_t size)
{
	this->PhysicalSectorSize = size;
	this->m_allocator.setAlignment(size);
}

AlignedBuffer Volume::newAlignedBuffer(size_t desiredSize)
{
	if (desiredSize % this->PhysicalSectorSize != 0)
		desiredSize = desiredSize + this->PhysicalSectorSize - (desiredSize % this->PhysicalSectorSize);
	auto ret = AlignedBuffer(m_allocator);
	ret.resize(desiredSize);
	return ret;
}


/*
The volume we're working with may be stored on some physical volume, either as a partition or as file.
We want to query the host volume's PhysicalSectorSize and maybe some other params.
*/
void Volume::queryPhysicalVolumeParams()
{
	this->PhysicalSectorSize = 0;

	//In Windows, the same call returns the host volume info, including some physical storage info,
	//whether we hold the volume itself or the file on it.
	CombinedVolumeData hostVolumeData{};
	if (this->queryVolumeData(&hostVolumeData)) {
		this->PhysicalSectorSize = hostVolumeData.extendedVolumeData.BytesPerPhysicalSector;
		return;
	}

	//Another way to maybe get the PhysicalSectorSize. Though if the previous one failed this will likely also fail.
	STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment = {};
	if (this->queryStorageAlignment(&alignment))
		this->PhysicalSectorSize = alignment.BytesPerPhysicalSector;

	//We can either leave 0/1 in PhysicalSectorSize, or choose say 512 as a default. Idk.
	//0/1 is better because we'll catch alignment errors.
}



void Volume::readVolumeData(NTFS_VOLUME_DATA_BUFFER* volData, NTFS_EXTENDED_VOLUME_DATA* extData)
{
	DWORD bytesReturned = 0;

	//If we're dealing with a live volume, our buffers have to be aligned on the sector size
	auto buffer = this->newAlignedBuffer(sizeof(BIOS_PARAMETER_BLOCK));

	OSCHECKBOOL(this->setFilePointer( 0 ));
	OSCHECKBOOL(this->read(&buffer[0], (DWORD)buffer.size(), &bytesReturned, nullptr));
	assert(bytesReturned == buffer.size());

	auto& block = *(BIOS_PARAMETER_BLOCK*)(&buffer[0]);
//	assert(block.decodeAndTestChecksum()); //Not needed!

	memset(volData, 0, sizeof(*volData));
	auto& vd = *volData;

	vd.VolumeSerialNumber.QuadPart = block.VolumeSerialNumber;
	vd.NumberSectors.QuadPart = block.TotalSectors;
	assert(block.SectorsPerCluster != 0);
	vd.TotalClusters.QuadPart = block.TotalSectors / block.SectorsPerCluster;
	//LARGE_INTEGER FreeClusters;
	//LARGE_INTEGER TotalReserved;
	vd.BytesPerSector = block.BytesPerSector;
	vd.BytesPerCluster = block.BytesPerSector*block.SectorsPerCluster;
	assert((int8_t)block.ClustersPerFileRecordSegment != 0);
	if ((int8_t)block.ClustersPerFileRecordSegment > 0) {
		vd.BytesPerFileRecordSegment = vd.BytesPerCluster * block.ClustersPerFileRecordSegment;
		vd.ClustersPerFileRecordSegment = block.ClustersPerFileRecordSegment;
	}
	else { //Negative values mean powers of 2, in bytes.
		vd.BytesPerFileRecordSegment = 1UL << (-(int8_t)(block.ClustersPerFileRecordSegment));
		vd.ClustersPerFileRecordSegment = 0;
	}
	//LARGE_INTEGER MftValidDataLength;
	vd.MftStartLcn.QuadPart = block.MftStartLcn;
	vd.Mft2StartLcn.QuadPart = block.Mft2StartLcn;
	//LARGE_INTEGER MftZoneStart;
	//LARGE_INTEGER MftZoneEnd;


	//To read MftValidDataLength we have to access record 0 of the MFT
	//Ntfs version is in record 3
	auto mftBytes = this->newAlignedBuffer(vd.BytesPerFileRecordSegment * 4);

	OSCHECKBOOL(this->setFilePointer( 0 + vd.MftStartLcn.QuadPart*vd.BytesPerCluster ));
	OSCHECKBOOL(this->read(&mftBytes[0], (DWORD)mftBytes.size(), &bytesReturned, nullptr));
	assert(bytesReturned == mftBytes.size());
	auto segment = (FILE_RECORD_SEGMENT_HEADER*)(&mftBytes[0]);

	//Spin up minimal MFT processing
	//We need this to check FILE signatures, apply fixups etc.
	int validCount = 0;
	Mft minimalMft(this);
	minimalMft.loadMinimal();
	minimalMft.processSegments(segment, 4, &validCount);
	assert(validCount == 4);

	vd.MftValidDataLength.QuadPart = 0;
	for (auto& attr : AttributeIterator(segment))
		if (attr.TypeCode == $DATA) {
			assert(attr.FormCode == NONRESIDENT_FORM);
			vd.MftValidDataLength.QuadPart = attr.Form.Nonresident.ValidDataLength;
			break;
		}
	assert(vd.MftValidDataLength.QuadPart != 0);

	if (extData == nullptr) return;
	memset(extData, 0, sizeof(*extData));
	extData->ByteCount = sizeof(*extData); //The extent of the data we cover here

	segment = (FILE_RECORD_SEGMENT_HEADER*)(&mftBytes[vd.BytesPerFileRecordSegment * 3]);
	VOLUME_INFORMATION* volumeInfo = nullptr;
	for (auto& attr : AttributeIterator(segment))
		if (attr.TypeCode == $VOLUME_INFORMATION) {
			assert(attr.FormCode == RESIDENT_FORM);
			assert(attr.Form.Resident.ValueLength >= sizeof(VOLUME_INFORMATION));
			volumeInfo = (VOLUME_INFORMATION*)(attr.ResidentValuePtr());
			break;
		}
	assert(volumeInfo != nullptr);
	extData->MajorVersion = volumeInfo->MajorVersion;
	extData->MinorVersion = volumeInfo->MinorVersion;

	//We don't know bytes per physical sector here.
	//This is really problematic but let's at least put something in it.
	extData->BytesPerPhysicalSector = volData->BytesPerSector;
}

/*
WARNING! If passed a file handle doesn't fail but returns its parent volume params.
*/
bool Volume::queryVolumeData(CombinedVolumeData* data)
{
	DWORD bytesReturned;
	auto ret = this->ioctl(FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, data, sizeof(*data), &bytesReturned, NULL);
	if (ret)
		assert(bytesReturned == sizeof(CombinedVolumeData));
	return (ret!=FALSE);
}

bool Volume::queryStorageAlignment(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR* alignment)
{
	DWORD bytesReturned = 0;

	STORAGE_PROPERTY_QUERY query = {};
	query.PropertyId = StorageAccessAlignmentProperty;
	query.QueryType = PropertyStandardQuery;
	if (this->ioctl(IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
		alignment, sizeof(*alignment),
		&bytesReturned, nullptr))
		return true;

	//This is supposed to be working from Vista/Server 2008 but I couldn't get it to work in 10.
	auto err = GetLastError();
	if (err != ERROR_INVALID_FUNCTION)
		throwOsError(err);
	return false;
}

/*
https://community.osr.com/t/locking-ntfs-volume-dismounts-it/16419/9
> FSCTL_GET_VOLUME_BITMAP for NTFS volumes succeeds only if either
> - you don’t lock the volume at all
> - or set dwShareMode in CreateFile to FILE_SHARE_READ or zero and lock it
> However, this doesn’t look right. The docs say that for FSCTL_LOCK_VOLUME,
> you must set dwShareMode in CreateFile to FILE_SHARE_READ | FILE_SHARE_WRITE.
From my experience, this also applies to shadow copies.

People advise:
> If you open volume with share access 0 file system will effectively lock the
> volume for you - you don’t need to issue FSCTL_LOCK_VOLUME.
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
		*offset = slot->ovl.Offset + ((uint64_t)slot->ovl.OffsetHigh << (sizeof(uint32_t) * 8));

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
		assert(this->try_pop_front(nullptr, nullptr));
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
		throwLastOsError("AsyncFileWriter::write");

	// Release the slot after processing
	slots[tail]->is_pending = false;
	tail = (tail + 1) % slots.size();
	pending_count--;

	assert(transferred == slot->bytesUsed);
	if (bytes_written) *bytes_written = transferred;
	if (offset != nullptr)
		*offset = slot->ovl.Offset + ((uint64_t)slot->ovl.OffsetHigh << (sizeof(uint32_t) * 8));
	return true;
}
