#include <pthread.h>
#include <string>
#include <gtest/gtest.h>

#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h"
#include "nvmm/global_ptr.h"
#include "nvmm/memory_manager.h"

#include "test_common/test.h"

#include "shelf_mgmt/shelf_file.h"
#include "shelf_mgmt/shelf_name.h"
#include "shelf_mgmt/shelf_manager.h"
#ifdef ZONE
#include "shelf_usage/zone_shelf_heap.h"
#else
#include "shelf_usage/shelf_heap.h"
#endif

using namespace nvmm;

static size_t const kShelfSize = 128*1024*1024LLU; // 128 M
//static size_t const kShelfSize = 8192*1024*1024LLU; // 128 MB
static ShelfId const kShelfId = ShelfId(1,1);

ShelfName shelf_name;

// single-threaded
// TODO: verify
TEST(ShelfHeap, CreateDestroyVerify)
{
    std::string shelf_path = shelf_name.Path(kShelfId);
    ShelfFile shelf(shelf_path, kShelfId);
    ShelfHeap heap(shelf_path, kShelfId);
    size_t heap_size = kShelfSize;

    // create a shelf
    EXPECT_EQ(NO_ERROR, shelf.Create(S_IRUSR | S_IWUSR));
    
    // create a shelf heap
    EXPECT_EQ(NO_ERROR, heap.Create(heap_size));
    EXPECT_EQ(NO_ERROR, heap.Verify());
    
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap.Destroy());

    // destroy the shelf
    EXPECT_EQ(NO_ERROR, shelf.Destroy());
}

TEST(ShelfHeap, OpenCloseSize)
{
    std::string shelf_path = shelf_name.Path(kShelfId);    
    ShelfFile shelf(shelf_path, kShelfId);
    ShelfHeap heap(shelf_path, kShelfId);
    size_t heap_size = kShelfSize;

    // open a shelf heap that does not exist
    EXPECT_EQ(SHELF_FILE_NOT_FOUND, heap.Open());
    
    // create a shelf
    EXPECT_EQ(NO_ERROR, shelf.Create(S_IRUSR | S_IWUSR));
    
    // create a shelf heap
    EXPECT_EQ(NO_ERROR, heap.Create(heap_size));
    
    // open the heap
    EXPECT_EQ(NO_ERROR, heap.Open());

    EXPECT_EQ(heap_size, heap.Size());
    
    // close the heap
    EXPECT_EQ(NO_ERROR, heap.Close());
    
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap.Destroy());

    // destroy the shelf
    EXPECT_EQ(NO_ERROR, shelf.Destroy());    
}

TEST(ShelfHeap, AllocFree)
{
    ErrorCode ret = NO_ERROR;
    std::string shelf_path = shelf_name.Path(kShelfId);    
    ShelfFile shelf(shelf_path, kShelfId);
    ShelfHeap heap(shelf_path, kShelfId);
    int const count = 10;
    size_t heap_size = kShelfSize;
    
    // create a shelf
    ret = shelf.Create(S_IRUSR | S_IWUSR);
    EXPECT_EQ(NO_ERROR, ret);

    // create a shelf heap
    ret = heap.Create(heap_size);
    EXPECT_EQ(NO_ERROR, ret);

    // open the heap
    EXPECT_EQ(NO_ERROR, heap.Open());

    // alloc
    Offset offset[count];
    for (int i=0; i<count; i++)
    {
        offset[i] = heap.Alloc(sizeof(int));
        EXPECT_TRUE(heap.IsValidOffset(offset[i]));
        int *ptr = (int*)heap.OffsetToPtr(offset[i]);
#ifndef ZONE
        EXPECT_EQ(offset[i], heap.PtrToOffset(ptr));
#endif        
        *ptr = i;
    }

    // // alloc failure
    // Offset offset_fail;
    // offset_fail = heap.Alloc(kShelfSize);
    // EXPECT_FALSE(heap.IsValidOffset(offset_fail));
    
    // close the heap
    EXPECT_EQ(NO_ERROR, heap.Close());        

    // open the heap again
    EXPECT_EQ(NO_ERROR, heap.Open());

    // verify and free
    for (int i=0; i<count; i++)
    {
        int *ptr = (int*)heap.OffsetToPtr(offset[i]);
        EXPECT_EQ(i, *ptr);        
        heap.Free(offset[i]);
    }

    // close the heap
    EXPECT_EQ(NO_ERROR, heap.Close());        

    // destroy the heap
    ret = heap.Destroy(); 
    EXPECT_EQ(NO_ERROR, ret);
   
    // destroy the shelf
    ret = shelf.Destroy();
    EXPECT_EQ(NO_ERROR, ret);
}


// multi-threaded
struct thread_argument{
    int id;
    int try_count;
    ShelfHeap *heap;
};

