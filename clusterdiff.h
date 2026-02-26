#pragma once
/*
Processes a list of clusters and bitwise compares their contents on two different volumes.
*/
#include "bitmap.h"
#include "mftdiff.h"

struct ClusterDiffStats {
	LCN diffCount = 0;
	LCN clustersChecked = 0;
	int64_t runsChecked = 0;
};

class ClusterDiffComparer {
protected:
	Volume& src;
	Volume& dest;
public:
	ClusterDiffStats stats;
public:
	ClusterDiffComparer(Volume& src, Volume& dest);
	~ClusterDiffComparer();
	void process(CandidateClusterMap& srcDiff);
	virtual void onDirty(LCN lcn, void* data);
	virtual void onDirtySpan(LCN lcnFirst, LCN len, void* data);

protected:
	ClusterDiffStats m_progress_prevStats;
	DWORD m_progress_tm = 0;
public:
	virtual void onProgress(LCN lcn);

};

class ClusterDiffWriter : public ClusterDiffComparer {
public:
	AsyncFileWriter writer;
//	ClusterDiffWriter(Volume& src, Volume& dest);
//	virtual void onDirty(LCN lcn);
};