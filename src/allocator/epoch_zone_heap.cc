/*
 *  (c) Copyright 2016-2017 Hewlett Packard Enterprise Development Company LP.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As an exception, the copyright holders of this Library grant you permission
 *  to (i) compile an Application with the Library, and (ii) distribute the
 *  Application containing code generated by the Library and added to the
 *  Application during this compilation process under terms of your choice,
 *  provided you also meet the terms and conditions of the Application license.
 *
 */

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <assert.h>

#include "nvmm/error_code.h"
#include "nvmm/global_ptr.h"
#include "nvmm/shelf_id.h"
#include "nvmm/heap.h"
#include "nvmm/nvmm_libpmem.h"
#include "nvmm/memory_manager.h"
#include "nvmm/log.h"

#include "shelf_mgmt/pool.h"

#include "shelf_usage/zone_shelf_heap.h"
#include "shelf_usage/shelf_region.h"

#include "nvmm/epoch_manager.h"

#include "allocator/epoch_zone_heap.h"

namespace nvmm {

EpochZoneHeap::EpochZoneHeap(PoolId pool_id)
    : pool_id_{pool_id}, pool_{pool_id}, rmb_size_{0}, rmb_{NULL}, region_size_{0}, region_{NULL},
      mapped_addr_{NULL}, header_{NULL}, is_open_{false},
      cleaner_start_{false}, cleaner_stop_{false}, cleaner_running_{false}
{
}

EpochZoneHeap::~EpochZoneHeap()
{
    if(IsOpen() == true)
    {
        (void)Close();
    }
}

ErrorCode EpochZoneHeap::Create(size_t shelf_size)
{
    TRACE();
    assert(IsOpen() == false);
    if (pool_.Exist() == true)
    {
        return POOL_FOUND;
    }
    else
    {
        ErrorCode ret = NO_ERROR;

        // create an empty pool
        ret = pool_.Create(shelf_size);
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }

        // add two shelves, one for zone, one for the headers
        ret = pool_.Open(false);
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }
        ShelfIndex shelf_idx;

        shelf_idx = kHeaderIdx;
        ret = pool_.AddShelf(shelf_idx,
                             [this](ShelfFile *shelf, size_t shelf_size)
                            {
                                ShelfRegion shelf_region(shelf->GetPath());
                                return shelf_region.Create(shelf_size);
                            },
                            false
                            );
        if (ret != NO_ERROR)
        {
            (void)pool_.Close(false);
            return HEAP_CREATE_FAILED;
        }

        std::string path;

        // get the header shelf
        ret = pool_.GetShelfPath(kHeaderIdx, path);
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }

        // open region
        region_ = new ShelfRegion(path);
        assert (region_ != NULL);
        ret = region_->Open(O_RDWR);
        if (ret != NO_ERROR)
        {
            LOG(error) << "Zone: region open failed " << (uint64_t)pool_id_;
            return HEAP_CREATE_FAILED;
        }

        // map region
        region_size_ = region_->Size();
        ret = region_->Map(NULL, region_size_, PROT_READ|PROT_WRITE, MAP_SHARED, 0, (void**)&mapped_addr_);
        if (ret != NO_ERROR)
        {
            LOG(error) << "Zone: region map failed " << (uint64_t)pool_id_;
            return HEAP_CREATE_FAILED;
        }

        // reserve the 5 global lists for delayed free
        uint64_t reserved = kListCnt * sizeof(ZoneEntryStack);
        pmem_memset_persist(mapped_addr_, 0, reserved);

        // use this region to help create the zone
        header_=(void*)((char*)mapped_addr_+round_up(reserved, kCacheLineSize));
        shelf_idx = kZoneIdx;
        ret = pool_.AddShelf(shelf_idx,
                             [this](ShelfFile *shelf, size_t shelf_size)
                            {
                                ShelfHeap shelf_heap(shelf->GetPath());
                                return shelf_heap.Create(shelf_size, header_, region_size_);
                            },
                            false
                            );
        if (ret != NO_ERROR)
        {
            // unmap and close the region
            ret = region_->Unmap(mapped_addr_, region_size_);
            if (ret != NO_ERROR)
            {
                return HEAP_CREATE_FAILED;
            }
            ret = region_->Close();
            if (ret != NO_ERROR)
            {
                return HEAP_CREATE_FAILED;
            }
            delete region_;
            (void)pool_.Close(false);
            return HEAP_CREATE_FAILED;
        }

        // unmap and close the region
        ret = region_->Unmap(mapped_addr_, region_size_);
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }
        ret = region_->Close();
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }
        delete region_;

        ret = pool_.Close(false);
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }

        return ret;
    }
}

