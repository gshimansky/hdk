/**
 * @file	FileMgr.cpp
 * @author	Steven Stewart <steve@map-d.com>
 *
 * Implementation file for the file manager.
 *
 * @see FileMgr.h
 */
#include <iostream>
#include <cassert>
#include <cstdio>
#include <string>
#include <cassert>
#include <exception>
#include "FileMgr.h"

using std::vector;

namespace File_Namespace {

FileInfo::FileInfo(int fileId, FILE *f, mapd_size_t blockSize, mapd_size_t nblocks)
     : fileId(fileId), f(f), blockSize(blockSize), nblocks(nblocks) // STEVE: careful here - assignment to same variable name fails on some compilers even though it should work according to C++ standard
{
    // initialize blocks and free block list
    for (mapd_size_t i = 0; i < nblocks; ++i) {
        blocks.push_back(new Block(fileId, i * blockSize));
        freeBlocks.insert(i);
    }
}

FileInfo::~FileInfo() {
	// free memory used by Block objects
	for (int i = 0; i < blocks.size(); ++i)
		delete blocks[i];

	// close file, if applicable
    if (f && close(f) != MAPD_SUCCESS)
        fprintf(stderr, "[%s:%d] Error closing file %d.\n", __func__, __LINE__, fileId);
}

void FileInfo::print(bool blockSummary) {
    printf("File #%d", fileId);
    printf(" size = %lu", size());
    printf(" used = %lu", used());
    printf(" free = %lu", available());
    printf("\n");
    if (!blockSummary)
        return;
    
    for (int i = 0; i < blocks.size(); ++i) {
    	// @todo block summary
    }
}

FileMgr::FileMgr(const std::string &basePath): basePath_(basePath) {
	nextFileId_ = 0;
}

FileMgr::~FileMgr() {
	for (int i = 0; i < files_.size(); ++i)
		delete files_[i];

	// free memory allocated for Chunk objects
	for(auto it = chunkIndex_.begin(); it != chunkIndex_.end(); ++it) {
		Chunk &v = (*it).second;

		// free memory allocated for MultiBlock objects
		for (auto it2 = v.begin(); it2 != v.end(); ++it2)
			delete *it2;
	}
}

FileInfo* FileMgr::createFile(const mapd_size_t blockSize, const mapd_size_t nblocks) {
	if (blockSize < 1 || nblocks < 1)
		return NULL;

    // create the new file
    FILE *f = NULL;
    f = create(nextFileId_, blockSize, nblocks, NULL);

    // check for error
    if (!f) {
    	fprintf(stderr, "[%s:%d] Error: unable to create file.\n", __func__, __LINE__);
    	return NULL;
    }

	// update file manager data structures
    int fileId = nextFileId_++;
	FileInfo *fInfo = NULL;
	try {
		fInfo = new FileInfo(fileId, f, blockSize, nblocks);
		files_.push_back(fInfo);
		fileIndex_.insert(std::pair<mapd_size_t, int>(blockSize, fileId));
	}
	catch (const std::bad_alloc& e) {
		std::cout << "Bad allocation exception encountered: " << e.what() << std::endl;
		return NULL;
	}
	catch (const std::exception& e) {
		std::cout << "Exception encountered: " << e.what() << std::endl;
		if (!fInfo) delete fInfo;
		return NULL;
	}
	assert(files_.back() == fInfo);
	return fInfo;
}

FileInfo* FileMgr::getFile(const int fileId) {
    if (fileId < 0 || fileId > files_.size())
    	return NULL;
    return files_[fileId];
}

mapd_err_t FileMgr::deleteFile(const int fileId, const bool destroy) {

    // confirm the file exists and obtain pointer
    FileInfo *fInfo = getFile(fileId);
    if (!fInfo)
    	return MAPD_FAILURE;

    // remove the file from the fileIndex_
    BlockSizeFileMMap::iterator it = fileIndex_.lower_bound(fInfo->blockSize);
    for (it = fileIndex_.begin(); it != fileIndex_.end(); ++it) {
    	if (it->second == fileId)
    		break;
    }
    if (it != fileIndex_.end())
    	fileIndex_.erase(it);

    // remove the file from the vector of files_
    files_.erase(files_.begin() + fileId);

    // @todo error-checking if erase fails?
    // @todo physically delete the file on disk
    return MAPD_SUCCESS;
}

// Gil wrote this. Send any complaints to Map-D's Zurich office.
mapd_err_t FileMgr::readFile(FileInfo &fInfo, mapd_size_t offset, mapd_size_t n, mapd_addr_t buf) {
    mapd_err_t err = MAPD_SUCCESS;
    size_t result = read(fInfo.f, offset, n, buf, &err);
    if (result != n) 
        err = MAPD_FAILURE;
    // @todo proper error handling
    return err;
}

mapd_err_t FileMgr::writeFile(FileInfo &fInfo, mapd_size_t offset, mapd_size_t n, mapd_addr_t src) {
    //size_t write(FILE *f, mapd_addr_t offset, mapd_size_t n, mapd_addr_t buf, mapd_err_t *err);
    mapd_err_t err = MAPD_SUCCESS;
    size_t result = write(fInfo.f, offset, n, src, &err);
    return err;
}


Block* FileMgr::getBlock(const int fileId, mapd_size_t blockNum) {
	FileInfo *fInfo = FileMgr::getFile(fileId);
    return !fInfo ? NULL : getBlock(*fInfo, blockNum);
}

Block* FileMgr::getBlock(FileInfo &fInfo, mapd_size_t blockNum) {
    assert(blockNum < fInfo.blocks.size() && fInfo.blocks[blockNum]);
    return fInfo.blocks[blockNum];
}

mapd_err_t FileMgr::putBlock(int fileId, mapd_size_t blockNum, mapd_addr_t buf) {
	FileInfo *fInfo;
	return ((fInfo = getFile(fileId)) == NULL) ? MAPD_FAILURE : putBlock(*fInfo, blockNum, buf);
}

mapd_err_t FileMgr::putBlock(FileInfo &fInfo, mapd_size_t blockNum, mapd_addr_t buf) {
    // assert buf

    // open the file if it is not open already
    if (openFile(fInfo) != MAPD_SUCCESS) {
        printf("openfile error");
        return MAPD_FAILURE;
    }
    // write the block to the file
    mapd_err_t err;
    size_t wrote = writeBlock(fInfo.f, fInfo.blockSize, blockNum, buf, &err);
    assert(wrote == fInfo.blockSize);

	return err;
}

mapd_err_t FileMgr::clearBlock(const int fileId, mapd_size_t blockNum) {
	FileInfo *fInfo = FileMgr::getFile(fileId);
    return !fInfo ? MAPD_FAILURE : clearBlock(*fInfo, blockNum);
}

mapd_err_t FileMgr::clearBlock(FileInfo &fInfo, mapd_size_t blockNum) {
    Block *b = getBlock(fInfo, blockNum);
    if (b) {
    	b->end = b->begin;
    	return MAPD_SUCCESS;
    }
    return MAPD_FAILURE;
}

mapd_err_t FileMgr::freeBlock(const int fileId, mapd_size_t blockNum) {
    FileInfo *fInfo = getFile(fileId);
    return !fInfo ? MAPD_FAILURE : freeBlock(*fInfo, blockNum);
}

mapd_err_t FileMgr::freeBlock(FileInfo &fInfo, mapd_size_t blockNum) {
    mapd_err_t err = MAPD_SUCCESS;
    err = clearBlock(fInfo, blockNum);
    if (err == MAPD_SUCCESS)
    	fInfo.freeBlocks.insert(blockNum); // @todo error-checking on insert() ?
    return err;
}

Chunk* FileMgr::getChunkRef(const ChunkKey &key) {
    auto it = chunkIndex_.find(key);
    return it != chunkIndex_.end() ? &it->second : NULL;
}

Chunk* FileMgr::getChunk(const ChunkKey &key, mapd_addr_t buf) {
    assert(buf);
    
    // find chunk
    auto it = chunkIndex_.find(key);
    if (it == chunkIndex_.end()) // chunk doesn't exist
        return NULL;

    // copy contents of chunk to buf
    Chunk &c = it->second;
    for (int i = 0; i < c.size(); ++i) {

        // get most recent address of current block
        Block &blk = c[i]->current();

        // obtain a reference to the file of the block address
        int fileId = blk.fileId;
        FileInfo *fInfo = getFile(fileId);
        if (!fInfo)
            return NULL;
        
        // open the file if it is not open already
        if (openFile(*fInfo) != MAPD_SUCCESS)
            return NULL;
        
        // read block from file into buf
        mapd_err_t err;
        read(fInfo->f, i * c[i]->blockSize, c[i]->blockSize, buf, &err);
        if (err != MAPD_SUCCESS)
        	return NULL;
    }
    return &c;
}

mapd_err_t FileMgr::getChunkSize(const ChunkKey &key, int *nblocks, mapd_size_t *size) {
    assert(size || nblocks); // at least one of these should be not NULL
    mapd_err_t err = MAPD_SUCCESS;
    
    ChunkKeyToChunkMap::iterator iter = chunkIndex_.find(key);
    if (iter == chunkIndex_.end()) {
        // not found
        err = MAPD_ERR_CHUNK_NOT_FOUND; // chunk doesn't exist
        return err;
    }
    
    // found
    Chunk &c = iter->second;

    // check if chunk has no blocks
    if (nblocks) {
        *nblocks = c.size();
        if (*nblocks < 1) {
            if (size) *size = 0;
            return err;
        }
    }
    if (size) { // Compute size based on block sizes
        *size = 0;
        for (int i = 0; i < c.size(); ++i)
            *size += c[i]->blockSize;
    }

    return err;
}

mapd_err_t FileMgr::getChunkActualSize(const ChunkKey &key, mapd_size_t *size) {
    assert(size);
    mapd_err_t err = MAPD_SUCCESS;
    
    ChunkKeyToChunkMap::iterator iter = chunkIndex_.find(key);
    if (iter == chunkIndex_.end()) // not found
        return MAPD_ERR_CHUNK_NOT_FOUND;
    
    // Compute size based on actual bytes used in block
    Chunk &c = iter->second;
    for (int i = 0; i < c.size(); ++i)
        *size += c[i]->current().end;
    
    return err;
}

/* Assume there are enough multiblocks in Chunk corresponded to Chunkkey to store data pointed to
in buf.
    @todo extend Chunk with new blocks.
*/
mapd_err_t FileMgr::putChunk(const ChunkKey &key, mapd_size_t size, mapd_addr_t src, int epoch, mapd_size_t optBlockSize) {
    assert(src);
    mapd_err_t err = MAPD_SUCCESS;
    
    // ensure chunk exists
    Chunk* c;
    if ((c = getChunkRef(key)) == NULL) { // not found 
        //fprintf(stderr, "getChunkRef failed\n");
        return MAPD_ERR_CHUNK_NOT_FOUND;
    }
    
    mapd_size_t blockSize;
    // obtain blockSize from Chunk. if no blocks in the Chunk, use default param.

    if (c->size() == 0) {
        if (optBlockSize == -1) 
            // jesus user work with me here
            return MAPD_FAILURE;
        else
            blockSize = optBlockSize;
    }
    else {
        // obtain a reference to the file of the block address
        Block &blk = (*c)[0]->current();
        int fileId = blk.fileId;
        FileInfo* fInfo = getFile(fileId);
        blockSize = fInfo->blockSize;
    }

    // number of blocks to be added from src
    mapd_size_t nblocks = (size + blockSize - 1) / blockSize;
    printf("%u\n", nblocks);
    mapd_size_t blockCount = 0;

    // iterator that keeps track of all files in fileIndex_
    auto it = fileIndex_.lower_bound(blockSize);

    // write blockSize bytes from src, in Block form, to each MultiBlock. 
    for (int i = 0; i < c->size(); ++i) {

        // check list of free blocks for room to create a new block
        mapd_size_t begin;

        // find a suitable fInfo
        FileInfo* fInfo = NULL;
        for (/* preserve iterator position */; it != fileIndex_.end(); ++it) {
            if (getFile(it->second)->available() > 0) {
                fInfo = getFile(it->second);
                //fprintf(stderr, "hey found a good file! shouldn't be no error now1\n");
            }
        }
        it--;
        // @todo handle no available files 
        if (fInfo == NULL) {
        //    fprintf(stderr, "unable to find file with available space1\n");
            return MAPD_FAILURE;
        }

        // if room, iterate through free blocks and create a new block at the given size, removing address from free blocks
        else {
            auto itFree = fInfo->freeBlocks.begin();
            begin = *itFree;
            fInfo->freeBlocks.erase(itFree);
            Block* newblk = new Block(fInfo->fileId, begin);
          //  printf("begin=%u\n", begin);
            (*c)[i]->push(newblk, epoch);
        }

        mapd_size_t bytesWritten = write(fInfo->f, begin*fInfo->blockSize, size, src+blockCount*blockSize, &err);
        // check if it wrote all n bytes
        if (bytesWritten != size) {
            fprintf(stderr, "Wrote %d bytes instead of %d\n", bytesWritten, size);
            err = MAPD_FAILURE;
            return err;
        }

        nblocks--;
        blockCount++;
    }

    // Create new Multiblocks for remaining bytes.
    while (nblocks > 0) {

        // check list of free blocks for room to create a new block
        mapd_size_t begin;

        // find a suitable fInfo
        FileInfo* fInfo = NULL;
        for (/* preserve iterator position */; it != fileIndex_.end(); ++it) {
            if (getFile(it->second)->available() > 0) {
                fInfo = getFile(it->second);
                //fprintf(stderr, "hey found a good file! shouldn't be no error now2\n");
            }
        }
        --it;
        // @todo handle no available files 
        if (fInfo == NULL) {
           // fprintf(stderr, "unable to find file with available space2\n");
            return MAPD_FAILURE;
        }

        // if room, iterate through free blocks and create a new block at the given size, removing address from free blocks
        else {
            auto itFree = fInfo->freeBlocks.begin();
            begin = *itFree;
            fInfo->freeBlocks.erase(itFree);
            Block* newblk = new Block(fInfo->fileId, begin);
          //  printf("begin=%u\n", begin);
            MultiBlock* mb = new MultiBlock(fInfo->fileId, fInfo->blockSize);
            mb->push(newblk, epoch);
            c->push_back(mb);
        }

        mapd_size_t bytesWritten = write(fInfo->f, begin*fInfo->blockSize, size, src+blockCount*blockSize, &err);
        // check if it wrote all n bytes
        if (bytesWritten != size) {
            fprintf(stderr, "Wrote %d bytes instead of %d\n", bytesWritten, size);
            err = MAPD_FAILURE;
            return err;
        }

        nblocks--;
        blockCount++;

    }    

    return err;
}

// Inserts free blocks into the chunk; creates a new file if necessary
Chunk* FileMgr::createChunk(ChunkKey &key, const mapd_size_t size, const mapd_size_t blockSize, void *src, int epoch) {
    Chunk *c;

    // check if the chunk already exists based on key
    if ((c = getChunkRef(key)) != NULL) {
        return c;
    }

    // instantiate, then determine number of blocks needed
    c = new Chunk();
    mapd_size_t nblocks = (size + blockSize - 1) / blockSize;
    
    // Iterate over the files from which to obtain free blocks
    for (auto it = fileIndex_.lower_bound(blockSize); it != fileIndex_.end() && nblocks > 0; ++it) {
    	FileInfo *fInfo = getFile(it->second);

        // obtain free blocks from current file
        while (fInfo->freeBlocks.size() > 0 && nblocks > 0) {
            
            // create a new MultiBlock
            MultiBlock *mb = new MultiBlock(fInfo->fileId, fInfo->blockSize);

            // obtain a free block and remove it from the set
            auto freeIt = fInfo->freeBlocks.begin();
            mb->push(new Block(fInfo->fileId, *freeIt), epoch);
            fInfo->freeBlocks.erase(freeIt);

            // insert the MultiBlock into the new Chunk
            c->push_back(mb);
            // @todo copy src into new Chunk
            nblocks--;
        }
    }

    if (nblocks > 0) { // create a new file to hold remaining blocks
    	// @todo fix this
    	fprintf(stderr, "[%s:%d] Error: unable to insert %lu blocks into new chunk.\n", __func__, __LINE__, nblocks);
    }
    
    // Add an entry to the file manager's chunk index
    chunkIndex_.insert(std::pair<ChunkKey, Chunk>(key, *c));

    return c;
}

void freeMultiBlock(MultiBlock* mb) {
    
}

mapd_err_t FileMgr::deleteChunk(Chunk &c) {
	// @todo go through each block and each copy of the block,
	// inserting them into the free lists of their respective files
	
    // Traverse the blocks of the chunk
    for (int i = 0; i < c.size(); i++) {

        //BlockInfo *bInfo = c[0];
    }

    return MAPD_FAILURE;

    // add every block to free list, delete each block
    // delete each multiblock
    // remove Chunk from ChunkIndex
    // remove Chunk from chunkKey/chunk mapping
    // delete Chunk
}

} // File_Namespace





