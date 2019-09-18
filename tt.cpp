/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2019 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstring>   // For std::memset
#include <iostream>
#include <thread>
#include <cstring>   // For std::memset
#include <fstream> //from kellykynyama mcts
#include "bitboard.h"
#include "misc.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"

//from Kelly Begin
using namespace std;
LearningHashTable globalLearningHT,experienceHT;
//from Kelly end
TranspositionTable TT; // Our global transposition table

/// TTEntry::save populates the TTEntry with a new node's data, possibly
/// overwriting an old position. Update is not atomic and can be racy.

void TTEntry::save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev) {

  assert(d / ONE_PLY * ONE_PLY == d);

  // Preserve any existing move for the same position
  if (m || (k >> 48) != key16)
      move16 = (uint16_t)m;

  // Overwrite less valuable entries
  if (  (k >> 48) != key16
      ||(d - DEPTH_OFFSET) / ONE_PLY > depth8 - 4
      || b == BOUND_EXACT)
  {
      assert((d - DEPTH_OFFSET) / ONE_PLY >= 0);

      key16     = (uint16_t)(k >> 48);
      value16   = (int16_t)v;
      eval16    = (int16_t)ev;
      genBound8 = (uint8_t)(TT.generation8 | uint8_t(pv) << 2 | b);
      depth8    = (uint8_t)((d - DEPTH_OFFSET) / ONE_PLY);
  }
}

int use_large_pages = -1;
int got_privileges = -1;


bool Get_LockMemory_Privileges()
{
    HANDLE TH, PROC7;
    TOKEN_PRIVILEGES tp;
    bool ret = false;

    PROC7 = GetCurrentProcess();
    if (OpenProcessToken(PROC7, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &TH))
    {
        if (LookupPrivilegeValue(NULL, TEXT("SeLockMemoryPrivilege"), &tp.Privileges[0].Luid))
        {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (AdjustTokenPrivileges(TH, FALSE, &tp, 0, NULL, 0))
            {
                if (GetLastError() != ERROR_NOT_ALL_ASSIGNED)
                    ret = true;
            }
        }
        CloseHandle(TH);
    }
    return ret;
}


void Try_Get_LockMemory_Privileges()
{
    use_large_pages = 0;

    if (!Options["Large Pages"])
        return;

    if (got_privileges == -1)
    {
        if (Get_LockMemory_Privileges() == true)
            got_privileges = 1;
        else
        {
            sync_cout << "No Privilege for Large Pages" << sync_endl;
            got_privileges = 0;
        }
    }

    if (got_privileges == 0)      
        return;

    use_large_pages = 1;        
}


/// TranspositionTable::resize() sets the size of the transposition table,
/// measured in megabytes. Transposition table consists of a power of 2 number
/// of clusters and each cluster consists of ClusterSize number of TTEntry.

