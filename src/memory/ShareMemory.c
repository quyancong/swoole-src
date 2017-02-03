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
#include <sys/shm.h>

void* sw_shm_malloc(size_t size)
{
    swShareMemory object;
    void *mem;
    //object对象需要保存在头部
    size += sizeof(swShareMemory);
    mem = swShareMemory_mmap_create(&object, size, NULL);
    if (mem == NULL)
    {
        return NULL;
    }
    else
    {
        memcpy(mem, &object, sizeof(swShareMemory));
        return mem + sizeof(swShareMemory);
    }
}

void* sw_shm_calloc(size_t num, size_t _size)
{
    swShareMemory object;
    void *mem;
    void *ret_mem;
    //object对象需要保存在头部
    int size = sizeof(swShareMemory) + (num * _size);
    mem = swShareMemory_mmap_create(&object, size, NULL);
    if (mem == NULL)
    {
        return NULL;
    }
    else
    {
        memcpy(mem, &object, sizeof(swShareMemory));
        ret_mem = mem + sizeof(swShareMemory);
        //calloc需要初始化
        bzero(ret_mem, size - sizeof(swShareMemory));
        return ret_mem;
    }
}

void sw_shm_free(void *ptr)
{
    //object对象在头部，如果释放了错误的对象可能会发生段错误
    swShareMemory *object = ptr - sizeof(swShareMemory);
#ifdef SW_DEBUG
    char check = *(char *)(ptr + object->size); //尝试访问
    swTrace("check: %c", check);
#endif
    swShareMemory_mmap_free(object);
}

void* sw_shm_realloc(void *ptr, size_t new_size)
{
    swShareMemory *object = ptr - sizeof(swShareMemory);
#ifdef SW_DEBUG
    char check = *(char *)(ptr + object->size); //尝试访问
    swTrace("check: %c", check);
#endif
    void *new_ptr;
    new_ptr = sw_shm_malloc(new_size);
    if(new_ptr==NULL)
    {
        return NULL;
    }
    else
    {
        memcpy(new_ptr, ptr, object->size);
        sw_shm_free(ptr);
        return new_ptr;
    }
}

/**
 * mmap方式创建内存映射方式共享内存
 * @param object    swShareMemory结构体变量指针
 * @param size      要创建的内存映射的大小
 * @param mapfile   内存要映射的文件的名字
 * @return 
 */
void *swShareMemory_mmap_create(swShareMemory *object, int size, char *mapfile)
{
    void *mem;  //指向任意变量类型的内存指针
    int tmpfd = -1; //默认赋值-1.open函数，若所有欲核查的权限都通过了检查则返回 0 值，表示成功，只要有一个权限被禁止则返回-1
    int flag = MAP_SHARED;  //mmap函数的 flag参数。MAP_SHARED 对映射区域的写入数据会复制回文件内，而且允许其他映射该文件的进程共享。
    bzero(object, sizeof(swShareMemory)); //void bzero(void *s,int n);将参数s 所指的内存区域前n 个字节，全部设为零值。相当于调用memset(（void *）s,0,size_t n);推荐使用memset替代bzero。

#ifdef MAP_ANONYMOUS
    flag |= MAP_ANONYMOUS;  //flag = (flag | MAP_ANONYMOUS)   flag和 MAP_ANONYMOUS 进行二级制或运算 ???
#else
    //如果没有指定要映射的文件，则使用/dev/zero 文件。 /dev/zero和 /dev/null 是linux的两个特殊文件，不会保存写入内容
    if (mapfile == NULL)
    {
        mapfile = "/dev/zero";
    }
    //读写方式打开要映射的文件，如果打开失败则 return NULL
    if ((tmpfd = open(mapfile, O_RDWR)) < 0)
    {
        return NULL;
    }
    //将要映射的文件名赋值给swShareMemory结构体。c语言的字符串复制需要用strcpy和strncpy等，不能直接用=赋值
    strncpy(object->mapfile, mapfile, SW_SHM_MMAP_FILE_LEN);
    //要映射的文件句柄赋值给swShareMemory结构体
    object->tmpfd = tmpfd;
#endif

//SW_USE_HUGEPAGE 在config.m4文件出现，应该是判断是否开启大内存页(默认内存一页是4KB，大页内存一页是2MB)
//判断是否定义了MAP_HUGETLB(since Linux 2.6.32)，它作为mmap函数的参数，使用该参数可以在内存映射时开启大内存页，有利于提高TLB(Translation Lookaside Buffer)命中率，提高虚拟地址到物理地址的转换效率http://www.laruence.com/2015/10/02/3069.html
#if defined(SW_USE_HUGEPAGE) && defined(MAP_HUGETLB)
    //如果创建的内存映射大于2M则开启大内存页，因为大内存页正好一页2M
    if (size > 2 * 1024 * 1024)
    {
        flag |= MAP_HUGETLB;
    }
#endif
    //建立内存映射若映射成功则返回映射区的内存起始地址，否则返回MAP_FAILED（－1）。
    mem = mmap(NULL, size, PROT_READ | PROT_WRITE, flag, tmpfd, 0);
#ifdef MAP_FAILED
    if (mem == MAP_FAILED)
#else
    if (!mem)
#endif
    {   //建立内存映射失败则输出错误内容和错误号
        swWarn("mmap() failed. Error: %s[%d]", strerror(errno), errno);//swWarn定义未深入研究???
        return NULL;
    }
    else
    {   //建立内存映射成功则给swShareMemory结构体赋值映射区内存大小和映射区内存起始地址
        object->size = size;
        object->mem = mem;
        return mem; //当前函数返回创建映射区的内存起始地址
    }
}
/**
 * 解除mmap内存映射
 * @param object  要解除内存映射的swShareMemory结构体变量指针
 * @return 
 */
