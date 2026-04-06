#pragma once
#include <unordered_set>
#include <unordered_map>
#include <windows.h>
#include "ntfs.h"
#include "util.h"
#include "ntfsvolume.h"
#include "bitmap.h"


//Volume-relative byte number
typedef LARGE_INTEGER VRBN;

/*
Non-resident attribute data is described by one or many (in different segments) attribute chunks
which contain lists of consecutive Data Runs for VCNs in the first..last span declared for the attribute in this chunk.

Segment 1:
  $Data VCN0=0, VCN1=100, Runs=0-15,116-200
Segment 2:
  $Data VCN0=101, VCN1=200, Runs=301-320,521-600

We convert this to a flat sorted list VCN->LCN, len:
0: 0
16: 116
101: 301
121: 521

ATTRIBUTE_FLAG_SPARSE and the sparse runs in those are not supported atm even though it should be easy.
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
	inline bool haveDataHeader() { return dataHeader.RecordLength > 0; }
public:
	Volume* vol;
	NonResidentData(Volume* vol);
	inline const std::vector<VcnMapEntry>& vcnMap() { return this->m_vcnMap; }
	LCN getLcn(VCN vcn);
	//Some files can be sparse but sometimes we legit need to check there are no holes:
	VCN getFirstMissingVcn();
	void addAttrChunk(ATTRIBUTE_RECORD_HEADER* attr);
	//Even though this is called NonResidentData, descendants use us for all sorts of things so let's try to be compatible.
	inline int64_t sizeInBytes() {
		if (this->dataHeader.FormCode == RESIDENT_FORM)
			return this->dataHeader.Form.Resident.ValueLength;
		return this->dataHeader.Form.Nonresident.FileSize;
	}
	inline int64_t sizeInClusterMultiples() {
		if (this->dataHeader.FormCode == RESIDENT_FORM) {
			//Strictly speaking you don't need a cluster multiple to handle this, but since the function name promises
			//Magic rounding up code:
			int64_t ret = this->dataHeader.Form.Resident.ValueLength + vol->volumeData().BytesPerCluster - 1;
			return ret - (ret % vol->volumeData().BytesPerCluster);
		}
		return this->dataHeader.Form.Nonresident.AllocatedLength;
	}

	//Doesn't work for files with holes.
	//Only reads full clusters (== multiples of logical sectors). Make sure you have enough space - use sizeInClusterMultiples()!
	void readAll(void* buf);
};


/*
A class to process non-resident (and sometimes resident!) attribute data sequentially, as chunks of the VCN->LCN map become available.
Mostly needed for $ATTRIBUTE_LIST processing below but can be reused for anything. See comments in the CPP.

Users:
- Pass addAttr() as you encounter matching attribute chunks.
- Call advance() after each chunk or at opportune times.
- assert_no_leftovers() when processing is complete.

Descendants:
- Override tryReadEntry()
- Try to read a complete something from the avaiable data. Return the size read (which guarantees
  another call to tryReadEntry), or 0 (which delays next call until more data is available).

Resident attributes:
If your tryReadEntry() does not rely on non-resident properties too much, you can call processResidentAttr() on a resident data and it will work.
But many properties of the inherited NonResidentData will be garbage so beware.
DO NOT call advance() in this case. This is why it's "process" and not "addResidentAttr".
*/
class AttributeCollectorProcessor : public NonResidentData {
protected:
	VCN m_nextVcn = 0;
	bool m_vcnEof = false;

	AlignedBuffer m_buf; //Resize to be at least 2 clusters in size
	byte* m_pos = nullptr;
	inline size_t remainingBytesInBuf() { return m_buf.size() - (m_pos - m_buf.data()); }
	void packBuffer(); //Moves unprocessed data to the beginning of the buffer

	//Try to read next virtual cluster
	bool tryReadMore();

	//Process another entry. Override to handle specifics.
	virtual size_t tryReadEntry(byte* buf, size_t len) = 0;
public:
	AttributeCollectorProcessor(Volume* vol);

	//Process the resident version of the attribute. Has to be the ONLY instance of this attribute.
	//Be careful! Your tryReadMore() has to be ready for RESIDENT_FORM of the ATTRIBUTE_RECORD_HEADER dataHeader.
	void processResidentAttr(ATTRIBUTE_RECORD_HEADER& attr);

	//Process a complete independent chunk of data, usually from a resident version of the attribute.
	//Be careful! Only works if your tryReadMore() is ATTRIBUTE_RECORD_HEADER-independent.
	void processData(void* data, size_t len);

	//When processing segments and encountering a non-resident attribute chunk, call base addDataAttr() + advance()
	//This will scan all currently available new sequential data and process any complete entry.
	int advance();

	//Asserts that this attribute is either missing or has been completely processed, with no unprocessed data left.
	inline void assert_all_processed() {
		assert(this->bof() || this->eof(), "Unprocessed data left in AttributeCollectorProcessor");
	}

	//True if no attributes has ever been passed here, resident or non-resident.
	//False if we have seen at least one attribute chunk.
	inline bool bof() {
		return this->m_vcnMap.empty() //No non-resident chunks seen
			&& !this->m_vcnEof; //No complete resident attributes processed
	}

