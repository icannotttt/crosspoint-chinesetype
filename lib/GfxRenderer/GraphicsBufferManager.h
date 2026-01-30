#include <cstdlib>
#include <cstring>
#include <cstdint>

// ------------ 内存池配置（针对 BW 缓冲块，可根据你的项目调整） ------------
#define BW_CHUNK_SIZE     8000    // 单个 BW 缓冲块大小（对应日志中的 8000 字节）
#define BW_POOL_MAX_CHUNKS 6      // 内存池最大容纳的缓冲块数量（按需调整，避免内存溢出）

// ------------ BW 内存池类（封装，方便调用和管理） ------------
class BwMemoryPool {
public:
    // 构造函数：初始化内存池（预分配所有缓冲块）
    BwMemoryPool() {
        // 1. 初始化空闲标记数组（所有块初始化为「空闲」：true）
        for (int i = 0; i < BW_POOL_MAX_CHUNKS; ++i) {
            is_chunk_free[i] = true;
        }

        // 2. 预分配整个内存池（一次性向系统申请连续内存）
        // 总内存大小 = 块大小 * 块数量
        pool_start = (uint8_t*)malloc(BW_CHUNK_SIZE * BW_POOL_MAX_CHUNKS);

        if (pool_start == nullptr) {
            // 内存池初始化失败（总内存不足），可添加日志打印
            return;
        }

        // 3. 初始化每个缓冲块的起始地址（方便后续快速查找）
        for (int i = 0; i < BW_POOL_MAX_CHUNKS; ++i) {
            chunk_starts[i] = pool_start + (i * BW_CHUNK_SIZE);
        }
    }

    // 析构函数：程序退出时，一次性释放整个内存池
    ~BwMemoryPool() {
        if (pool_start != nullptr) {
            free(pool_start);
            pool_start = nullptr;
        }
    }

    // 核心方法1：从内存池中申请一个 BW 缓冲块（替代 malloc）
    uint8_t* allocateBwChunk() {
        // 1. 检查内存池是否初始化成功
        if (pool_start == nullptr) {
            return nullptr;
        }

        // 2. 查找第一个空闲的缓冲块
        for (int i = 0; i < BW_POOL_MAX_CHUNKS; ++i) {
            if (is_chunk_free[i]) {
                // 3. 标记该块为「已使用」
                is_chunk_free[i] = false;
                // 4. 返回该块的起始地址
                return chunk_starts[i];
            }
        }

        // 5. 没有空闲块（内存池耗尽），返回 nullptr（对应你的原错误）
        return nullptr;
    }

    // 核心方法2：将 BW 缓冲块归还到内存池（替代 free）
    void freeBwChunk(uint8_t* chunk) {
        // 1. 合法性检查（防止传入非法指针）
        if (pool_start == nullptr || chunk == nullptr) {
            return;
        }

        // 2. 计算该块在内存池中的索引
        int chunk_index = (chunk - pool_start) / BW_CHUNK_SIZE;

        // 3. 再次验证索引合法性（防止越界）
        if (chunk_index >= 0 && chunk_index < BW_POOL_MAX_CHUNKS &&
            chunk == chunk_starts[chunk_index]) { // 确保指针是内存池中的有效块
            // 4. 标记该块为「空闲」，供下次复用
            is_chunk_free[chunk_index] = true;
            // 可选：清空缓冲块内容（防止数据残留，对 E-Ink 可省略，减少开销）
            // memset(chunk, 0, BW_CHUNK_SIZE);
        }
    }

    // 辅助方法：获取内存池的空闲块数量（用于调试/监控）
    int getFreeChunkCount() const {
        int count = 0;
        for (int i = 0; i < BW_POOL_MAX_CHUNKS; ++i) {
            if (is_chunk_free[i]) {
                count++;
            }
        }
        return count;
    }

private:
    uint8_t* pool_start = nullptr;                  // 内存池的起始地址（整个池的入口）
    uint8_t* chunk_starts[BW_POOL_MAX_CHUNKS] = {0};// 每个 BW 缓冲块的起始地址数组
    bool is_chunk_free[BW_POOL_MAX_CHUNKS] = {false};// 每个块的空闲状态标记（true=空闲）
};