void *worker(void *thread_arg)
{
    thread_argument *arg = (thread_argument*)thread_arg;
    ErrorCode ret = NO_ERROR;

    int count = arg->try_count;
    ShelfHeap *heap = arg->heap;
    
    assert(count != 0);
    Offset *offset = new Offset[count];
    assert(offset != NULL);
    
    for (int i=0; i<count; i++)
    {
        offset[i] = heap->Alloc(sizeof(int));
        if (heap->IsValidOffset(offset[i]) == false)
        {
            std::cout << "Thread " << arg->id << ": alloc failure (" << ret << ")" << std::endl;            
        }
        else
        {
            int *ptr = (int*)heap->OffsetToPtr(offset[i]);
#ifndef ZONE
        EXPECT_EQ(offset[i], heap->PtrToOffset(ptr));
#endif        
            *ptr = i;
        }
    }

    for (int i=0; i<count; i++)
    {
        if (heap->IsValidOffset(offset[i]) == true)
        {
            int *ptr = (int*)heap->OffsetToPtr(offset[i]);
            EXPECT_EQ(i, *ptr);        
            heap->Free(offset[i]);
        }
    }

    delete [] offset;

    pthread_exit(NULL);
}

TEST(ShelfHeap, MultiThread)
{
    int const kNumThreads = 5;
    int const kNumTry = 10;
    
    ErrorCode ret = NO_ERROR;
    std::string shelf_path = shelf_name.Path(kShelfId);    
    ShelfFile shelf(shelf_path, kShelfId);
    size_t heap_size = kShelfSize;
    ShelfHeap heap(shelf_path, kShelfId);
    
    // create a shelf
    ret = shelf.Create(S_IRUSR | S_IWUSR);
    EXPECT_EQ(NO_ERROR, ret);

    // create a dummy nvheap
    ret = heap.Create(heap_size);
    EXPECT_EQ(NO_ERROR, ret);

    // open the heap
    EXPECT_EQ(NO_ERROR, heap.Open());
    
    pthread_t threads[kNumThreads];
    thread_argument args[kNumThreads];
    int rc=0;
    
    for(int i=0; i<kNumThreads; i++)
    {
        std::cout << "Create worker " << i << std::endl;
        args[i].id = i;
        args[i].try_count = kNumTry;
        args[i].heap = &heap;
        rc = pthread_create(&threads[i], NULL, worker, (void*)&args[i]);
        assert (0 == rc);
    }

    void *status;
    for(int i=0; i<kNumThreads; i++)
    {
        std::cout << "Join worker " << i << std::endl;
        rc = pthread_join(threads[i], &status);
        assert (0 == ret);
    }

    EXPECT_EQ(NO_ERROR, heap.Close());        
        
    // destroy the heap
    ret = heap.Destroy(); 
    EXPECT_EQ(NO_ERROR, rc);
    
    // destroy the shelf
    ret = shelf.Destroy();
    EXPECT_EQ(NO_ERROR, ret);
}

void AllocFree()
{
    std::string shelf_path = shelf_name.Path(kShelfId);    
    ShelfFile shelf(shelf_path, kShelfId);
    ShelfHeap heap(shelf_path, kShelfId);

    // open the heap
    EXPECT_EQ(NO_ERROR, heap.Open());

    int count = 500;
    Offset *offset = new Offset[count];
    assert(offset != NULL);
    for (int i=0; i<count; i++)
    {
        offset[i] = heap.Alloc(sizeof(int));
        if (heap.IsValidOffset(offset[i]) == false)
        {
            std::cout << "Alloc failed" << std::endl;            
        }
        else
        {
            int *ptr = (int*)heap.OffsetToPtr(offset[i]);
#ifndef ZONE
            EXPECT_EQ(offset[i], heap.PtrToOffset(ptr));
#endif        
            *ptr = i;
        }
    }

    for (int i=0; i<count; i++)
    {
        if (heap.IsValidOffset(offset[i]) == true)
        {
            int *ptr = (int*)heap.OffsetToPtr(offset[i]);
            EXPECT_EQ(i, *ptr);        
            heap.Free(offset[i]);
        }
        else
        {
            std::cout << "Invalid pointer?" << std::endl;
        }
    }
    
    delete [] offset;

    // close the heap
    EXPECT_EQ(NO_ERROR, heap.Close());
}

TEST(ShelfHeap, MultiProcess)
{        
    int const process_count = 1;

    ErrorCode ret = NO_ERROR;
    std::string shelf_path = shelf_name.Path(kShelfId);    
    ShelfFile shelf(shelf_path, kShelfId);
    size_t heap_size = kShelfSize;
    ShelfHeap heap(shelf_path, kShelfId);
    
    // create a shelf
    ret = shelf.Create(S_IRUSR | S_IWUSR);
    EXPECT_EQ(NO_ERROR, ret);

    // create a dummy nvheap
    ret = heap.Create(heap_size);
    EXPECT_EQ(NO_ERROR, ret);
        
    pid_t pid[process_count];

    for (int i=0; i< process_count; i++)
    {
        pid[i] = fork();
        ASSERT_LE(0, pid[i]);
        if (pid[i]==0)
        {
            // child
            AllocFree();
            exit(0); // this will leak memory (see valgrind output)
        }
        else
        {
            // parent
            continue;
        }
    }

    for (int i=0; i< process_count; i++)
    {    
        int status;
        waitpid(pid[i], &status, 0);
    }
    
    // destroy the heap
    ret = heap.Destroy(); 
    EXPECT_EQ(NO_ERROR, ret);
    
    // destroy the shelf
    ret = shelf.Destroy();
    EXPECT_EQ(NO_ERROR, ret);
}

int main(int argc, char** argv)
{
    InitTest();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

