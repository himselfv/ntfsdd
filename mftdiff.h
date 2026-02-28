#pragma once
/*
Calculates differences between the two related MFTs and builds a map of candidate clusters on disk
to be checked for differences.
*/
#include <vector>
#include <unordered_map>
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
	bool dirty = false;
	std::vector<ClusterRun> runList;
};

struct ShortFileInfo {
	std::string filename{};
	LCN totalClusters = 0;
	bool skip = false; //If set, ignore this file; do not include its clusters nor its mft segments.
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
	BitmapBuf srcUsed;
	bool printDirtyFiles = false;
	CandidateClusterMap srcDiff;
	std::unordered_map<int64_t, FileEntry> filemap;

public:
	MftDiff(Mft& mftSrc, Mft& mftDest);
	void scan();
	virtual void onProgress(SegmentNumber idx, SegmentNumber totalSegments);
	virtual void onDirtyFile(const ShortFileInfo& fi);
};
