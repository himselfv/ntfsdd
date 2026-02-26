#pragma once
/*
Processes a list of clusters and bitwise compares their contents on two different volumes.
*/
#include "bitmap.h"
#include "mftdiff.h"

struct ClusterDifferStats {
	LCN diffCount = 0;
	LCN clustersChecked = 0;
	int64_t runsChecked = 0;
};

class ClusterDiffer {
protected:
	Volume& src;
	Volume& dest;
public:
	ClusterDifferStats stats;
public:
	ClusterDiffer(Volume& src, Volume& dest);
	~ClusterDiffer();
	void process(CandidateClusterMap& srcDiff);
	virtual void onDirty(LCN lcn);

protected:
	ClusterDifferStats m_progress_prevStats;
	DWORD m_progress_tm = 0;
public:
	virtual void onProgress(LCN lcn);


};