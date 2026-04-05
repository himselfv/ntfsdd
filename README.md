# NTFS Disk Destroyer
Compares and updates NTFS volume clones in a dangerously efficient fashion.

## Description
Given a source NTFS volume and a target clone of it, uses NTFS $Bitmap or compares the MFTs to generate a list of potentially changed clusters.
Compares these clusters between the source and the destination and copies the changes to the destination.

AKA in some apps: Rapid Delta Clone. (Only with read-compare-write!)

The goals:

* to minimize the number of clusters to be checked by using progressively more sophisticated ways of selecting them
* to minimize writes, preserving SSDs.

Can be used for:

* Cloning/imaging: ntfsdd copy all
* Volume comparison: ntfsdd compare all/compare bitmap
* Smart updates: ntfsdd copy mft (for HDDs)/ntfsdd rcw mft (for SSDs).
* Accidental HDD wipes.

In general you have to run this as Administrator. But start without that and see if your goals are satisfied.



### The basics

The tool works in two steps:

1. Select the clusters for processing.
2. Process them.

Selecting the clusters (``--select``):

**all**
: All clusters on the source volume.

**bitmap**
: Clusters marked as used in $Bitmap on the source volume.

**mft**:
: Clusters referenced in any of the MFT segments (~= files/dirs) which are different between the source and the destination. \
  Basically, all clusters referenced by the files that could have changed.

MFT is the most narrow and it's recommended unless it's not working for you somehow. All/Bitmap are useful for initial cloning.

The action to perform (``--action``):

**list**
: List the selected clusters.

**compare**
: Compare the selected clusters between the source and the destination. List the clusters with differences.

**copy**
: Copy *all* the selected clusters from the source to the destination. This is typically how clone resync works on HDDs. This is more efficient for the HDDs.

**rcw**
: Compare the selected clusters between the source and the destination and copy the changed clusters to the destination. This is better for the SSDs because it avoids writes unless neccessary.


#### Copy or RCW?
Most existing cloning tools and software/firmware mirror raids do the equivalent of **copy** (I checked), either for the whole drive (raids) or for the partition being cloned. This is more efficient for HDDs. On HDDs reads and writes have similar time costs so it's faster to read+write instead of read+read+compare+write. Writes on HDDs are more or less free and may even help by reinforcing the stored data.

SSDs can only sustain a limited number of writes through their lifetime. That number is not that high. You'll thrash the SSD fast by rewriting it often. Reads on SSDs are more or less free and usually faster than writes, so RCW makes much more sense for SSDs.



### Examples:

```
ntfsdd copy all --source SOURCE --dest DEST
```
Blindly clones SOURCE (volume, file, vss) to DEST (volume, file).

```
ntfsdd copy bitmap --source SOURCE --dest DEST
```
Same but copies only the clusters used by the filesystem.

```
ntfsdd copy mft --source SOURCE --dest DEST
```
Compares the MFTs and copies potentially changed clusters blindly (useful for HDDs).

```
ntfsdd rvw mft --source SOURCE --dest DEST
```
Compares the MFTs, then compares all the potentially changed clusters and copies the changes (useful for SSDs).

```
ntfsdd list all/bitmap/mft
```
Prints the cluster numbers matching given selection criteria.

```
ntfsdd compare all/bitmap/mft
```
Compares the clusters matching the selection criteria and prints the cluster numbers for the changed clusters.



## Unmounting
As a safety measure the tool requires your destination volume to not have any mount points. This is to prevent accidental wiping of unintended volumes. It is advised to NOT dismount your destination automatically via script, as that negates the safety. Instead,

1. For a one-shot copy, dismount the volume manually. Make sure you have dismounted the correct DriveLetter.
2. If you're doing regular scripted updates, keep the target free of mount points permanently.

You're free to do otherwise of course. ``--unsafe-allow-mounted`` disables this check.


## Cloning
The tool can be used for cloning. "``copy all``" will perform a forensic clone (all clusters) and "``copy bitmap``" will only copy the clusters used (recommended). In both cases, and especially after "``copy all``", ``defrag /Retrim`` is advised after clone.

