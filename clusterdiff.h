#pragma once
/*
Processes a list of clusters and bitwise compares their contents on two different volumes.
*/
#include "bitmap.h"
#include "mftdiff.h"


/*
Base class. Processes selected clusters in parallel on the src and on the dest.
Inherited to implement compare, read-compare-write and copy.
*/
class ClusterProcessor {
protected:
	Volume& src;
	Volume& dest;
	DWORD BytesPerCluster;
public:
	//Max size of the single chunk for a read operation, in clusters
	//Bigger spans will be processed in chunks. Too large chunks would hinder parallelization when chunks of wildly different sizes are processed one after another.
	static constexpr LCN ASYNC_BATCH_LEN = 160;
	static constexpr int ASYNC_QUEUE_DEPTH = 16;

	//Comparison unit length.
	//Must be a multiple of logical sector size. Read batches must be a multiple of this.
	//Ideally this is physical sector size, except SSDs lie and emulate 512b where they have 4096b, so in this case we want 4096.
	//int64_t COMPARISON_UNIT_SZ = src.extendedVolumeData().BytesPerPhysicalSector;
	//Well, there's a safer way: work in clusters. It's not ideal if the true sector size is less, but it is also not bad. This is how the OS handles it after all.
public:
	ClusterProcessor(Volume& src, Volume& dest);
	virtual ~ClusterProcessor();
};


struct ClusterDiffStats {
	LCN diffCount = 0;
	LCN clustersChecked = 0;
	int64_t runsChecked = 0;
};

/*
Compares all the selected clusters on the src and on the dest.
*/
class ClusterDiffComparer : public ClusterProcessor {
public:
	AsyncFileReader srcReader;
	AsyncFileReader destReader;
public:
	ClusterDiffStats stats;
	bool printDiff = false;
	bool printClustersAsSpans = false;
public:
	ClusterDiffComparer(Volume& src, Volume& dest);
	virtual void process(CandidateClusterMap& srcDiff);
	virtual void onDirty(LCN lcn, void* data);
	virtual void onDirtySpan(LCN lcnFirst, LCN len, void* data);

protected:
	ClusterDiffStats m_progress_prevStats;
	DWORD m_progress_tm = 0;
public:
	virtual void onProgress(LCN lcn);
};

void printClusterSpan(LCN lcnFirst, LCN len, bool printClustersAsSpans);


/*
Compares all the selected clusters and immediately writes all changes from src to dest.
*/
class ClusterDiffWriter : public ClusterDiffComparer {
public:
	AsyncFileWriter writer;
	ClusterDiffWriter(Volume& src, Volume& dest);
	virtual void process(CandidateClusterMap& srcDiff) override;
	virtual void onDirtySpan(LCN lcnFirst, LCN len, void* data) override;
};


/*
Copies all the selected clusters from src to dest.
*/
class ClusterCopier : public ClusterProcessor {
public:
	AsyncFileReader srcReader;
	AsyncFileWriter writer;
	ClusterDiffStats stats;
	ClusterCopier(Volume& src, Volume& dest);
	virtual void process(CandidateClusterMap& srcDiff);
	virtual void onProgress(LCN lcn);
};
