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

#include <memory>

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <boost/filesystem.hpp>

#include "nvmm/error_code.h"
#include "nvmm/epoch_manager.h"
#include "nvmm/log.h"

#include "common/epoch_shelf.h"
#include "shelf_usage/epoch_manager_impl.h"

namespace nvmm {

/*
 * Internal implementation of EpochManager
 */
class EpochManager::Impl_
{
public:
    static std::string const kEpochShelfPath; // path of the epoch shelf

    EpochManagerImpl *em;

    Impl_()
        : em(NULL), epoch_shelf_(kEpochShelfPath)
    {
    }
    ~Impl_()
    {
    }

    ErrorCode Init();
    ErrorCode Final();

private:
    EpochShelf epoch_shelf_;
};

std::string const EpochManager::Impl_::kEpochShelfPath = std::string(SHELF_BASE_DIR) + "/" + SHELF_USER + "_NVMM_EPOCH";

ErrorCode EpochManager::Impl_::Init()
{
    boost::filesystem::path shelf_base_path = boost::filesystem::path(SHELF_BASE_DIR);
    if (boost::filesystem::exists(shelf_base_path) == false)
    {
        LOG(fatal) << "NVMM: LFS/tmpfs does not exist?" << SHELF_BASE_DIR;
        exit(1);
    }

    if (epoch_shelf_.Exist() == false)
    {
        LOG(fatal) << "NVMM: Epoch shelf does not exist?" << kEpochShelfPath;
        exit(1);
    }

    if (epoch_shelf_.Open() != NO_ERROR)
    {
        LOG(fatal) << "NVMM: Epoch shelf open failed..." << kEpochShelfPath;
        exit(1);
    }

    em = new EpochManagerImpl(epoch_shelf_.Addr(), false);
    assert(em);

    return NO_ERROR;
}

ErrorCode EpochManager::Impl_::Final()
{
    if (em)
        delete em;

    ErrorCode ret = epoch_shelf_.Close();
    if (ret!=NO_ERROR)
    {
        LOG(fatal) << "NVMM: Epoch shelf close failed" << kEpochShelfPath;
        exit(1);
    }
    return ret;
}


/*
 * Public APIs of EpochManager
 */
void EpochManager::Start() {
    // Check if SHELF_BASE_DIR exists
    boost::filesystem::path shelf_base_path = boost::filesystem::path(SHELF_BASE_DIR);
    if (boost::filesystem::exists(shelf_base_path) == false)
    {
        LOG(fatal) << "NVMM: LFS/tmpfs does not exist?" << SHELF_BASE_DIR;
        exit(1);
    }

    // create a epoch shelf for EpochManager if it does not exist
    EpochShelf epoch_shelf(EpochManager::Impl_::kEpochShelfPath);
    if(epoch_shelf.Exist() == false)
    {
        ErrorCode ret = epoch_shelf.Create();
        if (ret!=NO_ERROR && ret != SHELF_FILE_FOUND)
        {
            LOG(fatal) << "NVMM: Failed to create the epoch shelf file " << EpochManager::Impl_::kEpochShelfPath;
            exit(1);
        }
    }
}

void EpochManager::Reset() {
    std::string cmd = std::string("exec rm -f ") + EpochManager::Impl_::kEpochShelfPath + " > /dev/null";
    (void)system(cmd.c_str());
}

// thread-safe Singleton pattern with C++11
// see http://preshing.com/20130930/double-checked-locking-is-fixed-in-cpp11/
EpochManager *EpochManager::GetInstance()
{
    static EpochManager instance;
    return &instance;
}

EpochManager::EpochManager()
    : pimpl_{new Impl_}
{
    ErrorCode ret = pimpl_->Init();
    assert(ret == NO_ERROR);
}

EpochManager::~EpochManager()
{
    ErrorCode ret = pimpl_->Final();
    assert(ret == NO_ERROR);
}

void EpochManager::ResetBeforeFork()
{
    ErrorCode ret = pimpl_->Final();
    assert(ret == NO_ERROR);
}

void EpochManager::ResetAfterFork()
{
    ErrorCode ret = pimpl_->Init();
    assert(ret == NO_ERROR);
}

void EpochManager::enter_critical() {
    pimpl_->em->enter_critical();
}


void EpochManager::exit_critical() {
    pimpl_->em->exit_critical();
}


bool EpochManager::exists_active_critical() {
    return pimpl_->em->exists_active_critical();
}

EpochCounter EpochManager::reported_epoch() {
    return pimpl_->em->reported_epoch();
}


EpochCounter EpochManager::frontier_epoch() {
    return pimpl_->em->frontier_epoch();
}

void EpochManager::set_debug_level(int level) {
    pimpl_->em->set_debug_level(level);
}

} // end namespace nvmm
