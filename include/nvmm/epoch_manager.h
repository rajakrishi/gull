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

#ifndef _NVMM_EPOCH_MANAGER_H_
#define _NVMM_EPOCH_MANAGER_H_

#include <stdint.h>
#include <memory>

#include "nvmm/error_code.h"

namespace nvmm{

/** Epoch Identifier */
typedef int64_t EpochCounter;

class EpochManager
{
public:
    // Start the epoch manager (creating necessary files to bootstrap the epoch manager)
    // this function is NOT thread-safe/process-safe
    // this function must run once and only once in both single-node and multi-node environments,
    // before the first call to GetInstance()
    static void Start();

    // Reset the epoch manager (deleting all files)
    // this function is NOT thread-safe/process-safe
    // this function can only run when no one is using the epoch manager
    static void Reset();

    // there is only one instance of EpochManager in a process
    // return a pointer to the instance
    static EpochManager *GetInstance();

    // helper functions to make it work with fork
    // be careful that there may be other threads using this instance!
    // before forking, stop all the threads; after forking, start all the threads
    // both functions are not thread/process safe
    void ResetBeforeFork();
    void ResetAfterFork();

    /** Enter an epoch-protected critical region */
    void enter_critical();

    /** Exit an epoch-protected critical region */
    void exit_critical();

    /**
     * \brief Return whether there is at least one active epoch-protected
     * critical region
     *
     * \details
     * This check is inherently racy as the active region may end by the time
     * the function returns.
     * On the other hand, we have no way to tell if a thread is running inside
     * a critical region as we don't maintain per-thread state.
     */
    bool exists_active_critical();

    /** Return the last reported epoch by this epoch manager */
    EpochCounter reported_epoch();

    /** Return the frontier epoch */
    EpochCounter frontier_epoch();

    /** Set debug logging level */
    void set_debug_level(int level);

 private:

    EpochManager();
    ~EpochManager();

    class Impl_;
    std::unique_ptr<Impl_> pimpl_;
};

class EpochOp {
public:
    EpochOp(EpochManager* em);
    virtual ~EpochOp();

    EpochCounter reported_epoch();


    EpochOp(const EpochOp&)            = delete;
    EpochOp& operator=(const EpochOp&) = delete;

private:
    EpochManager* em_;
};


} // namespace nvmm
#endif
