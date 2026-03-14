#pragma once
#include <windows.h>
#include "ntfs.h"
#include "util.h"
#include "ntfsvolume.h"
#include "bitmap.h"


//Volume-relative byte number
typedef LARGE_INTEGER VRBN;

/*
Файл описывается одним или несколькими (в разных сегментах) атрибутами $Data,
которые содержат списки подряд идущих DataRun, описывающие VCN с первого по последний указанные для данного атрибута в данном сегменте.

Например:
Сегмент 1:
$Data VCN0=0, VCN1=100, Runs=0-15,116-200
Сегмент 2:
$Data VCN0=101, VCN1=200, Runs=301-320,521-600

Мы превращаем всё это в плоскую структуру:
0: 0
16: 116
101: 301
121: 521

ATTRIBUTE_FLAG_SPARSE и встречающиеся в нём sparse runs сейчас не поддерживаем, хотя в принципе это несложно.
*/
struct VcnMapEntry {
	VCN vcnStart = 0;
	LCN lcnStart = 0;
	LCN len = 0;
	VcnMapEntry(VCN vcnStart, LCN lcnStart, LCN len) : vcnStart(vcnStart), lcnStart(lcnStart), len(len) {}
};
class NonResidentData {
protected:
	std::vector<VcnMapEntry> m_vcnMap;
public:
/*
Attribute record header for the first of our run-listing attributes (LowestVcn==0).
Contains:
  AllocatedLength: Multiple of cluster size, total allocated space.
  FileSize: Actual size, in bytes, precise.
  ValidDataLength: <= actual size, performance optimization, do not use. Not always aligned on clusters in practice.
*/
	ATTRIBUTE_RECORD_HEADER dataHeader;
public:
	Volume* vol;
	NonResidentData(Volume* vol) : vol(vol) {}
	inline const std::vector<VcnMapEntry>& vcnMap() { return this->m_vcnMap; }
	LCN getLcn(VCN vcn);
	//Некоторые файлы могут быть sparse, но иногда хочется проверить, что дыр нет.
	VCN getFirstMissingVcn();
	void addAttr(ATTRIBUTE_RECORD_HEADER* attr);
	inline int64_t sizeInBytes() { return this->dataHeader.Form.Nonresident.FileSize; }
	inline int64_t sizeInClusterMultiples() { return this->dataHeader.Form.Nonresident.AllocatedLength; }

	//Doesn't work for files with holes.
	//Only reads full clusters (== multiples of logical sectors). Make sure you have enough space - use sizeInClusterMultiples()!
	void readAll(void* buf);
};


/*
От MFT нам нужен следующий функционал:
1. Итерация по всем записям.
2. Возможность вытащить запись по её VCN. Зная размер записи, мы можем расчитать LCN + отступ записи в ней.
*/
class Mft : public NonResidentData {
public:
	int32_t SectorsPerFileSegment = 0;
	int BytesPerFileSegment = 0;
public:
	Mft(Volume* volume);

	void loadMinimal();
	void load();
	void loadMftStructure(LCN lcnFirst);

	inline int64_t sizeInSegments() { return this->sizeInBytes() / this->BytesPerFileSegment; }

	std::vector<char> newSegmentBuf();
	void readSegmentByIndex(int64_t segmentIndex, FILE_RECORD_SEGMENT_HEADER* segment);
	void readSegmentLcn(LCN lcn, FILE_RECORD_SEGMENT_HEADER* segment);
	void readSegmentsVrbn(VRBN vrbn, FILE_RECORD_SEGMENT_HEADER* segment, int count);
	void readSegmentsNoSeek(FILE_RECORD_SEGMENT_HEADER* segment, int count, LPOVERLAPPED lpOverlapped = nullptr);
	void processSegments(FILE_RECORD_SEGMENT_HEADER* segment, int count, int* validCount = nullptr);
	static bool IsValidSegment(FILE_RECORD_SEGMENT_HEADER* segment);
	void segmentApplyFixups(FILE_RECORD_SEGMENT_HEADER* segment);

	inline bool isValidSegment(FILE_RECORD_SEGMENT_HEADER* segment) {
		return (*((uint32_t*)(&(segment->MultiSectorHeader.Signature))) == *((uint32_t*)"FILE")); };
};

class NtfsBitmapFile : public NonResidentData {
public:
	VOLUME_BITMAP_BUFFER* buf = nullptr;
	inline uint8_t* data() { return buf->Buffer; }
	inline LCN totalClusters() { return buf->BitmapSize.QuadPart; }
	NtfsBitmapFile(Volume* vol, Mft* mft);
	~NtfsBitmapFile();
	Bitmap asBitmap() { return Bitmap(buf->Buffer, buf->BitmapSize.QuadPart); }
};


