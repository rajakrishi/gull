#include <unistd.h>
#include <string>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>

#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h"

#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/log.h"

#include "shelf_mgmt/shelf_manager.h"

#include "shelf_mgmt/shelf_file.h"

namespace nvmm {
    
ShelfFile::ShelfFile(std::string pathname)
    : fd_{-1}, path_{pathname}, shelf_id_{}
{
}

ShelfFile::ShelfFile(std::string pathname, ShelfId shelf_id)
    : fd_{-1}, path_{pathname}, shelf_id_{shelf_id}
{
}
    

ShelfFile::~ShelfFile()
{
    if (IsOpen() == true)
    {
        (void)Close();
    }
}

ErrorCode ShelfFile::Create(mode_t mode, size_t size)
{
    TRACE();
    if (Exist() == true)
    {
        return SHELF_FILE_FOUND;
    }
    if (IsOpen() == true)
    {
        return SHELF_FILE_OPENED;
    }
    
    ErrorCode ret = NO_ERROR;
    fd_ = creat(path_.c_str(), mode);
    if ( fd_ != -1)
    {
        if (size > 0)
        {
            ret = Truncate(size);
        }
        (void)Close();
        return ret;
    }
    else
    {
        if (errno == EEXIST)
        {
            return SHELF_FILE_FOUND;
        }
        else
        {
            return SHELF_FILE_CREATE_FAILED;
        }
    }
}

ErrorCode ShelfFile::Destroy()
{
    TRACE();
    ErrorCode ret = NO_ERROR;
    if (Exist() == false)
    {
        ret = SHELF_FILE_NOT_FOUND;
    }

    if (IsOpen() == true)
    {
        return SHELF_FILE_OPENED;
    }
    
    boost::filesystem::path shelf_path = boost::filesystem::path(path_.c_str());

    // remove() returns false if the path did not exist in the first place, otherwise true.
    // NOTE: boost::filesystem::remove() is racy at least up to 1.57.0, see
    // https://svn.boost.org/trac/boost/ticket/11166
    // therefore, we try to catch the exception and prevent it from being exposed
    try
    {
        (void)boost::filesystem::remove(shelf_path);
    }
    catch (boost::filesystem::filesystem_error const &err)
    {
        if(err.code().value() == 2)
        {
            LOG(trace) << "boost::filesystem::remove - BUGGY exceptions " << err.code();
        }
        else
        {
            LOG(fatal) << "boost::filesystem::remove - REAL exceptions" << err.code();
            throw (err);
        }
    }    
    return ret;
}

ErrorCode ShelfFile::Truncate(off_t length)
{
    TRACE();
    int ret;
    if(IsOpen() == false)
    {
        ret = truncate(path_.c_str(), length);
    }
    else
    {
        ret = ftruncate(fd_, length);
    }
    if (ret != -1)
    {
        return NO_ERROR;
    }
    else
    {
        if (errno == ENOENT)
        {
            return SHELF_FILE_NOT_FOUND;
        }
        else
        {
            return SHELF_FILE_TRUNCATE_FAILED;
        }
    }    
}
    
ErrorCode ShelfFile::Rename(char const *new_pathname)
{
    TRACE();
    int ret = rename(path_.c_str(), new_pathname);
    if (ret != -1)
    {
        path_ = std::string(new_pathname);
        return NO_ERROR;
    }
    else
    {
        return SHELF_FILE_RENAME_FAILED;
    }
}
    
bool ShelfFile::Exist()
{
    boost::filesystem::path shelf_path = boost::filesystem::path(path_.c_str());
    return boost::filesystem::exists(shelf_path);
}

size_t ShelfFile::Size()
{
    if(IsOpen() == false)
    {
        boost::filesystem::path shelf_path = boost::filesystem::path(path_.c_str());
        return (size_t)boost::filesystem::file_size(shelf_path);
    }
    else
    {
        struct stat buf;
        int ret = fstat(fd_, &buf);
        if (ret != -1)
        {
            return (size_t)buf.st_size;
        }
        else
        {
            return (size_t)-1;
        }
    }    
}

ErrorCode ShelfFile::Open(int flags)
{
    TRACE();
    //TRACE() << path_;    
    if(IsOpen() == true)
    {
        return SHELF_FILE_OPENED;
    }
    if (Exist() == false)
    {
        return SHELF_FILE_NOT_FOUND;
    }
    fd_ = open(path_.c_str(), flags);
    if (fd_ != -1)
    {
        return NO_ERROR;
    }
    else
    {
        if (errno == ENOENT)
        {
            return SHELF_FILE_NOT_FOUND;
        }
        else
        {
            return SHELF_FILE_OPEN_FAILED;
        }
    }
}

ErrorCode ShelfFile::Close()
{
    TRACE();
    //TRACE() << path_;    
    if(IsOpen() == false)
    {
        return SHELF_FILE_CLOSED;
    }
    int ret = close(fd_);
    if (ret != -1)
    {
        fd_ = -1;
        return NO_ERROR;
    }
    else
    {
        return SHELF_FILE_CLOSE_FAILED;
    }
}

ErrorCode ShelfFile::Map(void *addr_hint, size_t length, int prot, int flags, loff_t offset, void **mapped_addr, bool register_fam_atomic)
{
    TRACE();
    if(IsOpen() == false)
    {
        return SHELF_FILE_CLOSED;
    }

    void *ret = mmap(addr_hint, length, prot, flags, fd_, offset);
    if (ret != MAP_FAILED)
    {
        *mapped_addr = ret;
        if (register_fam_atomic == true)
        {
            //LOG(fatal) << "register fam atomic " << path_ << " " << (uint64_t)*mapped_addr;
            int rc = fam_atomic_register_region(*mapped_addr, length, fd_, offset);
            if (rc < 0)
            {
                LOG(fatal) << "fam_atomic_register_region failed";
                return SHELF_FILE_FAM_ATOMIC_REGISTER_REGION_FAILED;
            }
            //LOG(fatal) << "AFTER register fam atomic " << path_ << " " << (uint64_t)*mapped_addr;
        }
        return NO_ERROR;        
    }
    else
    {
        return SHELF_FILE_MAP_FAILED;        
    }
}

ErrorCode ShelfFile::Unmap(void *mapped_addr, size_t length, bool unregister_fam_atomic)
{
    TRACE();
    // TODO: slow!
    //pmem_persist(mapped_addr, length);
    if (unregister_fam_atomic == true)
    {
        //LOG(fatal) << "unregister fam atomic " << (uint64_t)mapped_addr;
        fam_atomic_unregister_region(mapped_addr, length);
        //LOG(fatal) << "AFTER unregister fam atomic "  << length << " " << (uint64_t)mapped_addr;
    }
    int ret = munmap(mapped_addr, length);
    if (ret != -1)
    {
        return NO_ERROR;
    }
    else
    {
        return SHELF_FILE_UNMAP_FAILED;
    }
}

    
ErrorCode ShelfFile::Map(void *addr_hint, void **mapped_addr)
{
    TRACE();
    if(IsOpen() == false)
    {
        return SHELF_FILE_CLOSED;
    }

    assert(shelf_id_.IsValid() == true);    
    void *addr;
    ShelfManager::Lock();
    addr = ShelfManager::LookupShelf(shelf_id_);
    if (addr != NULL)
    {
        *mapped_addr = addr;
        ShelfManager::Unlock();
        return NO_ERROR;
    }

    size_t length = Size();
    int prot = PROT_READ|PROT_WRITE;
    int flags = MAP_SHARED;
    loff_t offset = 0;

    ErrorCode ret = Map(addr_hint, length, prot, flags, offset, &addr, true);
    if (ret == NO_ERROR)
    {
        void *actual_addr = ShelfManager::RegisterShelf(shelf_id_, addr, length);
        assert(actual_addr == addr);
        ShelfManager::Unlock();
        *mapped_addr = actual_addr;
        return NO_ERROR;
    }
    else
    {
        ShelfManager::Unlock();
        return SHELF_FILE_MAP_FAILED;        
    }
}
    
ErrorCode ShelfFile::Unmap(void *mapped_addr, bool unregister)
{
    TRACE();
    assert(shelf_id_.IsValid() == true);
    if (unregister == true)
    {
        void *addr;
        ShelfManager::Lock();
        addr = ShelfManager::LookupShelf(shelf_id_);
        assert(addr == mapped_addr);
        addr = ShelfManager::UnregisterShelf(shelf_id_);
        assert(addr == mapped_addr);        
        ShelfManager::Unlock();
        size_t size = Size();
        return Unmap(mapped_addr, size, true);
    }
    else
    {
        void *addr;
        ShelfManager::Lock();
        addr = ShelfManager::LookupShelf(shelf_id_);
        assert(addr == mapped_addr);
        ShelfManager::Unlock();
        return NO_ERROR;
    }
}
    
} // namespace nvmm