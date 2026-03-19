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
	SegmentNumber dirtyBecauseOfCmp = 0;
	SegmentNumber dirtyBecauseOfIndex = 0; //Segments marked dirty because of a "mark all indexes dirty" rule
	SegmentNumber dirtyBecauseOfParent = 0; //Segments marked dirty because their parent is "mark children dirty".
	void print(int BytesPerCluster);
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

	//If set, filemap will include ALL files. Warning: This may take a lot of memory.
	bool filemapListAll = false;

	/*
	Index entries are unreliable: they sometimes update minor DUPLICATE_INFORMATION fields without changing anything in the MFT.
	So far I have seen ONLY DUPLICATE_INFORMATION updates like that, but there's no guarantee.
	And if we want a perfect mirror, we have to add all directory indices as candidate clusters:
	*/
	bool markAllIndexClustersDirty = false;

	/*
	Skip cluster listing/comparison/copying for these particular MFT segments.
	The MFT segments itself will still be copied. The file will be of correct size, pointing at correct (updated) cluster numbers.
	But the contents will be skipped, so the copy will point to random junk left there.

	The segments skipped this way WILL be tracked normally in the filemap, filenames etc. Their dirty status will be tracked.
	But their clusters WILL NOT BE MARKED in the difference bitmap. That's the only effect.

	Warning: Very limited application. Mostly for hiberfil.sys.
		Data leak risk.
	Determine the segment numbers for your file with "fsutil file layout" or by listing changed files w/this tool.
	*/
	void skipSegments(const std::unordered_set<SegmentNumber>& segments);

	/*
	These segments will always be marked as dirty (their data clusters will always get rcw/copied).
	Some system files may get updated without the MFT entry changing at all.
	System files 0-35 are automatically added to this.
	*/
	void addDirtySegments(const std::unordered_set<SegmentNumber>& segments);

	/*
	All MFT entries ONE LEVEL below these will be marked dirty (their data clusters will always get rcw/copied).
	
	In folders like $Extend a lot of files are updated by the system in a way that their MFT entries do not change.
	For files with predefined segmentNumbers (MFT 0-35) we can hardcode forced-dirty status (see addDirtySegments),
	but some of those file IDs are arbitrary.
	Instead of trying to track this by name we skip "everything inside $Extend".

	Entries 0-35 are added automatically. For System Volume Information, determine its dynamic ID and pass here.

	Note: This works ONE LEVEL down. More levels would require pre-scanning the MFT to build the folder tree
	which is slow.
	*/
	std::unordered_set<SegmentNumber> dirtySubtreeRoots {};
	void addDirtySubtreeRoots(const std::unordered_set<SegmentNumber>& segments);


public:
	MftDiff(Mft& mftSrc, Mft& mftDest);
	void verifyMftRunsCompatible();
	virtual void scan() override;
	virtual void onDirtyFile(const SegmentNumber segmentNo, const FileEntry& fi);
};