When doing a full clone as a safety measure you have to pass ``--overwrite``. No checks against the destination volume layout and MFT will then be made.


## Imaging and restoration
You can pass files both as source and as destination. This way you can image volumes, update and restore those images. The file has to exist:
```
type nul >filename.img
```

Note that "```copy all```" will produce a file equivalent to the source in size. "```copy bitmap```" might produce a smaller file as final empty unused sectors will not be copied. This may confuse some tools, idk. You'll also likely not be able to "```copy all```" from that file and will have to restore it with "```copy bitmap```". \
To prevent this, pre-fill the file with zeroes.

#### HOW TO CREATE SMALL NTFS VOLUMES IN FILES W/VARIOUS PARAMS:
* Disk management MMC.
* Create virtual disk (VHD). VHDs are stored almost as is, only with additional footer. But we need partitions, not the entire disk.
* Create NTFS partitions w/different cluster sizes.
* ```fsutil volume list```
* ```dd if=\\.\{GUID} of=volumeName.img```
* Make sure it's \\.\ and not \\?\.


## VSS
It is recommended to use VSS snapshots as the source instead of the live volume:
```
--shadow
```
Auto-creates a temporary no-writer-cooperation shadow of the source. To enable writer cooperation (slower, flakier, but better consistency):
```
--shadow --shadow-writers
```

To create/delete a permanent shadow copy manually:
```
wmic shadowcopy call create Volume="C:\"
vssadmin list shadows
vssadmin delete shadows [...your shadow ids]
```



## Trim
This tool does NOT trim sectors that became free/had been free on full clone. The reasons for this are:

1. Trim is DANGEROUS and FAST. On modern SSDs trimmed data cannot be restored. One wrongly passed param and a second later your entire partition is irreversibly wiped. Do you want us messing with that? No, you don't.
2. Don't do what's already done well:
```
defrag /Retrim DriveLetter:
```
Trims all empty sectors on the target volume. Run after a full clone or from time to time after updates. I'd advise not running retrim after every update as: 1. The changes are likely limited and it's not that urgent. 2. This raises your chances to notice an update going awry and recover data before its trimmed. Just in case!

With regular and limited updates I think you can even retrim only occasionally, manually. If a non-trimmed "empty" sector is reused, for the SSD it simply looks like you're overwriting your own data. It's suboptimal (the SSD could rotate the backing sector with trim) but it's a normal work mode. You're doing this to files all the time when you change their contents.



## Ignoring/enforcing files
There's minimal ability to ignore files. Read carefully what it does! It's not meant as a full filename-based filtering. This is just to skip ```hiberfil.sys``` and ```pagefile.sys``` and so on.

These options only work with MFT-based selection.

* ``--exclude-segment``, ``--exclude-file`` excludes this single file (all of its segments) OR DIR, ALONE.

* ``--exclude-subtree``, ``--exclude-path`` excludes this file (all of its segments) or dir, with all its contents recursively.

* ``--include-*`` forcefully marks files as dirty. Same zoo of versions as with ``--exclude``


What this ``--exclude`` does:

* MFT segments will be copied anyway. MFTs will always match perfectly. The cluster usage on the volumes must always match!

* The data clusters for these segments will not copied.

The file on the destination will become of its current size, referencing its current clusters, but those clusters will contain garbage left from their previous tenants.


WARNING: Security risk. Data from other files will leak into these. Might not matter if your destination is accessible only to the admin anyway, but think it through.

* Retrim regularly to lower the chance, but it's never zero.

* Manually ```del``` the file on the destination after cloning if you care.


In the same, but safer, way you can force the file clusters to ALWAYS be selected with ``--include-``. Internally this is used to handle driver magic folders such as System Volume Information. This has absolutely NO side effects except for more data to rcw/copy each time.


**Q**: Can I pass file names and paths?\
**A**: Yeah, with limitations. Symlinks not suppored, pass real paths. A file referenced by ANY of its paths (hardlinks) will be skipped/dirtied. Doesn't matter if there are no rules for the other hardlinks.

**Q**: Why not delete ignored files?\
**A**: This requires editing the destination MFT, $Bitmap, the transaction log and getting involved in the NTFS internals much deeper than required for our simple cloning. Just delete the file with ```del``` later.