#define SEGMENTITERATOR_BATCHSIZE 16
//Read data in batches. Hugely speeds up processing.
//Read this number of clusters/disk sectors/segments, whichever is larger.
//After 16-32x 512b the gains are marginal.

#define SEGMENTITERATOR_QUEUEDEPTH 8
//Queue depth when reading via overlapped asynchronous requests.

//#define SEGMENTITERATOR_TRACKPOS
//Track additional counters while iterating. Sometimes useful for debugging.

//#define SEGMENTITERATOR_ZEROMEM
//Zero the buffer memory before each read. Helps debug accidental reuse of stale data
//(e.g. you've read less than expected but old data looks valid and masks this).


#define SI_SKIP_INVALID		(1UL << 1)
//Skip segments without FILE header

#define SI_SKIP_NOT_IN_USE	(1UL << 2)
//Skip segments without IN_USE set


struct SegmentIteratorBase {
	typedef uint32_t Flags;

	Mft* mft = nullptr;
	Flags flags = 0;

	const VcnMapEntry* currentRun = nullptr;
	int remainingRuns = 0;

	SegmentIteratorBase(Mft* mft, Flags flags = 0);
	int64_t selectMaxChunkSize();

	// Comparison for the loop termination
	// Since we're reading into temporary buffers, iterators are in general not comparable. We only have to care about != end().
	inline bool operator!=(const SegmentIteratorBase& other) const {
		return (currentRun != other.currentRun) || (segment != other.segment);
	}

	FILE_RECORD_SEGMENT_HEADER* segment = nullptr;
	// Access the current value
	inline FILE_RECORD_SEGMENT_HEADER& operator*() { return *segment; }
	inline FILE_RECORD_SEGMENT_HEADER* operator->() { return segment; }

	//Advances 1 times until eof() or reads current segment
	virtual void advance();

	//Advances 0 or more times until eof() or the first segment matching the criteria
	void readCurrent();

	// Advance the generator
	inline SegmentIteratorBase& operator++() {
		this->advance();
		this->readCurrent();
		return *this;
	}
};

struct SegmentIteratorBuffered : public SegmentIteratorBase {
	LARGE_INTEGER vrbn;
#ifdef SEGMENTITERATOR_TRACKPOS
	int64_t lbn = 0;
	VCN vcn = 0;
#endif

	SegmentNumber remainingSegmentsInRun = 0;

	std::vector<uint8_t> buffer;
	int64_t remainingBufferData = 0;

	SegmentIteratorBuffered(Mft* mft, Flags flags = 0);

	void advanceRun();

	//Open currently selected run. It must have at least one segment.
	void openRun();

	virtual void advance() override;

	void readInt();
};



struct SegmentIteratorOverlapped : public SegmentIteratorBase {
	AsyncFileReader* reader = nullptr;

	int64_t remainingBufferData = 0;

#ifdef SEGMENTITERATOR_TRACKPOS
	int64_t readsPushed = 0;
	int64_t readsPulled = 0;
	int64_t readsPopped = 0;
	int64_t readClustersPushed = 0;
	int64_t readClustersPulled = 0;
	int64_t readSegmentsPulled = 0;
	int64_t segmentAdvances = 0;
#endif

	SegmentIteratorOverlapped(Mft* mft, Flags flags = 0);
	~SegmentIteratorOverlapped();

	/*
	В этом итераторе итерация по кластерам runs (постановка на чтение) и указатель на прочитанные данные независимы.
	currentRun установлен с самого начала и до исчерпания входных кластеров. После этого он нулевой. До этого он всегда указывает на следующий кластер для чтения.
	*/
	int64_t currentClusterInRun = 0;
	void advanceRun();
	void openRun();

	virtual void advance() override;
};


//typedef SegmentIteratorBuffered SegmentIterator;
typedef SegmentIteratorOverlapped SegmentIterator;


class SegmentIter {
public:
	Mft* mft = nullptr;
	SegmentIterator::Flags flags = 0;

	SegmentIter(Mft* mft, SegmentIterator::Flags flags = 0) : mft(mft), flags(flags) {}

	// Begin and End methods for range-based for loop
	inline SegmentIterator begin() { return{ mft, flags }; }
	inline SegmentIterator end() { return{ nullptr }; }
};