	//True if we have read everything there is to read in this non-resident data.
	//True after any single call to processResidentAttr().
	//False if no attributes has ever been passed here.
	inline bool eof() {
		return this->m_vcnEof //Have read all clusters there are to read
			&& this->remainingBytesInBuf()==0;
	}
};


/*
Processes resident and non-resident $ATTRIBUTE_LIST entries as they're passed to it,
and accumulates information, currently just a growing list of all segments mentioned in those.
*/
class AttributeListProcessor : public AttributeCollectorProcessor {
protected:
	//Process another $ATTRIBUTE_LIST entry if it's available. Store segments encountered.
	virtual size_t tryReadEntry(byte* buf, size_t len) override;
public:
	//All segments encountered in this $ATTRIBUTE_LIST so far.
	std::vector<SegmentNumber> segments;

	using AttributeCollectorProcessor::AttributeCollectorProcessor;
};


/*
Base class. Receives MFT and the base segment number for a file. Loads that segment and any referenced in it sequentially.
Processes $ATTRIBUTE_LIST automatically to retrieve additional segment numbers.
Descendants should override processAttr() for further attribute collection and processing.

Standalone usage:
  MyFileLoader loader(vol);
  loader.load(mft, baseSegmentNo);
  for (auto& result : loader.results()) { ... }

Single-pass MFT processing usage:
  if (!isBaseSegmentEntry || !isInterestingFile) continue;
  loader = this->m_loaderMap[segmentNo];
  loader.loadSegment(segment); //no MFT needed, segment already read
*/
class Mft;
class MultiSegmentFileLoader {
protected:
	AttributeListProcessor attrList;
	virtual void processAttr(ATTRIBUTE_RECORD_HEADER& attr);
public:
	MultiSegmentFileLoader(Volume* vol);
	virtual ~MultiSegmentFileLoader();
	//Start with the base segment and use Mft to load all segments sequentially
	virtual void load(Mft& mft, SegmentNumber baseSegmentNo);
	//Load this particular segment
	virtual void loadSegment(Mft& mft, SegmentNumber segmentNo);
	//Load this particular segment from data
	virtual void loadSegment(FILE_RECORD_SEGMENT_HEADER* segment);
};



inline bool sectorsCheckSignature(MULTI_SECTOR_HEADER& header, const UCHAR signature[4])
{
	return (*((uint32_t*)(&(header.Signature))) == *((const uint32_t*)(&signature[0])));
}

//Applies fixups to a given number of sectors, according to the fixup table in MULTI_SECTOR_HEADER.
//Thankfully, MULTI_SECTOR_HEADER is the first thing in all structs where it is used so we need no separate data pointer.
void sectorsApplyFixups(MULTI_SECTOR_HEADER* data, int sectors, DWORD bytesPerSector);


/*
MFT access:
- Segment iteration
- Ability to extract a segment by its number/VCN
*/
class Mft : public NonResidentData, public MultiSegmentFileLoader {
public:
	int32_t SectorsPerFileSegment = 0;
	int BytesPerFileSegment = 0;

protected:
	virtual void processAttr(ATTRIBUTE_RECORD_HEADER& attr) override;

public:
	Mft(Volume* volume);

	void loadMinimal();
	void load();

	inline int64_t sizeInSegments() { return this->sizeInBytes() / this->BytesPerFileSegment; }

	AlignedBuffer newSegmentBuf();
	void readSegmentByIndex(SegmentNumber segmentNo, FILE_RECORD_SEGMENT_HEADER* segment);
	void readSegmentByIndexMinimal(SegmentNumber segmentNo, FILE_RECORD_SEGMENT_HEADER* segment);
	void readSegmentsVrbn(VRBN vrbn, FILE_RECORD_SEGMENT_HEADER* segment, int count);
	void readSegmentsNoSeek(FILE_RECORD_SEGMENT_HEADER* segment, int count, LPOVERLAPPED lpOverlapped = nullptr);
	void processSegments(FILE_RECORD_SEGMENT_HEADER* segment, int count, int* validCount = nullptr);
	void segmentApplyFixups(FILE_RECORD_SEGMENT_HEADER* segment);

	inline static bool IsValidSegment(FILE_RECORD_SEGMENT_HEADER* segment) {
		return sectorsCheckSignature(segment->MultiSectorHeader, SIGNATURE_FILE);
	}
};

//$BITMAP file. Not the same thing as a $BITMAP attribute on a normal file.
//This one stores its data as $DATA.
class NtfsBitmapFile : public NonResidentData {
public:
	VOLUME_BITMAP_BUFFER* buf = nullptr;
	inline uint8_t* data() { return buf->Buffer; }
	inline LCN totalClusters() { return buf->BitmapSize.QuadPart; }
	NtfsBitmapFile(Volume* vol, Mft* mft);
	~NtfsBitmapFile();
	Bitmap asBitmap() { return Bitmap(buf->Buffer, buf->BitmapSize.QuadPart); }
};



/*
Segment iterators.
*/

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

	AlignedBuffer buffer;
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
	SegmentIteratorOverlapped(SegmentIteratorOverlapped&& other);
	SegmentIteratorOverlapped& operator=(SegmentIteratorOverlapped&& other);

	/*
	In this iterator iteration over runs (read queueing) and the pointer to the processed data are independent.
	currentRun is set from the start and until input clusters are exhausted. After that it's zero. Before that it always points to the next cluster to read.
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