int swShareMemory_mmap_free(swShareMemory *object)
{
    //int munmap(void *start,size_t length); 成功返回0，失败返回-1
    return munmap(object->mem, object->size);
}

/**
 * sysv系统v方式创建共享内存（基于shm）
 * @param object    swShareMemory结构体变量指针(mmap和sysv 共用了同一个结构体)
 * @param size      新建的共享内存大小，以字节为单位
 * @param key       一般是通过 ftok()获得这个key（大于0的32位整数）。如果key等于0 即key等于IPC_PRIVATE，则只用于有亲缘关系进程共享内存。
 * @return   成功返回连接好的共享内存地址。失败返回NULL
 */
void *swShareMemory_sysv_create(swShareMemory *object, int size, int key)
{
    int shmid;
    void *mem;
    bzero(object, sizeof(swShareMemory));//void bzero(void *s,int n);将参数s 所指的内存区域前n 个字节，全部设为零值。相当于调用memset(（void *）s,0,size_t n);推荐使用memset替代bzero。

    if (key == 0)//有亲缘关系进程使用
    {
        key = IPC_PRIVATE;
    }
    //SHM_R | SHM_W |
    if ((shmid = shmget(key, size, IPC_CREAT)) < 0)   //得到一个共享内存标识符或创建一个共享内存对象并返回共享内存标识符
    {
        swWarn("shmget() failed. Error: %s[%d]", strerror(errno), errno);
        return NULL;
    }
    //void *shmat(int shmid, const void *shmaddr, int shmflg)。   shmid 共享内存标识符; shmaddr 指定共享内存出现在进程内存地址的什么位置，直接指定为NULL让内核自己决定一个合适的地址位置; shmflg SHM_RDONLY：为只读模式，默认0：可读可写
    if ((mem = shmat(shmid, NULL, 0)) < 0) //连接共享内存标识符为shmid的共享内存，连接成功后把共享内存区对象映射到调用进程的地址空间，随后可像本地空间一样访问.
    {
        swWarn("shmat() failed. Error: %s[%d]", strerror(errno), errno);
        return NULL;
    }
    else
    {
        object->key = key;
        object->shmid = shmid;
        object->size = size;
        object->mem = mem;
        return mem;
    }
}

/**
 * 断开sysv共享内存连接（可指定删除这块共享内存）
 * @param object  swShareMemory结构体变量指针(mmap和sysv 共用了同一个结构体)
 * @param rm 是否删除共享内存，1则删除
 * @return  成功0，失败-1
 */
int swShareMemory_sysv_free(swShareMemory *object, int rm)
{
    int shmid = object->shmid;
    int ret = shmdt(object->mem);//将先前用shmat函数连接（attach）好的共享内存脱离（detach）目前的进程
    if (rm == 1)
    {
        shmctl(shmid, IPC_RMID, NULL);//通过共享内存标识符控制共享内存。第二个参数IPC_RMID：删除这片共享内存。
    }
    return ret;//成功0，失败-1
}
