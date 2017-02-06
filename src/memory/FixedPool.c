/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "swoole.h"

static void swFixedPool_init(swFixedPool *object);
static void* swFixedPool_alloc(swMemoryPool *pool, uint32_t size);
static void swFixedPool_free(swMemoryPool *pool, void *ptr);
static void swFixedPool_destroy(swMemoryPool *pool);

void swFixedPool_debug_slice(swFixedPool_slice *slice);

/**
 * create new FixedPool, random alloc/free fixed size memory
 */
/**
 * 创建一个新的 FixedPool 内存池，随机分配或者释放一块固定大小的内存
 * @param slice_num     内存块节点数量
 * @param slice_size    每个内存块的大小，不包括内存块结构体实例的大小。
 * @param shared        是否共享内存，1是共享
 * @return 
 */
swMemoryPool* swFixedPool_new(uint32_t slice_num, uint32_t slice_size, uint8_t shared)
{
    size_t size = slice_size * slice_num + slice_num * sizeof(swFixedPool_slice);
    size_t alloc_size = size + sizeof(swFixedPool) + sizeof(swMemoryPool);  //分配内存的大小包括：所有内存块节点大小 + 所有内存块共享内存结构体实例大小 + swFixedPool内存池大小（子类） + swMemoryPool内存池大小(基类)
    void *memory = (shared == 1) ? sw_shm_malloc(alloc_size) : sw_malloc(alloc_size);

    swFixedPool *object = memory;
    memory += sizeof(swFixedPool);//整块内存的第一部分是swFixedPool结构体变量，第二部分是swMemoryPool结构体变量
    bzero(object, sizeof(swFixedPool));//将object指向的内存池开始的 swFixedPool结构体实例 大小的空间初始化为0

    object->shared = shared;
    object->slice_num = slice_num;
    object->slice_size = slice_size;
    object->size = size;

    swMemoryPool *pool = memory;
    memory += sizeof(swMemoryPool);//整块内存的第一部分是swFixedPool结构体变量，第二部分是swMemoryPool结构体变量
    pool->object = object;//swMemoryPool结构体变量的object属性指针指向了swFixedPool结构体变量的开始位置
    //三个函数指针
    pool->alloc = swFixedPool_alloc;
    pool->free = swFixedPool_free;
    pool->destroy = swFixedPool_destroy;

    object->memory = memory;//wFixedPool结构体变量的memory属性指向了后面即将初始化的swFixedPool_slice双向链表的尾部

    /**
     * init linked list
     */
    swFixedPool_init(object);//初始化swFixedPool_slice双向链表

    return pool;
}

/**
 * create new FixedPool, Using the given memory
 */
swMemoryPool* swFixedPool_new2(uint32_t slice_size, void *memory, size_t size)
{
    swFixedPool *object = memory;
    memory += sizeof(swFixedPool);
    bzero(object, sizeof(swFixedPool));

    object->slice_size = slice_size;
    object->size = size - sizeof(swMemoryPool) - sizeof(swFixedPool);
    object->slice_num = object->size / (slice_size + sizeof(swFixedPool_slice));

    swMemoryPool *pool = memory;
    memory += sizeof(swMemoryPool);
    bzero(pool, sizeof(swMemoryPool));

    pool->object = object;
    pool->alloc = swFixedPool_alloc;
    pool->free = swFixedPool_free;
    pool->destroy = swFixedPool_destroy;

    object->memory = memory;

    /**
     * init linked list
     */
    swFixedPool_init(object);

    return pool;
}

/**
 * linked list
 */
/**
 * swFixedPool内存池的swFixedPool_slice双向链表初始化
 * @param object swFixedPool结构体指针变量
 * @description 内存池整体是由一个swFixedPool结构体变量 + swMemoryPool结构体变量 + 一组swFixedPool_slice内存块节点结构体变量组成的双向链表 所维护的内存组成。循环从内存小的地址开始作为内存块的末尾节点，一个一个生成前面的节点内存地址越来越大。
 * 整个内存池内存里（从左到右内存地址越来越大）的结构是      |swFixedPool结构体变量|swMemoryPool结构体变量|swFixedPool_slice末尾节点|... swFixedPool_slice中间n节点 ...|swFixedPool_slice开始节点|
 */
static void swFixedPool_init(swFixedPool *object)
{
    swFixedPool_slice *slice;
    void *cur = object->memory;//当前指针位置为内存块的开始位置
    void *max = object->memory + object->size;//最大的指针位置是内存池的结尾位置=开始地址+内存池大小
    do
    {
        slice = (swFixedPool_slice *)   cur;
        bzero(slice, sizeof(swFixedPool_slice));

        if (object->head != NULL)
        {
            object->head->pre = slice;//这行代码是不是可以去掉，重复操作了，这一步在后面的“slice->pre = (swFixedPool_slice *) cur;”已经做了
            slice->next = object->head;
        }
        else
        {
            object->tail = slice;//第一次循环走到这里，swFixedPool结构体变量的tail指向swFixedPool内存池的开始位置
        }

        object->head = slice;
        cur += (sizeof(swFixedPool_slice) + object->slice_size);//每个swFixedPool_slice内存块占据的内存包括swFixedPool_slice结构体变量的大小+实际内存块数据存储占据的空间大小

        if (cur < max)
        {
            slice->pre = (swFixedPool_slice *) cur;
        }
        else
        {
            slice->pre = NULL;
            break;
        }

    } while (1);
}

