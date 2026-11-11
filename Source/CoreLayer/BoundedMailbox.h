#pragma once
#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

namespace se
{
enum class MailboxResult {Accepted,Closed,Full,TooLarge};
struct MailboxStatistics
{
    std::size_t pending=0,bytes=0,queued=0,peakPending=0,peakBytes=0,rejected=0;
    bool closed=false;
};
// 数量/字节预算包括已取出但仍在处理的 Lease。发布端只弱引用队列，不能延长业务所有者寿命。
template<class T> class BoundedMailbox
{
    static_assert(std::is_nothrow_move_constructible_v<T>&&std::is_nothrow_move_assignable_v<T>);
    struct Entry {T value;std::size_t bytes;};
    struct State
    {
        std::mutex mutex;std::deque<Entry> queue;MailboxStatistics statistics;
        std::size_t capacity,maxBytes;
        State(std::size_t count,std::size_t bytes):capacity(count),maxBytes(bytes){}
    };
public:
    class Publisher
    {
    public:
        Publisher()=default;
        MailboxResult Push(T value,std::size_t bytes)const
        {
            const auto state=state_.lock();if(!state)return MailboxResult::Closed;
            std::lock_guard lock(state->mutex);auto& statistics=state->statistics;
            if(statistics.closed)return MailboxResult::Closed;
            if(bytes>state->maxBytes){++statistics.rejected;return MailboxResult::TooLarge;}
            if(statistics.pending>=state->capacity||bytes>state->maxBytes-statistics.bytes)
            {++statistics.rejected;return MailboxResult::Full;}
            state->queue.push_back({std::move(value),bytes});++statistics.pending;statistics.bytes+=bytes;
            statistics.peakPending=(std::max)(statistics.peakPending,statistics.pending);statistics.peakBytes=(std::max)(statistics.peakBytes,statistics.bytes);
            return MailboxResult::Accepted;
        }
    private:
        friend class BoundedMailbox;
        explicit Publisher(std::weak_ptr<State> state):state_(std::move(state)){}
        std::weak_ptr<State> state_;
    };
    class Lease
    {
    public:
        Lease()=default;
        Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
        Lease(Lease&& other)noexcept:state_(std::move(other.state_)),value_(std::move(other.value_)),bytes_(other.bytes_){other.value_.reset();}
        Lease& operator=(Lease&& other)noexcept
        {if(this!=&other){Reset();state_=std::move(other.state_);value_=std::move(other.value_);bytes_=other.bytes_;other.value_.reset();}return *this;}
        ~Lease(){Reset();}
        const T& Get()const{return *value_;}
        void Reset()noexcept
        {
            value_.reset();if(!state_)return;
            {std::lock_guard lock(state_->mutex);--state_->statistics.pending;state_->statistics.bytes-=bytes_;}
            state_.reset();
        }
    private:
        friend class BoundedMailbox;
        Lease(std::shared_ptr<State> state,T value,std::size_t bytes):state_(std::move(state)),value_(std::move(value)),bytes_(bytes){}
        std::shared_ptr<State> state_;std::optional<T> value_;std::size_t bytes_=0;
    };
    BoundedMailbox(std::size_t count,std::size_t bytes):state_(std::make_shared<State>(count,bytes)){}
    BoundedMailbox(const BoundedMailbox&)=delete;BoundedMailbox& operator=(const BoundedMailbox&)=delete;
    ~BoundedMailbox(){Close();}
    Publisher GetPublisher()const{return Publisher{state_};}
    template<class Predicate> std::vector<Lease> TakeIf(Predicate predicate)
    {
        std::vector<Lease> result;
        {
            std::lock_guard lock(state_->mutex);result.reserve(state_->queue.size());
            for(auto it=state_->queue.begin();it!=state_->queue.end();)
            {
                if(!predicate(it->value)){++it;continue;}
                result.push_back(Lease{state_,std::move(it->value),it->bytes});it=state_->queue.erase(it);
            }
        }
        return result;
    }
    MailboxStatistics Statistics()const
    {std::lock_guard lock(state_->mutex);auto result=state_->statistics;result.queued=state_->queue.size();return result;}
    void Close()noexcept
    {
        std::deque<Entry> discarded;
        {
            std::lock_guard lock(state_->mutex);state_->statistics.closed=true;discarded.swap(state_->queue);
            for(const auto& entry:discarded){--state_->statistics.pending;state_->statistics.bytes-=entry.bytes;}
        }
    }
private:
    std::shared_ptr<State> state_;
};
}
