#pragma once
#include "clusterdiff.h"
#include "util.h"
#include "ntfsmft.h"



ClusterProcessor::ClusterProcessor(Volume& src, Volume& dest)
	: src(src), dest(dest)
{
	BytesPerCluster = this->src.volumeData().BytesPerCluster;
	assert(BytesPerCluster > 0);
}

ClusterProcessor::~ClusterProcessor()
{
}

void ClusterProcessor::init()
{
	//Override to initialize supplementary objects once this one has been configured by the user.
}

void ClusterProcessor::process(CandidateClusterMap& srcSelection)
{
	this->init();

	LCN lastProgress = 0;
	if (this->progressCallback) {
		this->progressCallback->setMax(srcSelection.bitCount());
		this->progressCallback->progress(0, true);
	}
	this->onProgress(0);
}

void ClusterProcessor::onProgress(LCN lcn)
{
}



void ClusterDiffComparer::init()
{
	inherited::init();
	srcReader.reset(new AsyncFileReader(src.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*BytesPerCluster));
	destReader.reset(new AsyncFileReader(dest.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*BytesPerCluster));
}

void ClusterDiffComparer::process(CandidateClusterMap& srcSelection)
{
	inherited::process(srcSelection);

	if (this->diffMap)
		this->diffMap->resize(srcSelection.size);

	auto slicedRuns = slice_runs(BitmapSpans(&srcSelection), ASYNC_BATCH_LEN);
	auto sliceIt = slicedRuns.begin();

	if (this->verbose)
		std::cerr << "Selection bit count: " << srcSelection.bitCount() << std::endl;

	while (true) {
		//Part 1. Push read commands into the queue
		while (sliceIt != slicedRuns.end()) {
			if (!srcReader->try_push_back(sliceIt->offset*BytesPerCluster, (uint32_t)(sliceIt->length*BytesPerCluster)))
				break;
			assert(destReader->try_push_back(sliceIt->offset*BytesPerCluster, (uint32_t)(sliceIt->length*BytesPerCluster)));
			++sliceIt;
		}

		//Part 2. Extract next element.
		//Easier to pop exactly one read a cycle and push one new. If multiple are done, next few cycles will just be fast.
		uint32_t bytesRead = 0, bytesRead2 = 0;
		uint64_t offset = 0, offset2 = 0;
		auto srcPtr = srcReader->finalize_front(&bytesRead, &offset);
		if (srcPtr == nullptr) {
			assert(sliceIt == slicedRuns.end());
			assert(srcReader->pending_count == 0);
			assert(destReader->pending_count == 0);
			break;
		}
		assert(offset % BytesPerCluster == 0);
		assert(bytesRead % BytesPerCluster == 0);
		stats.bytesRead += bytesRead;
		auto destPtr = destReader->finalize_front(&bytesRead2, &offset2);
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
			srcPtr += BytesPerCluster;
			destPtr += BytesPerCluster;
			stats.clustersChecked++;
		}
		//We do not support "dirty spans" across chunk boundaries so finalize one if we have one
		if (lastClean != lcn - 1)
			this->onDirtySpan(lastClean + 1, lcn - lastClean - 1, srcPtr - (lcn - lastClean - 1)*src.volumeData().BytesPerCluster);

		srcReader->pop_front();
		destReader->pop_front();

		this->doProgress(stats.clustersChecked);
	}

	if (this->verbose) {
		std::cerr << "Clusters checked: " << stats.clustersChecked << std::endl;
		std::cerr << "Bytes read: " << stats.bytesRead << std::endl;
		std::cerr << "Dirty span totals: " << stats.dirtySpanTotals << std::endl;
		if (this->diffMap)
			std::cerr << "Diff set bits: " << diffMap->bitCount() << std::endl;
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

	if (printProgressDetails)
		qDebug() << "Clusters: " << stats.clustersChecked << ", runs: " << thisRunCount << ", t=" << t2 << ", cpm=" << (double)thisClusterCount / t2 << ", rpm=" << (double)thisRunCount / t2 << std::endl;
	this->m_progress_prevStats = this->stats;
}

void ClusterDiffComparer::onDirty(LCN lcn, void* data)
{
}

void ClusterDiffComparer::onDirtySpan(LCN lcnFirst, LCN len, void* data)
{
	stats.dirtySpanTotals += len;
	if (diffMap)
		diffMap->set(ClusterRun{ lcnFirst, len });
}


void ClusterDiffWriter::init()
{
	inherited::init();
	writer.reset(new AsyncFileWriter(dest.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*src.volumeData().BytesPerCluster));
}

void ClusterDiffWriter::process(CandidateClusterMap& srcSelection)
{
	inherited::process(srcSelection);

	//Wait until the writer has finished writing
	while (writer->try_pop_front(nullptr, nullptr)) {}
}

void ClusterDiffWriter::onDirtySpan(LCN lcnFirst, LCN len, void* data)
{
	ClusterDiffComparer::onDirtySpan(lcnFirst, len, data);
	//Block until we have a free slot and push the write request
	writer->push_back(lcnFirst*BytesPerCluster, (uint32_t)(len*BytesPerCluster), data);
}


void ClusterCopier::init()
{
	inherited::init();
	srcReader.reset(new AsyncFileReader(src.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*src.volumeData().BytesPerCluster));
	writer.reset(new AsyncFileWriter(dest.h(), ASYNC_QUEUE_DEPTH, ASYNC_BATCH_LEN*src.volumeData().BytesPerCluster));
}

void ClusterCopier::process(CandidateClusterMap& srcSelection)
{
	inherited::process(srcSelection);

	auto slicedRuns = slice_runs(BitmapSpans(&srcSelection), ASYNC_BATCH_LEN);
	auto sliceIt = slicedRuns.begin();

	while (true) {
		//Part 1. Push read commands into the queue
		while (sliceIt != slicedRuns.end()) {
			if (!srcReader->try_push_back(sliceIt->offset*BytesPerCluster, (uint32_t)(sliceIt->length*BytesPerCluster)))
				break;
			++sliceIt;
		}

		//Part 2. Extract next element.
		//Easier to pop exactly one read a cycle and push one new. If multiple are done, next few cycles will just be fast.
		uint32_t bytesRead = 0, bytesRead2 = 0;
		uint64_t offset = 0, offset2 = 0;
		auto srcPtr = srcReader->finalize_front(&bytesRead, &offset);
		if (srcPtr == nullptr) {
			assert(sliceIt == slicedRuns.end());
			assert(srcReader->pending_count == 0);
			break;
		}
		assert(offset % BytesPerCluster == 0);
		assert(bytesRead % BytesPerCluster == 0);

		stats.spansChecked++;
		stats.clustersChecked += (bytesRead % BytesPerCluster);

		LCN lcn = offset / BytesPerCluster;
		LCN lastClean = lcn - 1;

		//Block until we have a free slot and push the write request
		writer->push_back(offset, bytesRead, srcPtr);

		srcReader->pop_front();

		this->doProgress(stats.clustersChecked);
	}

	//Wait until the writer has finished writing
	while (writer->try_pop_front(nullptr, nullptr)) {}
}