/**
 * 从swFixedPool_slice内存块双向链表中头部分配一个空的内存块节点，并移动到链表的末尾。因此如果头部节点是已占用，则整个内存池已经用完。
 * @param pool  内存池swMemoryPool基类指针变量
 * @param size  实际没有用到，为了符合函数声明加上的
 * @return  成功则返回了分配的swFixedPool_slice内存块节点的实际存储数据位置的地址
 */
static void* swFixedPool_alloc(swMemoryPool *pool, uint32_t size)
{
    swFixedPool *object = pool->object;
    swFixedPool_slice *slice;

    slice = object->head;

    if (slice->lock == 0)
    {
        slice->lock = 1;
        object->slice_use ++;
        /**
         * move next slice to head (idle list)
         */
        //移动当前头部节点的下一个节点作为头部节点
        object->head = slice->next;
        slice->next->pre = NULL;

        /*
         * move this slice to tail (busy list)
         */
        //将当前头部节点移到链表尾部
        object->tail->next = slice;
        slice->next = NULL;
        slice->pre = object->tail;
        object->tail = slice;

        return slice->data;//返回了分配的swFixedPool_slice内存块节点的实际存储数据的位置的地址
    }
    else
    {
        return NULL;
    }
}

/**
 * 释放内存块节点的占用，将该内存块移到链表的头部
 * @param pool  内存池指针变量
 * @param ptr   需要释放的内存块实际存储数据的开始地址（swFixedPool_alloc返回的地址）
 */
static void swFixedPool_free(swMemoryPool *pool, void *ptr)
{
    swFixedPool *object = pool->object;
    swFixedPool_slice *slice;
    //assert断言 ptr指针肯定是在整个swFixedPool_slice内存块双向链表中间。如果不在这之间则报错，程序终止abert
    assert(ptr > object->memory && ptr < object->memory + object->size);

    //ptr是指向的内存块存储数据的开始位置，因此减去swFixedPool_slice结构体变量大小，正好是该内存块的结构体变量起始位置。
    slice = ptr - sizeof(swFixedPool_slice);

    //如果内存块已经占用，则解锁
    if (slice->lock)
    {
        object->slice_use--;//内存块使用数减一
    }

    slice->lock = 0;

    //list head, AB
    //如果该内存块是位于链表头部位置，则不做任何操作，直接返回
    if (slice->pre == NULL)
    {
        return;
    }
    //list tail, DE
    //如果该内存块是在链表尾部，则直接移到头部，原来的该内存块的前一个变成了链表的尾部
    if (slice->next == NULL)
    {
        slice->pre->next = NULL;
        object->tail = slice->pre;
    }
    //middle BCD
    //如果该内存块是链表中间，则把该内存块的前后块连起来，该内存块移到链表开头
    else
    {
        slice->pre->next = slice->next;
        slice->next->pre = slice->pre;
    }

    slice->pre = NULL;
    slice->next = object->head;
    object->head->pre = slice;
    object->head = slice;
}

/**
 * 释放整个内存池的内存
 * @param pool  swMemoryPool内存池的对象指针
 */
static void swFixedPool_destroy(swMemoryPool *pool)
{
    swFixedPool *object = pool->object;
    if (object->shared)
    {
        sw_shm_free(object);//释放object指向的共享内存
    }
    else
    {
        sw_free(object);//free（） 释放 object指向的内存（整个内存池malloc的内存：包括swMemoryPool+swFixedPool+内存块链表）。并量object指针指向NULL
    }
}

void swFixedPool_debug(swMemoryPool *pool)
{
    int line = 0;
    swFixedPool *object = pool->object;
    swFixedPool_slice *slice = object->head;

    printf("===============================%s=================================\n", __FUNCTION__);
    while (slice != NULL)
    {
        if (slice->next == slice)
        {
            printf("-------------------@@@@@@@@@@@@@@@@@@@@@@----------------\n");

        }
        printf("#%d\t", line);
        swFixedPool_debug_slice(slice);

        slice = slice->next;
        line++;
        if (line > 100)
            break;
    }
}

void swFixedPool_debug_slice(swFixedPool_slice *slice)
{
    printf("Slab[%p]\t", slice);
    printf("pre=%p\t", slice->pre);
    printf("next=%p\t", slice->next);
    printf("tag=%d\t", slice->lock);
    printf("data=%p\n", slice->data);
}
