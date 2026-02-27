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

	VOLUME_BITMAP_BUFFER* queryVolumeBitmap();
};

/*
Что нужно от читалки с помощью OVERLAPPED:
- Очередь заданий на чтение
- Последовательно записываются в закольцованный буфер
- Буфер достаточно большой, чтобы вместить 4-5 максимальных заданий
- Что, если для крайнего нижнего задания не хватает места без закольцовывания?
 Это неудобно. Можно оставить ещё один максимальный участок в конце в качестве запаса, но не начинать внутри него.
- Что, если после закольцовывания не хватает места для нового участка, т.к. он больше прежнего? Ждём, пока не освободится.

Как должно работать получение следующего участка:
- Если есть возможность (есть свободное места для следующего участка), ставим запросы на чтение (столько, сколько влезет) и сдвигаем указатели.
  while (push_read(srcIt)) srcIt++;
  while (push_read(destIt)) srcIt++;
- Если с другого конца имеется прочитанный участок, достаём его
- Иначе висим на чём-то и ждём.

На чём висим?
try_push_back() берёт первый свободный OVERLAPPED и инициирует чтение на нём, затем добавляет пачку "overlapped, адрес, размер, буфер" в список чтений
finalize_front() проверяет, что первый пункт в списке чтений активен и ждёт, пока он закончится. Финализирует его. Выпускает overlapped.
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

class AsyncFileReader : public AsyncSlotProcessor {
public:
	using AsyncSlotProcessor::AsyncSlotProcessor;

	// Try to queue a new read request
	bool try_push_back(uint64_t offset, uint32_t size);

	// Wait for the oldest read and return its buffer
	uint8_t* finalize_front(uint32_t* bytes_read, uint64_t* offset);

	// Release the slot after processing
	void pop_front();
};


/*
Очередь на запись, аналогичная очереди на чтение.

Только на этот раз она блокирует сразу в push_back, если свободных слотов нет.
Там же она и освобождает завершившиеся.

Есть отдельная функция try_finalize_front, которая тоже блокирующая и возвращает false, если все слоты свободны.
Нужна, чтобы доделать задания, оставшиеся, когда добавлять уже больше нечего.

Можно было бы сделать отдельную бесконечную очередь, куда клиенты бы безблокировочно ставили задания,
а она бы тихонько в фоне несколькими или одним работником их писала.
Но всё равно пришлось бы ограничивать её длину, чтобы память не выросла бесконечно.
*/
class AsyncFileWriter : public AsyncSlotProcessor {
public:
	using AsyncSlotProcessor::AsyncSlotProcessor;

	// Queue a new write request
	void push_back(uint64_t offset, uint32_t size, void* data);

	// Block until the next queued write completes. False if no queued writes remains.
	bool try_pop_front(uint32_t* bytes_written, uint64_t* offset);
};