void TranspositionTable::resize(size_t mbSize) {

  if (mbSize == 0)
      mbSize = mbSize_last_used;

  if (mbSize == 0)
      return;

  mbSize_last_used = mbSize;

  Try_Get_LockMemory_Privileges();

  size_t newClusterCount = mbSize * 1024 * 1024 / sizeof(Cluster);

  if (newClusterCount == clusterCount)
  {
      if ((use_large_pages == 1) && (large_pages_used))      
          return;
      if ((use_large_pages == 0) && (large_pages_used == false))
          return;
  }

  clusterCount = newClusterCount;
 
  if (use_large_pages < 1)
  {
      if (mem != NULL)
      {
          if (large_pages_used)
              VirtualFree(mem, 0, MEM_RELEASE);
          else          
              free(mem);
      }
      uint64_t memsize = clusterCount * sizeof(Cluster) + CacheLineSize - 1;
      mem = calloc(memsize, 1);
      large_pages_used = false;
  }
  else
  {
      if (mem != NULL)
      {
          if (large_pages_used)
              VirtualFree(mem, 0, MEM_RELEASE);
          else
              free(mem);
      }

      int64_t memsize = clusterCount * sizeof(Cluster);
      mem = VirtualAlloc(NULL, memsize, MEM_LARGE_PAGES | MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (mem == NULL)
      {
          std::cerr << "Failed to allocate " << mbSize
              << "MB Large Page Memory for transposition table, switching to default" << std::endl;

          use_large_pages = 0;
          mem = malloc(clusterCount * sizeof(Cluster) + CacheLineSize - 1);
          large_pages_used = false;
      }
      else
      {
          sync_cout << "info string Hash LargePages " << (memsize >> 20) << " Mb" << sync_endl;
          large_pages_used = true;
      }
        
  }

  if (!mem)
  {
      std::cerr << "Failed to allocate " << mbSize
                << "MB for transposition table." << std::endl;
      exit(EXIT_FAILURE);
  }

  table = (Cluster*)((uintptr_t(mem) + CacheLineSize - 1) & ~(CacheLineSize - 1));
  clear();
}


/// TranspositionTable::clear() initializes the entire transposition table to zero,
//  in a multi-threaded way.

void TranspositionTable::clear() {

  std::vector<std::thread> threads;

  for (size_t idx = 0; idx < Options["Threads"]; ++idx)
  {
      threads.emplace_back([this, idx]() {

          // Thread binding gives faster search on systems with a first-touch policy
          if (Options["Threads"] > 8)
              WinProcGroup::bindThisThread(idx);

          // Each thread will zero its part of the hash table
          const size_t stride = clusterCount / Options["Threads"],
                       start  = stride * idx,
                       len    = idx != Options["Threads"] - 1 ?
                                stride : clusterCount - start;

          std::memset(&table[start], 0, len * sizeof(Cluster));
      });
  }

  for (std::thread& th: threads)
      th.join();
}

/// TranspositionTable::probe() looks up the current position in the transposition
/// table. It returns true and a pointer to the TTEntry if the position is found.
/// Otherwise, it returns false and a pointer to an empty or least valuable TTEntry
/// to be replaced later. The replace value of an entry is calculated as its depth
/// minus 8 times its relative age. TTEntry t1 is considered more valuable than
/// TTEntry t2 if its replace value is greater than that of t2.

TTEntry* TranspositionTable::probe(const Key key, bool& found) const {

  TTEntry* const tte = first_entry(key);
  const uint16_t key16 = key >> 48;  // Use the high 16 bits as key inside the cluster

  for (int i = 0; i < ClusterSize; ++i)
      if (!tte[i].key16 || tte[i].key16 == key16)
      {
          tte[i].genBound8 = uint8_t(generation8 | (tte[i].genBound8 & 0x7)); // Refresh

          return found = (bool)tte[i].key16, &tte[i];
      }

  // Find an entry to be replaced according to the replacement strategy
  TTEntry* replace = tte;
  for (int i = 1; i < ClusterSize; ++i)
      // Due to our packed storage format for generation and its cyclic
      // nature we add 263 (256 is the modulus plus 7 to keep the unrelated
      // lowest three bits from affecting the result) to calculate the entry
      // age correctly even after generation8 overflows into the next cycle.
      if (  replace->depth8 - ((263 + generation8 - replace->genBound8) & 0xF8)
          >   tte[i].depth8 - ((263 + generation8 -   tte[i].genBound8) & 0xF8))
          replace = &tte[i];

  return found = false, replace;
}


/// TranspositionTable::hashfull() returns an approximation of the hashtable
/// occupation during a search. The hash is x permill full, as per UCI protocol.

int TranspositionTable::hashfull() const {

  int cnt = 0;
  for (int i = 0; i < 1000 / ClusterSize; ++i)
      for (int j = 0; j < ClusterSize; ++j)
          cnt += (table[i].entry[j].genBound8 & 0xF8) == generation8;

  return cnt * 1000 / (ClusterSize * (1000 / ClusterSize));
}
//from Kelly begin
void loadLearningFileIntoLearningTables(bool toDeleteBinFile) {
  std::string fileName="experience";
  ifstream inputLearningFile("experience.bin", ios::in | ios::binary);
  int loading = 1;
  while (loading)
  {
    LearningFileEntry currentInputLearningFileEntry;
    currentInputLearningFileEntry.depth = DEPTH_ZERO;
    currentInputLearningFileEntry.hashKey = 0;
    currentInputLearningFileEntry.move = MOVE_NONE;
    currentInputLearningFileEntry.score = VALUE_NONE;
    inputLearningFile.read((char*)&currentInputLearningFileEntry, sizeof(currentInputLearningFileEntry));
    if (currentInputLearningFileEntry.hashKey)
    {
      insertIntoOrUpdateLearningTable(currentInputLearningFileEntry,globalLearningHT);
      if(toDeleteBinFile)
      {
	 insertIntoOrUpdateLearningTable(currentInputLearningFileEntry,experienceHT);
      }
    }
    else
      loading = 0;
  }
  inputLearningFile.close();
  if(toDeleteBinFile)
  {
    char fileNameStr[fileName.size() + 1];
    strcpy(fileNameStr, fileName.c_str());
    remove(fileNameStr);
  }
}

void insertIntoOrUpdateLearningTable(LearningFileEntry& fileExpEntry,LearningHashTable& learningHT)
{
    // We search in the range of all the hash table entries with key fileExpEntry
    auto range = learningHT.equal_range(fileExpEntry.hashKey);
    auto it1 = range.first;
    auto it2 = range.second;

    bool isNewNode = true;
    while (it1 != it2)
    {
      Node node = &(it1->second);
      if (node->hashKey == fileExpEntry.hashKey)
	{
	  isNewNode = false;
	  if(
	    (((node->latestMoveInfo.move == fileExpEntry.move) && (node->latestMoveInfo.depth <= fileExpEntry.depth))
		||
	     ((node->latestMoveInfo.move != fileExpEntry.move) &&
	      ((node->latestMoveInfo.depth < fileExpEntry.depth) || ((node->latestMoveInfo.depth == fileExpEntry.depth) && (node->latestMoveInfo.score <= fileExpEntry.score ))))
	    )
	   )
	  { // Return the HashTable's node updated
	    //update lateChild begin
	    node->latestMoveInfo.move = fileExpEntry.move;
	    node->latestMoveInfo.score = fileExpEntry.score;
	    node->latestMoveInfo.depth = fileExpEntry.depth;
	    //update lateChild end
	    //exit the position
	    break;
	  }
	}
      it1++;
    }

    if (isNewNode)
    {
      // Node was not found, so we have to create a new one
      NodeInfo infos;
      infos.hashKey = fileExpEntry.hashKey;
      infos.latestMoveInfo.move = fileExpEntry.move;
      infos.latestMoveInfo.score = fileExpEntry.score;
      infos.latestMoveInfo.depth = fileExpEntry.depth;
      learningHT.insert(make_pair(fileExpEntry.hashKey, infos));
    }
}

/// getNodeFromGlobalHT(Key key) probes the Monte-Carlo hash table to return the node with the given
/// position or a nullptr Node if it doesn't exist yet in the table.
Node getNodeFromHT(Key key,HashTableType hashTableType)
{
  // We search in the range of all the hash table entries with key key.
  Node currentNode = nullptr;
  auto range=globalLearningHT.equal_range(key);
  if(hashTableType==HashTableType::experience)
    {
      range=experienceHT.equal_range(key);
    }
  auto it1 = range.first;
  auto it2 = range.second;
  while (it1 != it2)
  {
    currentNode = &(it1->second);
    if (currentNode->hashKey == key)
    {
	return currentNode;
    }
    it1++;
  }

  return currentNode;
}

void writeLearningFile(HashTableType hashTableType)
{
  std::ofstream outputFile ("experience.bin", std::ofstream::trunc | std::ofstream::binary);
  LearningHashTable currentLearningHT;
  currentLearningHT=experienceHT;
  if(hashTableType==HashTableType::global)
    {
      currentLearningHT=globalLearningHT;
    }
  for(auto& it:currentLearningHT)
  {
    LearningFileEntry currentFileExpEntry;
    NodeInfo currentNodeInfo=it.second;
    MoveInfo currentLatestMoveInfo=currentNodeInfo.latestMoveInfo;
    currentFileExpEntry.depth = currentLatestMoveInfo.depth;
    currentFileExpEntry.hashKey = it.first;
    currentFileExpEntry.move = currentLatestMoveInfo.move;
    currentFileExpEntry.score = currentLatestMoveInfo.score;
    outputFile.write((char*)&currentFileExpEntry, sizeof(currentFileExpEntry));
  }
  outputFile.close();
}

void loadSlaveLearningFilesIntoLearningTables()
{
    bool merging=true;
    int i=0;
    while (merging)
    {
      std::string index = std::to_string(i);
      std::string slaveFileName ="";
      slaveFileName="experience" + index + ".bin";
      ifstream slaveInputFile (slaveFileName, ios::in | ios::binary);
      if(!slaveInputFile.good())
      {
	merging=false;
	i++;
      }
      else
      {
	while(slaveInputFile.good())
	{
	  LearningFileEntry slaveFileExpEntry;
	  slaveFileExpEntry.depth = DEPTH_ZERO;
	  slaveFileExpEntry.hashKey = 0;
	  slaveFileExpEntry.move = MOVE_NONE;
	  slaveFileExpEntry.score = VALUE_NONE;

	  slaveInputFile.read((char*)&slaveFileExpEntry, sizeof(slaveFileExpEntry));
	  if (slaveFileExpEntry.hashKey)
	  {
	      insertIntoOrUpdateLearningTable(slaveFileExpEntry,experienceHT);
	  }
	  else
	  {
	    slaveInputFile.close();
	    char slaveStr[slaveFileName.size() + 1];
	    strcpy(slaveStr, slaveFileName.c_str());
	    remove(slaveStr);
	    i++;
	  }
	}
      }
    }
}
//from Kelly End
