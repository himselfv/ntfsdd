#pragma once
/*
Calculates differences between the two related MFTs and builds a map of candidate clusters on disk
to be checked for differences.
*/
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "bitmap.h"
#include "ntfsmft.h"
#include "mftutil.h"

/*
There are two ways to store the candidate cluster list: as a bitmap or as a run list.

Run list can be more efficient in theory. We're normally not going to have a lot of run candidates
and we want to read/write in continuous chunks anyway.

Bitmaps are easier to compare/contrast, incl. w/other types of bitmaps, easier to work with.

With the comparatively efficient SpanIterator bitmaps do in practice work well,
so for now we're sticking with them.

Still, this is a class for run lists.
We're going to make this a little bit compatible with Bitmap, but do not expect us to handle overlapping ClusterRuns.
These do not normally occur in our tasks.
*/
class ClusterRunList : public std::vector<ClusterRun> {
public:
	ClusterRunList() {
		//Reserve a lot of space for efficiency
		this->reserve(8192);
	}
	inline void set(const LCN offset, const LCN length) { this->emplace_back(offset, length); }
	inline void set(const ClusterRun& run) { this->push_back(run); }
};

//typedef ClusterRunList CandidateClusterMap;
typedef BitmapBuf CandidateClusterMap;

struct DiffStats {
	SegmentNumber usedSegments = 0;
	SegmentNumber dirtySegments = 0;
	SegmentNumber multiSegments = 0;
	SegmentNumber filesSkipped = 0;
	SegmentNumber clustersSkipped = 0;
};

struct FileEntry {
	bool dirty = false; //If set, include this file's clusters.
	bool skip = false; //If set, ignore this file; do not include its clusters.
	bool multisegment = false; //Multisegment file detected
	std::vector<ClusterRun> runList;
	LCN totalClusters = 0;
	bool hasIndexAllocations = false;
	void reset() {
		this->dirty = false;
		this->skip = false;
		this->multisegment = false;
		this->runList.clear();
		this->totalClusters = 0;
		this->hasIndexAllocations = false;
	}
};
//MftFilemap is a map because we often need just a bunch of entries
typedef std::unordered_map<SegmentNumber, FileEntry> MftFilemap;


struct FilenameEntry {
	std::string filename;
	bool filenameNtfs = false;
	SegmentNumber parentDir = -1;
};

//FilenameMap is a vector bc when we build it, we need all of the names (all of the used segments; large share of all).
class FilenameMap : public std::vector<FilenameEntry>
{
public:
	void process(SegmentNumber segmentNo, ATTRIBUTE_RECORD_HEADER& attr);
	std::string getFullPath(SegmentNumber segmentNo);
};


/*
Scans one MFT table and builds the list of files.
*/
class MftScan {
protected:
	LCN TotalClusters = 0;
	int BytesPerFileRecordSegment = 0;
	LCN totalSegments = 0;
	virtual void scanInit();
	void processAttributes(SegmentNumber segmentNo, FileEntry* segmentEntry, FILE_RECORD_SEGMENT_HEADER* segment);

public:
	Mft& mftSrc;
	BitmapBuf srcUsed;

	/*
	Add files (segments) to mark them for skipping or force-copying.
	On exit, contains entries for some of the files processed, including any explicitly requested by flags below.
	*/
	MftFilemap filemap;

	//If set, $FILENAME attributes will be processed and file names collected. Only the first filename for every file is recorded.
	//Warning: slow and memory-non-trivial!
	FilenameMap* filenames = nullptr;

	ProgressCallback* progressCallback = nullptr;
	MftScan(Mft& mftSrc);

	virtual void scan();
};

/*
Compares two related MFT tables.
Builds maps of clusters mentioned by potentially changed file entries.
*/
class MftDiff : public MftScan {
protected:
	virtual void scanInit() override;

public:
	Mft& mftDest;

	DiffStats stats;

	//Populated during the scan
	CandidateClusterMap srcDiff;

	//If set, on exit filemap will include all files with dirty segments.
	bool filemapListDirty = false;

	//Index entries are unreliable: they sometimes update minor DUPLICATE_INFORMATION fields without changing anything in the MFT.
	//So far I have seen ONLY DUPLICATE_INFORMATION updates like that, but there's no guarantee.
	//And if we want a perfect mirror, we have to add all directory indices as candidate clusters:
	bool markAllIndexClustersDirty = false;

	/*
	Skip cluster listing/comparison/copying for these particular MFT segments.
	The MFT segments itself will still be copied. The file will be of correct size, pointing at correct (updated) cluster numbers.
	But the contents will be skipped, so the copy will point to random junk left there.

	Warning: Very limited application. Mostly for hiberfil.sys.
		Data leak risk.
	Determine the segment numbers for your file with "fsutil file layout" or by listing changed files w/this tool.
	*/
	void skipSegments(const std::unordered_set<SegmentNumber>& segments);

public:
	MftDiff(Mft& mftSrc, Mft& mftDest);
	void verifyMftRunsCompatible();
	virtual void scan() override;
	virtual void onDirtyFile(const SegmentNumber segmentNo, const FileEntry& fi);
};
