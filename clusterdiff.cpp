#pragma once
#include "clusterdiff.h"
#include "ntfsutil.h"
#include "ntfsmft.h"



void printClusterSpan(LCN lcnFirst, LCN len, bool printClustersAsSpans)
{
	if (printClustersAsSpans)
		std::cout << lcnFirst << ":" << len << std::endl;
	else {
		while (len > 0) {
			std::cout << lcnFirst << std::endl;
			len--;
			lcnFirst++;
		}
	}
}



ClusterProcessor::ClusterProcessor(Volume& src, Volume& dest)
	: src(src), dest(dest)
{
	BytesPerCluster = this->src.volumeData().BytesPerCluster;
	assert(BytesPerCluster > 0);
}

ClusterProcessor::~ClusterProcessor()
{
}



ClusterDiffComparer::ClusterDiffComparer(Volume& src, Volume& dest)
	: ClusterProcessor(src, dest),
	srcReader(src.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*BytesPerCluster),
	destReader(dest.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*BytesPerCluster)
{
}

void ClusterDiffComparer::process(CandidateClusterMap& srcDiff)
{
	LCN lastProgress = 0;
	this->onProgress(0);

	auto slicedRuns = slice_runs(BitmapSpans(&srcDiff), ASYNC_BATCH_LEN);
	auto sliceIt = slicedRuns.begin();

	while (true) {
		//Part 1. Push read commands into the queue
		while (sliceIt != slicedRuns.end()) {
			if (!srcReader.try_push_back(sliceIt->offset*BytesPerCluster, (uint32_t)(sliceIt->length*BytesPerCluster)))
				break;
			assert(destReader.try_push_back(sliceIt->offset*BytesPerCluster, (uint32_t)(sliceIt->length*BytesPerCluster)));
			++sliceIt;
		}

		//Part 2. Extract next element.
		//Easier to pop exactly one read a cycle and push one new. If multiple are done, next few cycles will just be fast.
		uint32_t bytesRead = 0, bytesRead2 = 0;
		uint64_t offset = 0, offset2 = 0;
		auto srcPtr = srcReader.finalize_front(&bytesRead, &offset);
		if (srcPtr == nullptr) {
			assert(sliceIt == slicedRuns.end());
			assert(srcReader.pending_count == 0);
			assert(destReader.pending_count == 0);
			break;
		}
		assert(offset % BytesPerCluster == 0);
		assert(bytesRead % BytesPerCluster == 0);
		auto destPtr = destReader.finalize_front(&bytesRead2, &offset2);
		assert(destPtr != nullptr);
		assert(bytesRead == bytesRead2);
		assert(offset == offset2);
		
		stats.spansChecked++;

		LCN lcn = offset / BytesPerCluster;
		LCN lastClean = lcn - 1;

		//Compare these cluster by cluster
		while (bytesRead > 0) {
			auto diff = memcmp(srcPtr, destPtr, src.volumeData().BytesPerCluster);
			if (diff != 0) {
				stats.clustersDiffCount++;
				this->onDirty(lcn, srcPtr);
			}
			else {
				if (lastClean != lcn - 1)
					this->onDirtySpan(lastClean + 1, lcn - lastClean - 1, srcPtr - (lcn - lastClean - 1)*src.volumeData().BytesPerCluster);
				lastClean = lcn;
			}
			lcn++;
			bytesRead -= BytesPerCluster;
			stats.clustersChecked++;
		}
		//We do not support "dirty spans" across chunk boundaries so finalize one if we have one
		if (lastClean != lcn - 1)
			this->onDirtySpan(lastClean + 1, lcn - lastClean - 1, srcPtr - (lcn - lastClean - 1)*src.volumeData().BytesPerCluster);

		srcReader.pop_front();
		destReader.pop_front();

		if (stats.clustersChecked - lastProgress > 50000) {
			lastProgress = stats.clustersChecked;
			this->onProgress((offset2 + bytesRead2) / BytesPerCluster);
		}
	}
}

