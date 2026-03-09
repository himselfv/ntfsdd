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
};

struct FileEntry {
	bool dirty = false; //If set, include this file's clusters.
	bool skip = false; //If set, ignore this file; do not include its clusters.
	bool multisegment = false; //Multisegment file detected
	bool filenameNtfs = false;
	std::vector<ClusterRun> runList;
	std::string filename{};
	LCN totalClusters = 0;
};

/*
Compares two related MFT tables.
Builds maps of clusters mentioned by potentially changed file entries.
*/
class MftDiff {
public:
	Mft& mftSrc;
	Mft& mftDest;

	DiffStats stats;

	//Populated during the scan
	BitmapBuf srcUsed;
	CandidateClusterMap srcDiff;

	/*
	Add files (segments) to mark them for skipping or force-copying.
	On exit, contains entries for some of the files processed, including any explicitly requested by flags below.
	*/
	std::unordered_map<int64_t, FileEntry> filemap;

	//If set, on exit filemap will include all files with dirty segments
	bool filemapListDirty = false;

	//If set, $FILENAME attributes will be processed and added to entries in the filemap
	bool filemapNeedNames = false;
	bool printDirtyFiles = false;

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
	void scan();
	virtual void onProgress(SegmentNumber idx, SegmentNumber totalSegments);
	virtual void onDirtyFile(const SegmentNumber segmentNo, const FileEntry& fi);
};
