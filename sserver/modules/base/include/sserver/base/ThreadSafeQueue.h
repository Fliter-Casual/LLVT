// ============================================================
// 文件：ThreadSafeQueue.h
// 模块：sserver::base
// 功能：线程安全队列模板 ThreadSafeQueue：互斥量 + 条件变量 保护双端队列，
//       提供阻塞限时弹出、满员丢最旧、按谓词选择性丢弃等入队策略。
//       它是采集-编码-传输流水线各级之间传递帧的公共通道。
// 知识点：类模板、mutex + condition_variable 生产者消费者模型、
//         wait_for 谓词重载（过滤虚假唤醒）、谓词驱动的淘汰策略、
//         mutable 成员、deque 双端操作
// ============================================================

#ifndef SSERVER_BASE_THREAD_SAFE_QUEUE_H
#define SSERVER_BASE_THREAD_SAFE_QUEUE_H 

#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <cstddef>

namespace sserver {
namespace base { 

// 单互斥量保护 deque，条件变量只表达「队列非空」这一个事实。
//
// 容量上限不存放在队列内部，而是每次入队时由调用方传入：同一条队列在
// 不同阶段可能有不同预算，且「丢最旧」是调用方的策略选择，不是队列的
// 固有属性。容量参数按以下语义解释：
//   maxSize == 0 → 只保留本次入队的新值，既有元素全部丢弃
//   maxSize == n → 入队后队列长度不超过 n
//
// 关闭不是队列的职责：关停时调用 notifyAll() 广播唤醒，等待方醒来看到
// 外部的停止标志就退出。谓词本身不含停止条件，所以这里的等待必须有
// 超时预算，线程才不会在关停后卡死在队列上。
template <typename T> //
class ThreadSafeQueue {
public:
    // ==================== 入队 ====================
    // 无容量限制的入队。需要背压的场合不要用它。
    void push(const T &value) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(value);
        }
        // 解锁之后再唤醒：被唤醒的消费者立刻能拿到锁，不会白醒一次。
        m_conditionVariable.notify_one();
    }

    void pushDropOldest(const T &value, std::size_t maxSize) { // 丢最旧
        pushDropOldestCountDropped(value, maxSize); // 不解释返回值，仅丢帧
    }

    // 入队前先把数量淘汰到低于 maxSize，再插入新值。返回本次淘汰的元素数，
    // 调用方据此累计丢帧统计——丢帧是有意义的数据，不能静默发生。
    std::size_t pushDropOldestCountDropped(const T &value, std::size_t maxSize) {
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!m_queue.empty() && m_queue.size() >= maxSize) {
                m_queue.pop_front();
                ++dropped;
            }
            m_queue.push_back(value);
        }
        m_conditionVariable.notify_one();
        return dropped; // 如果想接收返回值，就调用这个函数
    }

    // 队列满时优先丢弃满足 predicate(谓词) 的元素（典型用法：先丢非关键帧），
    // 一个都找不到才退回丢最旧。关键帧保护就靠这个入口实现：
    // 保护关键帧比保护积压深度更重要，丢了关键帧整条参考链就断了。
    template <typename Predicate> // 谓词:一个返回布尔值（bool）的可调用对象（函数、函数指针或 Lambda 表达式）
    std::size_t pushDropSelective(const T &value, std::size_t maxSize, Predicate predicate) {
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!m_queue.empty() && m_queue.size() >= maxSize) {
                typename std::deque<T>::iterator candidate =
                        std::find_if(m_queue.begin(), m_queue.end(), predicate);
                if (candidate != m_queue.end()) {
                    m_queue.erase(candidate);
                } else {
                    m_queue.pop_front();
                }
                ++dropped;
            }
            m_queue.push_back(value);
        }
        m_conditionVariable.notify_one(); // 解锁之后再唤醒：被唤醒的消费者立刻能拿到锁
        return dropped;
    }

    // ==================== 出队 ====================

    // 限时阻塞弹出。谓词负责过滤虚假唤醒：醒来先检查「队列非空」，
    // 不满足就继续睡，超时到期返回 false。
    bool waitPopFor(T *value, const std::chrono::milliseconds &timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_conditionVariable.wait_for(lock, timeout, [this]() { return !m_queue.empty(); })) {
            return false;
        }
        *value = m_queue.front();
        m_queue.pop_front();
        return true;
    }

    // 非阻塞弹出，供轮询式消费者使用。
    bool tryPop(T *value) {        // 取出消费
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        *value = m_queue.front();
        m_queue.pop_front();
        return true;
    }

    // ==================== 唤醒与清空 ====================

    // 关停用：广播唤醒所有等待者，配合外部停止标志退出等待循环。
    // 通知不改变谓词，空队列上的 waitPopFor 仍会等到超时。
    void notifyAll() {
        m_conditionVariable.notify_all();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }

    // ==================== 状态查询 ====================

    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    // 队列中是否存在 predicate(谓词) 匹配的元素
    template <typename Predicate>
    bool anyMatching(Predicate predicate) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::find_if(m_queue.begin(), m_queue.end(), predicate) != m_queue.end();
    } // find_if判断逻辑: 遍历帧队列，再加入一个以帧对象为参数的谓词predicate，谓词通常是用来判断这个帧是否是I帧P帧之类的

private:
    // const 成员函数也要加锁，mutex 因此必须 mutable。
    mutable std::mutex m_mutex;
    std::condition_variable m_conditionVariable; // 条件变量,必须配合互斥量(mutex)去使用;调用wait时绑定锁
    std::deque<T> m_queue;
};

} // namespace base
} // namespace sserver

#endif