ErrorCode EpochZoneHeap::Destroy()
{
    TRACE();
    assert(IsOpen() == false);
    if (pool_.Exist() == false)
    {
        return POOL_NOT_FOUND;
    }
    else
    {
        ErrorCode ret = NO_ERROR;

        // remove both shelves
        ret = pool_.Open(false);
        if (ret != NO_ERROR)
        {
            return HEAP_DESTROY_FAILED;
        }
        ret = pool_.Recover();
        if(ret != NO_ERROR)
        {
            LOG(fatal) << "Destroy: Found inconsistency in Heap " << (uint64_t)pool_id_;
        }

        std::string path;

        ret = pool_.GetShelfPath(kZoneIdx, path);
        assert(ret == NO_ERROR);
        ShelfHeap shelf_heap(path, ShelfId(pool_id_, kZoneIdx));
        shelf_heap.Destroy();

        ret = pool_.RemoveShelf(kZoneIdx);
        if (ret != NO_ERROR)
        {
            (void)pool_.Close(false);
            return HEAP_DESTROY_FAILED;
        }

        ret = pool_.GetShelfPath(kHeaderIdx, path);
        assert(ret == NO_ERROR);
        ShelfRegion shelf_region(path);
        shelf_region.Destroy();

        ret = pool_.RemoveShelf(kHeaderIdx);
        if (ret != NO_ERROR)
        {
            (void)pool_.Close(false);
            return HEAP_DESTROY_FAILED;
        }

        ret = pool_.Close(false);
        if (ret != NO_ERROR)
        {
            return HEAP_DESTROY_FAILED;
        }

        // destroy the pool
        ret = pool_.Destroy();
        if (ret != NO_ERROR)
        {
            return HEAP_DESTROY_FAILED;
        }
        return ret;
    }
}

bool EpochZoneHeap::Exist()
{
    return pool_.Exist();
}

ErrorCode EpochZoneHeap::Open()
{
    TRACE();
    LOG(trace) << "Open Heap " << (uint64_t) pool_id_;
    assert(IsOpen() == false);
    ErrorCode ret = NO_ERROR;

    // open the pool
    ret = pool_.Open(false);
    if (ret != NO_ERROR)
    {
        return HEAP_OPEN_FAILED;
    }

    // // perform recovery
    // ret = pool_.Recover();
    // if(ret != NO_ERROR)
    // {
    //     LOG(fatal) << "Open: Found inconsistency in Region " << (uint64_t)pool_id_;
    // }

    std::string path;

    // get the header shelf
    ret = pool_.GetShelfPath(kHeaderIdx, path);
    if (ret != NO_ERROR)
    {
        return HEAP_OPEN_FAILED;
    }

    // open region
    region_ = new ShelfRegion(path);
    assert (region_ != NULL);
    ret = region_->Open(O_RDWR);
    if (ret != NO_ERROR)
    {
        LOG(error) << "Zone: region open failed " << (uint64_t)pool_id_;
        return HEAP_OPEN_FAILED;
    }

    // map region
    region_size_ = region_->Size();
    ret = region_->Map(NULL, region_size_, PROT_READ|PROT_WRITE, MAP_SHARED, 0, (void**)&mapped_addr_);
    if (ret != NO_ERROR)
    {
        LOG(error) << "Zone: region map failed " << (uint64_t)pool_id_;
        return HEAP_OPEN_FAILED;
    }

    // get the global freelists
    global_list_ = (ZoneEntryStack*)mapped_addr_;

    // get the zone shelf
    ret = pool_.GetShelfPath(kZoneIdx, path);
    if (ret != NO_ERROR)
    {
        return HEAP_OPEN_FAILED;
    }

    // open rmb
    uint64_t reserved = round_up(kListCnt * sizeof(ZoneEntryStack), kCacheLineSize);
    header_=(void*)((char*)mapped_addr_+round_up(reserved, kCacheLineSize));
    rmb_ = new ShelfHeap(path, ShelfId(pool_id_, kZoneIdx));
    assert (rmb_ != NULL);
    ret = rmb_->Open(header_, region_size_);
    if (ret != NO_ERROR)
    {
        LOG(error) << "Zone: rmb open failed " << (uint64_t)pool_id_;
        return HEAP_OPEN_FAILED;
    }
    is_open_ = true;
    assert(ret == NO_ERROR);

    rmb_size_ = rmb_->Size();
    min_obj_size_ = rmb_->MinAllocSize();

    // start the cleaner thread
    int rc = StartWorker();
    if (rc != 0)
    {
        return HEAP_OPEN_FAILED;
    }

    // wait for the cleaner to be running
    std::unique_lock<std::mutex> mutex(cleaner_mutex_);
    running_cv_.wait(mutex, [this]{return cleaner_running_==true;});
    return ret;
}