**Q**: Why don't you at least trim the clusters you're skipping, to prevent data leaks?\
**A**: See the Trim section. Best to stay away from trim.

**Q**: What files can I skip?\
**A**: ```hiberfil.sys``` and ```pagefile.sys```. When you boot from the clone, the system may not boot the first time around due to garbage in ``hiberfil.sys``. It should delete it on the second boot. If it fails to do so, help it.

**Q**: Why not skip ``$BadClus``?\
**A**: True: Copying the map of physical bad clusters elsewhere rarely makes sense. But keeping the old map also makes no sense! We're duplicating the source clusters precisely. What if those clusters are marked bad on the destination? Cluster-by-cluster copy just doesn't mesh with bad cluster maps. Thankfully, these days bad clusters are managed by the hardware and $BadClus in NTFS is unused. So we copy it like any other file.\
Honestly, we could skip it. Skip it at your peril. Anyway, I don't think it should even change, so why bother.

**Q**: Why is this only working with MFT selection modes?\
**A**: Not much sense in it in other modes. Bitmap already includes all there is to include. Bitmap+Exclusion might make some sense if you want "100% guaranteed coverage minus manually excluded paths" but it's a strange thing to do and currently not implemented.




## NTFS version support
I wrote this for my own uses, so I enabled NTFS versions on which I could have checked this. If you need other NTFS versions supported, send me something.


## Building
Compiles with MSVC2015/C++14.
All requirements are in Requirements.example.props, rename and provide local paths.

There are some tests, write more.



## In-depth discussion

### Is the Bitmap selection system safe?
Bitmap selection is slow but should be 100% safe. We're comparing ALL clusters that the source file system says are in use. You really have to break NTFS fundamentally for this to be less than a full clone.



### What the MFT comparison system does, precisely?
It reads the volume index (the $MFT) and compares the same file header records ("segments") on both volumes:

* The MFT itself is always "selected". This means ALL the segments ("file headers") will always be verified/copied.

* Segments not IN_USE on the source are skipped (see "Deletions" below)

* Identical segments are skipped

* If segments differ, all the external clusters referenced in the source segment are "selected".

* For multi-segment files, the decision is made for the entire set of file segments together.



### Is the MFT comparison system safe?
Will it really catch everything? Can't the data change without the MFT entry changing?
https://www.boku.ru/2026/04/02/ntfs-can-a-file-change-without-its-mft-record-also-changing/

MFT comparison is a bit of a gamble. For normal files and normal cases MFT checks are safe with very solid safety margins. There does not seem to be a mode where you can change anything substantial without the corresponding MFT entries changing with a guarantee.

But there are exceptions. Driver magic. System files, cached DUPLICATED_INFORMATION in index entries. And what these exceptions tell us is that there could be *more* exceptions. We handle all the exceptions we know about and it seems to cover everything, but there's no documented promise anywhere that says "No other files will receive driver magic".

What we rely on instead is:
1. We cover most of the major exceptions.
2. We leave the target volume in a consistent state.
3. Any files or clusters that slip through the cracks are mishandled predictably. Their contents will be garbage/old versions, but the volume will work.

To protect against this long term, do a ```compare --select antimft --exit-code``` scan from time to time to detect any slippages. Hopefully there should be none.

Another way to look at this system is: When you're running rclone, it compares every file by looking at its size and modification date to decide whether it needs copying. For normal files, we're doing much, much better than that. Of course, rclone does not cover system files at all. That's where the exceptions are.


### Eventual consistency
NTFS delays writing out some changes to the volume, from milliseconds for normal data to hours for LastAccessTime/LastModificationTime according to some reports. Is this not a problem for our algorithms? How consistent will be a clone made from a live system?

