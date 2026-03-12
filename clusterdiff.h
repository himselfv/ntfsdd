#pragma once
/*
Processes a list of clusters and bitwise compares their contents on two different volumes.
*/
#include <memory>
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
	LCN ASYNC_BATCH_LEN = 160;
	int ASYNC_QUEUE_DEPTH = 16;

	//Comparison unit length.
	//Must be a multiple of logical sector size. Read batches must be a multiple of this.
	//Ideally this is physical sector size, except SSDs lie and emulate 512b where they have 4096b, so in this case we want 4096.
	//int64_t COMPARISON_UNIT_SZ = src.extendedVolumeData().BytesPerPhysicalSector;
	//Well, there's a safer way: work in clusters. It's not ideal if the true sector size is less, but it is also not bad. This is how the OS handles it after all.

protected:
	virtual void init();

public:
	ClusterProcessor(Volume& src, Volume& dest);
	virtual ~ClusterProcessor();
	virtual void process(CandidateClusterMap& srcSelection);

protected: //Progress tracking
	LCN m_lastProgress = 0;
	inline void doProgress(LCN lcn) {
		if (this->progressCallback)
			this->progressCallback->progress(lcn, false);
		if (lcn - m_lastProgress > 50000) {
			m_lastProgress = lcn;
			this->onProgress(lcn);
		}
	}
	virtual void onProgress(LCN lcn);
public:
	ProgressCallback* progressCallback = nullptr;
};


struct ClusterDiffStats {
	int64_t spansChecked = 0;
	LCN clustersChecked = 0;
	LCN clustersDiffCount = 0;
	size_t bytesRead = 0;
	size_t dirtySpanTotals = 0;
};

/*
Compares all the selected clusters on the src and on the dest.
*/
class ClusterDiffComparer : public ClusterProcessor {
	typedef ClusterProcessor inherited;
protected:
	std::unique_ptr<AsyncFileReader> srcReader;
	std::unique_ptr<AsyncFileReader> destReader;
	virtual void init() override;
public:
	ClusterDiffStats stats;
	BitmapBuf* diffMap; //Set to collect the map of clusters
	using inherited::inherited;
	virtual void process(CandidateClusterMap& srcSelection) override;
	virtual void onDirty(LCN lcn, void* data);
	virtual void onDirtySpan(LCN lcnFirst, LCN len, void* data);

protected:
	ClusterDiffStats m_progress_prevStats;
	DWORD m_progress_tm = 0;
public:
	bool printProgressDetails = false;
	virtual void onProgress(LCN lcn) override;
};

void printClusterSpan(LCN lcnFirst, LCN len, bool printClustersAsSpans, const std::string& separator);


/*
Compares all the selected clusters and immediately writes all changes from src to dest.
*/
class ClusterDiffWriter : public ClusterDiffComparer {
	typedef ClusterDiffComparer inherited;
protected:
	std::unique_ptr<AsyncFileWriter> writer;
	virtual void init() override;
public:
	using inherited::inherited;
	virtual void process(CandidateClusterMap& srcSelection) override;
	virtual void onDirtySpan(LCN lcnFirst, LCN len, void* data) override;
};


/*
Copies all the selected clusters from src to dest.
*/
class ClusterCopier : public ClusterProcessor {
	typedef ClusterProcessor inherited;
protected:
	std::unique_ptr<AsyncFileReader> srcReader;
	std::unique_ptr<AsyncFileWriter> writer;
	virtual void init() override;
public:
	ClusterDiffStats stats;
	using inherited::inherited;
	virtual void process(CandidateClusterMap& srcSelection) override;
};
