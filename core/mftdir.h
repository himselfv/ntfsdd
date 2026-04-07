#pragma once
/*
Directory index and filename processing.
*/
#include <vector>
#include "ntfs.h"
#include "util.h"
#include "ntfsvolume.h"
#include "ntfsmft.h"
#include "mftutil.h"


struct AttrFilename {
public:
	FILE_NAME* fn = nullptr;
	AttrFilename(ATTRIBUTE_RECORD_HEADER* attr);
	AttrFilename(FILE_NAME* fn) : fn(fn) {}

	inline std::string name()
	{
		//Sic! wcharToUtf8 wants one char after the last one
		return wcharToUtf8(fn->FileName, fn->FileName + fn->FileNameLength);
	}

	inline operator std::string()
	{
		return this->name();
	}
};



struct FilenameEntry {
	std::string filename;
	SegmentNumber parentDir = -1;
	bool filenameNtfs = false;
	void update(ATTRIBUTE_RECORD_HEADER& attr);
};

//FilenameMap is a vector bc when we build it, we need all of the names (all of the used segments; large share of all).
class FilenameMap : public std::vector<FilenameEntry>
{
public:
	void process(SegmentNumber segmentNo, ATTRIBUTE_RECORD_HEADER& attr);
	std::string getFullPath(SegmentNumber segmentNo);
};



/*
We need a dynamically loading $Bitmap for dir index processing.
This class receives a BitmapBuf, initializes it to a given size once the first chunk is available,
and populates it with data as it arrives.
Use pos to figure out how much has already been loaded.
*/
class BitmapProcessor : public AttributeCollectorProcessor {
protected:
	BitmapBuf& m_bitmap;
	virtual size_t tryReadEntry(byte* buf, size_t len) override;
public:
	size_t bytesProcessed = 0;
	BitmapProcessor(Volume* vol, BitmapBuf& bitmap);
};



/*
Processes resident and non-resident $INDEX_ROOT and $INDEX_ALLOCATION attributes, extracts and collects INDEX_ENTRYes.
Plain version: We do not care about the tree structure.

Filter attributes and only pass us those with the same name ($I30 for dirs).
The NonResidentDataProcessor here is meant for $INDEX_ALLOCATION chunks. Pass them normally,
and pass $INDEX_ROOT directly to processIndexRoot().

Update: We also need to process $BITMAP with the same name as there's no way to tell
which Index Allocation Buffers are in use otherwise.
$BITMAP can be non-resident so we need another collector.

In general, we cannot start processing INDEX_ALLOCATION until we have compiled BITMAP.
We could track the VCN map loading extent for the BITMAP and allow INDEX_ALLOCATION
processing accordingly, but it's a bother.
*/
struct DirIndexEntry {
	SegmentNumber segmentNo;
	std::string filename;
};
class DirIndexProcessor : public AttributeCollectorProcessor {
protected:
	INDEX_ROOT m_root;
	inline bool haveIndexRoot() { return this->m_root.BytesPerIndexBuffer > 0; }
	BitmapBuf m_bitmap;
	BitmapProcessor m_bitmapLoader;
	int m_blockNo = 0;

	virtual size_t tryReadEntry(byte* buf, size_t len) override;
	void readIndexEntries(INDEX_HEADER* header);
	size_t tryReadIndexEntry(byte* buf, size_t len);
public:
	std::vector<DirIndexEntry> entries;

	DirIndexProcessor(Volume* vol);

	//Pass index root here. It's always resident, only one instance of it in the multi-segment file.
	void processIndexRoot(void* data, size_t len);

	//Pass matching $BITMAP here. Resident or non-resident.
	void processBitmapAttr(ATTRIBUTE_RECORD_HEADER& attr);
};


class DirEntryLoader : public MultiSegmentFileLoader, public DirIndexProcessor
{
public:
	//The entry we've been tasked with loading is indeed marked as a directory.
	//Available after the base segment is processed.
	//You can also check if it IN FACT had INDEX_ROOT by checking DirIndexProcessor::haveIndexRoot().
	bool isDir = false;
	FilenameEntry filename;
	DirEntryLoader(Mft& mft);
	virtual void loadSegment(FILE_RECORD_SEGMENT_HEADER* segment) override;
	virtual void processAttr(ATTRIBUTE_RECORD_HEADER& attr) override;
};



/*
Directory reading:
Loads and caches directory entries. Resolves paths by splitting and following them dir by dir.
Let's distinguish between assertion failures and user failures (no such file, not a dir etc). Let's return Windows error codes on this.
*/
struct MftDirEntry
{
	SegmentNumber segmentNo;
	std::string name;
	std::vector<DirIndexEntry> children;
};
class DirectoryTreeLoader
{
protected:
	std::unordered_map<SegmentNumber, MftDirEntry> m_data;
	Mft& mft;
	MftDirEntry load(SegmentNumber segmentNo);
public:
	DirectoryTreeLoader(Mft& mft);
	MftDirEntry& get(SegmentNumber segmentNo);
	SegmentNumber traverse(const std::string& path);
	std::unordered_set<SegmentNumber> traversePaths(const std::vector<std::string>& paths);
};



/*
Syntactic sugar for inclusion/exclusion options on the command line.
Collects inclusions/exclusions in various ways (segments, file names) and then resolves all of them into two lists:
1. Segments to be excluded.
2. Segments whose direct descendants are to be excluded.

Usage:
 - Populate any of the public sets.
 - setTree();
   resolve();
*/
struct SegmentInclusionOptions {
protected:
	DirectoryTreeLoader* dirTree = nullptr;
	void resolvePaths();
public:
	std::unordered_set<SegmentNumber> segments;
	std::unordered_set<SegmentNumber> segmentRoots;
	std::vector<std::string> paths {};
	std::vector<std::string> files {};

	void setTree(DirectoryTreeLoader* dirTree);
	void resolve();
	void printDebugInfo();
};