void ClusterDiffComparer::onProgress(LCN lcn)
{
	if (lcn == 0) {
		this->m_progress_prevStats = this->stats;
		this->m_progress_tm = GetTickCount();
		return;
	}

	int64_t thisClusterCount = this->stats.clustersChecked - this->m_progress_prevStats.clustersChecked;
	int64_t thisRunCount = this->stats.spansChecked - this->m_progress_prevStats.spansChecked;
	auto t2 = GetTickCount() - this->m_progress_tm + 1; //To avoid div by zero.
	this->m_progress_tm += t2;

	std::cerr << "Clusters: " << stats.clustersChecked << ", runs: " << thisRunCount << ", t=" << t2 << ", cpm=" << (double)thisClusterCount / t2 << ", rpm=" << (double)thisRunCount / t2 << std::endl;
	this->m_progress_prevStats = this->stats;
}

void ClusterDiffComparer::onDirty(LCN lcn, void* data)
{
}

void ClusterDiffComparer::onDirtySpan(LCN lcnFirst, LCN len, void* data)
{
	if (this->printDiff)
		printClusterSpan(lcnFirst, len, this->printClustersAsSpans);
}


ClusterDiffWriter::ClusterDiffWriter(Volume& src, Volume& dest)
	: ClusterDiffComparer(src, dest), writer(dest.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*src.volumeData().BytesPerCluster)
{
}

void ClusterDiffWriter::process(CandidateClusterMap& srcDiff)
{
	ClusterDiffComparer::process(srcDiff);

	//Wait until the writer has finished writing
	while (writer.try_pop_front(nullptr, nullptr)) {}
}

void ClusterDiffWriter::onDirtySpan(LCN lcnFirst, LCN len, void* data)
{
	ClusterDiffComparer::onDirtySpan(lcnFirst, len, data);
	//Block until we have a free slot and push the write request
	writer.push_back(lcnFirst*BytesPerCluster, (uint32_t)(len*BytesPerCluster), data);
}


ClusterCopier::ClusterCopier(Volume& src, Volume& dest)
	: ClusterProcessor(src, dest),
	srcReader(src.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*src.volumeData().BytesPerCluster),
	writer(dest.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*src.volumeData().BytesPerCluster)
{
}

void ClusterCopier::process(CandidateClusterMap& srcDiff)
{
	LCN lastProgress = 0;
	this->onProgress(0);

	auto slicedRuns = slice_runs(BitmapSpans(&srcDiff), ASYNC_BATCH_LEN);
	auto sliceIt = slicedRuns.begin();

	while (true) {
		//Part 1. Push read commands into the queue
		while (sliceIt != slicedRuns.end()) {
			if (!srcReader.try_push_back(sliceIt->offset*BytesPerCluster, (uint32_t)(sliceIt->length*BytesPerCluster)))
				break;
			++sliceIt;
		}

		//Part 2. Extract next element.
		//Easier to pop exactly one read a cycle and push one new. If multiple are done, next few cycles will just be fast.
		uint32_t bytesRead = 0, bytesRead2 = 0;
		uint64_t offset = 0, offset2 = 0;
		auto srcPtr = srcReader.finalize_front(&bytesRead, &offset);
		if (srcPtr == nullptr) {
			assert(sliceIt == slicedRuns.end());
			assert(srcReader.pending_count == 0);
			break;
		}
		assert(offset % BytesPerCluster == 0);
		assert(bytesRead % BytesPerCluster == 0);

		stats.spansChecked++;

		LCN lcn = offset / BytesPerCluster;
		LCN lastClean = lcn - 1;

		//Block until we have a free slot and push the write request
		writer.push_back(offset, bytesRead, srcPtr);

		srcReader.pop_front();

		if (stats.clustersChecked - lastProgress > 50000) {
			lastProgress = stats.clustersChecked;
			this->onProgress((offset2 + bytesRead2) / BytesPerCluster);
		}
	}

	//Wait until the writer has finished writing
	while (writer.try_pop_front(nullptr, nullptr)) {}
}

void ClusterCopier::onProgress(LCN lcn)
{
	//TODO
}