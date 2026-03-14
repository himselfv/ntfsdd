#pragma once
#include "vssutil.h"
#include <windows.h>
#include <iostream>
#include "util.h"

VssShadowCopy::VssShadowCopy()
{
	HRCHECK(CoInitialize(NULL));

	//Initialize COM security for VSS
	//Normally should be done application-wide
	HRCHECK(CoInitializeSecurity(
		NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
		RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_NONE, NULL
	));
}

VssShadowCopy::~VssShadowCopy()
{
	release();
	CoUninitialize();
}

void VssShadowCopy::create(const std::wstring& volumePath)
{
	HRCHECK(CreateVssBackupComponents(&m_pBackup));

	//Initialize for a standard backup (non-persistent context)
	HRCHECK(m_pBackup->InitializeForBackup());

	HRCHECK(m_pBackup->SetContext(
		(this->m_snapshotMode==VssSnapshotMode::WriterBackup) ?
		VSS_CTX_BACKUP :
		VSS_CTX_FILE_SHARE_BACKUP
	));
	/*
	VSS_CTX_BACKUP
		The standard backup context. Specifies an auto-release, nonpersistent shadow copy in which writers are involved in the creation.
	VSS_CTX_FILE_SHARE_BACKUP
		Specifies an auto-release, nonpersistent shadow copy created without writer involvement.

	Different requirements as to what functions to call:
	https://learn.microsoft.com/en-us/windows/win32/vss/shadow-copy-creation-details
	*/


	IVssAsync* pAsync = nullptr;

	if (this->m_snapshotMode == VssSnapshotMode::WriterBackup) {
		//What we pass here is passed to writers and tells them how well to prepare
		HRCHECK(m_pBackup->SetBackupState(
			FALSE,			//Individual backup component selection?
			FALSE,			//Backup bootable system state?
			VSS_BT_FULL,	//Type: Full, Incremental, etc
			FALSE			//Partial file support?
		));

		HRCHECK(m_pBackup->GatherWriterMetadata(&pAsync));
		HRCHECK(pAsync->Wait());
		pAsync->Release();
	}

	//Start the Snapshot Set
	VSS_ID snapshotSetId;
	HRCHECK(m_pBackup->StartSnapshotSet(&snapshotSetId));

	//Add volume to the set (use a wide string for the volume path)
	std::wstring volumePathCopy = volumePath;
	assert(!volumePathCopy.empty());
	if (volumePathCopy.back() != '\\')
		volumePathCopy += '\\'; //Must end with \\ 
	HRCHECK(m_pBackup->AddToSnapshotSet(const_cast<wchar_t*>(volumePathCopy.c_str()), GUID_NULL, &m_snapshotId));
	this->m_haveSnapshotId = true;


	if (this->m_snapshotMode == VssSnapshotMode::WriterBackup) {
		//Prepare and Create the Snapshot
		HRCHECK(m_pBackup->PrepareForBackup(&pAsync));
		HRCHECK(pAsync->Wait());
		pAsync->Release();
	}

	HRCHECK(m_pBackup->DoSnapshotSet(&pAsync));
	HRCHECK(pAsync->Wait());
	pAsync->Release();

	//Get the Device Object Path (for block-level access)
	HRCHECK(m_pBackup->GetSnapshotProperties(m_snapshotId, &m_prop));
	this->m_haveSnapshotProp = true;

	//m_prop can be used to extract the snapshot path and its properties.
}

void VssShadowCopy::release() {
	if (m_haveSnapshotProp) {
		VssFreeSnapshotProperties(&m_prop);
		this->m_haveSnapshotProp = false;
	}

	//Explicit Deletion (Optional for non-persistent, required for persistent)
	if (this->m_haveSnapshotId) {
		LONG deletedCount = 0;
		VSS_ID nonDeletedId;
		m_pBackup->DeleteSnapshots(this->m_snapshotId, VSS_OBJECT_SNAPSHOT, TRUE, &deletedCount, &nonDeletedId);
		this->m_haveSnapshotId = false;
	}

	m_pBackup->Release();
	m_pBackup = nullptr;
}