Here's what you should understand:
1. You can pull the power plug at any moment. Anything not yet written to the disk will be lost. What you get is called *crash consistency*. NTFS is designed to be crash consistent. On the next boot it will perform some automatic transparent recovery actions and the volume will continue working.
2. Crash consistency only means that the volume itself remains healthy. The data that has not yet been written to disk at the moment of the crash will be lost. Some apps, like NTFS, are prepared for this and will continue working. Others may be so unprepared that their data becomes broken.
3. If you copy a raw LIVE volume WITHOUT VSS, you get INCONSISTENCY. As you're copying the sectors, other sectors change and so your copy will be self-contradictory. Thankfully, the OS will usually not let you do that.
4. LOCKED volume should be NTFS-crash-consistent. If you managed to lock the volume, NTFS will not touch it until you release it. You get the same guarantees as with pulling the power plug (maybe even a bit better). The running apps may still be in the process of updating their files, and the copy you make will be inconsistent from their point of view. But it will work.
5. VSS snapshot (--shadow) instantly creates a "copy" of the volume which is crash consistent. It's the same as locking, only you can use the snapshot long-term. It's your individual plaything, while the world goes on, modifying the real volume. You won't see those modifications. Still, it's only crash consistent.
6. VSS with writers (--shadow --writers) additionally coordinates many system services by asking them to prepare for backup: finish any operations and write any cached data. This lowers the chance that the snapshot will have broken data for those services. This does not cover all the apps in the system, and the ones it covers are normally pretty resilient anyway.

The recommended way of running this on live systems is ``--shadow`` or ``--shadow --writers``. Therefore any time you're updating the clone you get crash consistency: more or less no worse than what you'd get if you pulled the plug at that moment. This is pretty much normal for live backups. Try to run updates at a time when there's minimal system activity to minimize the number of apps that can be affected.

Crash consistency + VSS snapshot guarantee that the NTFS volume state will be low-level consistent. $Bitmap will correctly identify clusters used, MFT segments will correctly account for all of them. We're verifying that when we're doing the clone.

VSS crash consistency might not guarantee that some cached data, such as LastModificationTime, is written out. It's supposed to do *something like that* (prepare for backup), but there are no literal lists of high-level guarantees about particulars of caching. So what this tool cares about is *eventual consistency*. If for some reason some less consequential changes to the MFT are not yet written out at the time of taking the snapshot, yes, with ``--select mft`` we will miss the changes in data those reflect. But we will catch them next time!

This is not a problem because the precise moment when the clone runs is arbitrary anyway: it could have happened a moment earlier when those changes to the data had not yet been made, and we would have missed them too until next time. So even though the changes *had* been made and we have not seen them *yet*, what matters is every change gets caught *eventually*.

Creating a shadow is supposed to trigger flushing all possible caches, including writing out any cached modtimes and segments (which can otherwise be cached in some cases for up to, sources tell, hours). There's no real guarantee this will happen.
HOWEVER. What we care about is *eventual* consistency. Even if you miss some changes today, if you do the sync again you'll catch it later. The goal is to have no *permanently undetectable* changes. And to have no *inconsistent FS*. We guarantee FS consistency by force-comparing the MFT (which gives us power-down crash consistency, and maybe more, if VSS+writers work as intended).
With FS-consistency, file-inconsistency, if any, will look like recently changed files having 1. Their old content (if their cached segment changes have not been written out) - their state should be consistent with the state of the rest of the volume. If any inconsistencies are present, those will get resolved in the same way as when you reboot after a sudden power-down, by using the NTFS log journal. 2. Their old content (if the content had changed, no segment changes had been needed, no size change had happened, and the lastmodtime and LSN/USN/stuff had not yet been written down to disk). 3. Purely theoretically, maybe in some cases, garbage (can't invent examples right now, but it's one of the theoretically stable states of the volume where some updates had been skipped).


### What about deletions?
The algorithm only compares segments which are IN_USE on the source. What about the segments that were IN_USE but are now not? Do we not need to free their segments?

No! We're selecting clusters to verify/copy. *Which* clusters are in use depends only on the $MFT itself and on $Bitmap, not the content of those clusters. We *always* sync the entire $MFT and $Bitmap, and so we always copy the entire actual picture of what clusters are now not in use. We do not need to copy those clusters.

Do we not need to update the segment on the destination? Yes we do, and we will, even without selecting its clusters: Again: We *always* sync the entire $MFT and $Bitmap.