ErrorCode EpochZoneHeap::Close()
{
    TRACE();
    LOG(trace) << "Close Heap " << (uint64_t)pool_id_;
    assert(IsOpen() == true);
    ErrorCode ret = NO_ERROR;

    // stop the cleaner thread
    int rc = StopWorker();
    if (rc != 0)
    {
        return HEAP_CLOSE_FAILED;
    }

    // close the rmb
    ret = rmb_->Close();
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }
    delete rmb_;
    rmb_=NULL;

    // unmap and close the region
    ret = region_->Unmap(mapped_addr_, region_size_);
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }
    mapped_addr_=NULL;
    header_=NULL;
    global_list_=NULL;

    ret = region_->Close();
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }
    delete region_;
    region_=NULL;

    // // perform recovery
    // ret = pool_.Recover();
    // if(ret != NO_ERROR)
    // {
    //     LOG(fatal) << "Close: Found inconsistency in Heap " << (uint64_t)pool_id_;
    // }

    // close the pool
    ret = pool_.Close(false);
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }

    rmb_size_ = 0;
    region_size_ = 0;
    is_open_ = false;

    assert(ret == NO_ERROR);
    return ret;
}

size_t EpochZoneHeap::Size()
{
    assert(IsOpen() == true);
    return rmb_size_;
}

GlobalPtr EpochZoneHeap::Alloc (size_t size)
{
    assert(IsOpen() == true);
    GlobalPtr ptr;
    Offset offset = rmb_->Alloc(size);
    if (rmb_->IsValidOffset(offset) == true)
    {
        // this offset has size encoded
        ptr = GlobalPtr(ShelfId(pool_id_, kZoneIdx), offset);
    }
    return ptr;
}

void EpochZoneHeap::Free(GlobalPtr global_ptr)
{
    assert(IsOpen() == true);
    Offset offset = global_ptr.GetOffset();
    rmb_->Free(offset);
}

GlobalPtr EpochZoneHeap::Alloc (EpochOp &op, size_t size)
{
    assert(IsOpen() == true);
    (void)op; // we don't use epoch to do allocation, but this allocation must be in an EpochOp
    return Alloc(size);
}

void EpochZoneHeap::Free(EpochOp &op, GlobalPtr global_ptr)
{
    assert(IsOpen() == true);
    Offset offset = global_ptr.GetOffset();
    if (rmb_->IsValidOffset(offset) == false)
        return;

    {
        EpochCounter e = op.reported_epoch();
        LOG(trace) << "delay freeing block [" << offset << "] at epoch " << e+3;
        global_list_[(e+3)%kListCnt].push(header_, offset/min_obj_size_);
    }
}

void *EpochZoneHeap::GlobalToLocal(GlobalPtr global_ptr)
{
    TRACE();
    assert(IsOpen() == true);
    void *local_ptr = NULL;
    Offset offset = global_ptr.GetOffset();
    local_ptr = rmb_->OffsetToPtr(offset);
    return local_ptr;
}

int EpochZoneHeap::StartWorker()
{
    // start the cleaner thread
    std::lock_guard<std::mutex> mutex(cleaner_mutex_);
    if (cleaner_start_ == true)
    {
        LOG(trace) << " cleaner thread is already started...";
        return 0;
    }
    cleaner_start_ = true;
    cleaner_stop_ = false;
    cleaner_running_ = false;
    cleaner_thread_ = std::thread(&EpochZoneHeap::BackgroundWorker, this);
    return 0;
}

int EpochZoneHeap::StopWorker()
{
    // signal the cleaner to stop
    {
        std::lock_guard<std::mutex> mutex(cleaner_mutex_);
        if (cleaner_running_ == false || cleaner_start_ == false)
        {
            LOG(trace) << " cleaner thread is not running...";
            return 0;
        }
        cleaner_stop_ = true;
    }

    // join the cleaner thread
    if(cleaner_thread_.joinable())
    {
        cleaner_thread_.join();
    }

    {
        std::lock_guard<std::mutex> mutex(cleaner_mutex_);
        cleaner_start_ = false;
        cleaner_stop_ = false;
        cleaner_running_ = false;
    }
    return 0;
}

void EpochZoneHeap::BackgroundWorker()
{
    TRACE();
    assert(IsOpen() == true);

    while(1)
    {
        LOG(trace) << "cleaner: sleep";
        usleep(kWorkerSleepMicroSeconds);
        LOG(trace) << "cleaner: wakeup";

        // check if we are shutting down...
        {
            std::lock_guard<std::mutex> mutex(cleaner_mutex_);
            if (cleaner_running_ == false)
            {
                cleaner_running_ = true;
                LOG(trace) << "cleaner: running...";
                running_cv_.notify_all();
            }
            if (cleaner_stop_ == true)
            {
                LOG(trace) << "cleaner: exiting...";
                return;
            }
        }

        // do work
        {
            EpochManager *em = EpochManager::GetInstance();
            EpochOp op(em);
            EpochCounter e = op.reported_epoch();

            LOG(trace) << "cleaner: now looking at epoch " << e;
            int i=0;
            for(; i<kFreeCnt; i++)
            {
                Offset offset = global_list_[e%kListCnt].pop(header_) * min_obj_size_;
                if (offset==0)
                    break;
                // TODO: a crash here will leak memory
                LOG(trace) << " freeing block [" << offset << "]";
                rmb_->Free(offset);
            }
            LOG(trace) << " in total " << i << " blocks have been freed";
        }
    }
}

} // namespace nvmm
