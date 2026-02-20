#pragma once
#include <windows.h>
#include "ntfs.h"
#include "ntfsutil.h"
#include "ntfsvolume.h"


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
  ValidDataLength: Actual size, rounded up to cluster.
*/
	ATTRIBUTE_RECORD_HEADER dataHeader;
public:
	inline const std::vector<VcnMapEntry>& vcnMap() { return this->m_vcnMap; }
	LCN getLcn(VCN vcn);
	//Некоторые файлы могут быть sparse, но иногда хочется проверить, что дыр нет.
	VCN getFirstMissingVcn();
	void addAttr(ATTRIBUTE_RECORD_HEADER* attr);
	inline int64_t sizeInBytes() { return this->dataHeader.Form.Nonresident.FileSize; }
};

/*
От MFT нам нужен следующий функционал:
1. Итерация по всем записям.
2. Возможность вытащить запись по её VCN. Зная размер записи, мы можем расчитать LCN + отступ записи в ней.
*/
class Mft : public NonResidentData {
public:
	Volume* vol;
	int32_t SectorsPerFileSegment = 0;
	int BytesPerFileSegment = 0;
public:
	Mft(Volume* volume);

	void load();
	void loadMftStructure(LCN lcnFirst);

	inline int64_t sizeInSegments() { return this->sizeInBytes() / this->BytesPerFileSegment; }

	std::vector<char> newSegmentBuf();
	void readSegmentByIndex(int64_t segmentIndex, FILE_RECORD_SEGMENT_HEADER* segment);
	void readSegmentLcn(LCN lcn, FILE_RECORD_SEGMENT_HEADER* segment);
	void readSegmentsVrbn(VRBN vrbn, FILE_RECORD_SEGMENT_HEADER* segment, int count);
	void readSegmentsNoSeek(FILE_RECORD_SEGMENT_HEADER* segment, int count);
	void segmentApplyFixups(FILE_RECORD_SEGMENT_HEADER* header);

	inline bool isValidSegment(FILE_RECORD_SEGMENT_HEADER* segment) {
		return (*((uint32_t*)(&(segment->MultiSectorHeader.Signature))) == *((uint32_t*)"FILE")); };
};


//#define SEGMENTITERATOR_EXCLUSIVE
//Exclusive mode: Assume no one else is moving the file pointer in the MFT you're holding.
//Saves on regular SetFilePointer calls. Less important with large batches.

#define SEGMENTITERATOR_BATCHREAD
//Read data in batches. Hugely speeds up processing.

#define SEGMENTITERATOR_BATCHSIZE 16
//Read this number of clusters/disk sectors/segments, whichever is larger.
//After 16-32x 4Kb the gains are marginal.

//#define SEGMENTITERATOR_TRACKPOS
//Track additional counters while iterating. Sometimes useful for debugging.

//#define SEGMENTITERATOR_ZEROMEM
//Zero the buffer memory before each read. Helps debug accidental reuse of stale data
//(e.g. you've read less than expected but old data looks valid and masks this).
/*
Пытается читать MFT быстрее, экономя на повторных установках FilePointer. Можно использовать одновременно максимум один!
*/
struct ExclusiveSegmentIterator {
	Mft* mft = nullptr;
	const VcnMapEntry* currentRun = nullptr;
	int remainingRuns = 0;
	VCN remainingSegmentsInRun = 0;
	LARGE_INTEGER vrbn;
#ifdef SEGMENTITERATOR_TRACKPOS
	int64_t lbn = 0;
	VCN vcn = 0;
#endif

	std::vector<uint8_t> buffer;
#ifdef SEGMENTITERATOR_BATCHREAD
	int64_t remainingBufferData = 0;
	FILE_RECORD_SEGMENT_HEADER* segment = nullptr;
	// Access the current value
	inline FILE_RECORD_SEGMENT_HEADER& operator*() { return *segment; }
#else
	// Access the current value
	inline FILE_RECORD_SEGMENT_HEADER& operator*() { return *((FILE_RECORD_SEGMENT_HEADER*)(buffer.data())); }
#endif
	void readCurrent();


	ExclusiveSegmentIterator(Mft* mft);

	// Comparison for the loop termination
	inline bool operator!=(const ExclusiveSegmentIterator& other) const {
		return (currentRun != other.currentRun) || (remainingSegmentsInRun != other.remainingSegmentsInRun);
	}

	void advanceRun();

	//Open currently selected run. It must have at least one segment.
	void openRun();

	// Advance the generator
	inline ExclusiveSegmentIterator& operator++() {
		if (remainingSegmentsInRun > 0) {
			remainingSegmentsInRun--;
			vrbn.QuadPart += mft->BytesPerFileSegment; //Отслеживаем всегда, т.к. нужно для нескольких механизмов сразу.
		}
		else {
			this->advanceRun();
			if (!this->currentRun)
				return *this;
		}

		this->readCurrent();
		return *this;
	}
};


class ExclusiveSegmentIter {
public:
	Mft* mft = nullptr;

	ExclusiveSegmentIter(Mft* mft) : mft(mft) {}

	// 3. Begin and End methods for range-based for loop
	inline ExclusiveSegmentIterator begin() { return{ mft }; }
	inline ExclusiveSegmentIterator end() { return{ nullptr }; }
};
