#pragma once

#include "defines.h"

#include <cassert>
#include <deque>
#include <set>

namespace tokoro::internal
{

template <typename T>
class TimeQueue
{
private:
    // Zero-delay items (fast path: O(1) insert, O(1) pop)
    struct ZeroNode
    {
        T    value;
        bool cancelled = false;
    };

    // Timed items (slow path: multiset for sorted order)
    struct TimedNode
    {
        double   time;
        uint32_t seq;
        uint32_t frame;
        T        value;
    };

    struct TimedComp
    {
        bool operator()(const TimedNode& a, const TimedNode& b) const noexcept
        {
            if (a.time != b.time)
                return a.time < b.time;
            else
                return a.seq < b.seq;
        }
    };

    using TimedSetType = std::multiset<TimedNode, TimedComp>;

public:
    struct Iterator
    {
        enum class Type : uint8_t { Zero, Timed } type = Type::Zero;
        ZeroNode*                             zeroPtr   = nullptr;
        typename TimedSetType::const_iterator timedIter = {};
    };

    TimeQueue()
    {
        mTimedUpdatePtr = mTimedSet.end();
    }

    void Clear()
    {
        mZeroPending.clear();
        mZeroReady.clear();
        mZeroReadyIdx = 0;
        mPhase        = Phase::Zero;

        mTimedSet.clear();
        mTimedUpdatePtr = mTimedSet.end();
        mAddOrder       = 0;
        mAddFrame       = 0;
        mCurExeTime     = 0;
    }

    Iterator AddTimed(const double time, const T& e)
    {
        if (time == 0.0)
        {
            mZeroPending.push_back(ZeroNode{e, false});
            Iterator it;
            it.type    = Iterator::Type::Zero;
            it.zeroPtr = &mZeroPending.back();
            return it;
        }
        else
        {
            TimedNode node{time, mAddOrder++, mAddFrame, e};
            auto setIt = mTimedSet.insert(std::move(node));
            Iterator it;
            it.type      = Iterator::Type::Timed;
            it.timedIter = setIt;
            return it;
        }
    }

    void Remove(Iterator iter)
    {
        if (iter.type == Iterator::Type::Zero)
        {
            iter.zeroPtr->cancelled = true;
        }
        else
        {
            if (iter.timedIter == mTimedUpdatePtr)
            {
                mTimedUpdatePtr = mTimedSet.erase(mTimedUpdatePtr);
            }
            else
            {
                mTimedSet.erase(iter.timedIter);
            }
        }
    }

    T Pop()
    {
        if (mPhase == Phase::Zero)
        {
            assert(mZeroReadyIdx < mZeroReady.size());
            T ret = std::move(mZeroReady[mZeroReadyIdx].value);
            mZeroReadyIdx++;
            return ret;
        }
        else
        {
            assert(mTimedUpdatePtr != mTimedSet.end());
            T ret = std::move(mTimedUpdatePtr->value);
            mTimedUpdatePtr = mTimedSet.erase(mTimedUpdatePtr);
            return ret;
        }
    }

    bool CheckUpdate() noexcept
    {
        if (mPhase == Phase::Zero)
        {
            while (mZeroReadyIdx < mZeroReady.size())
            {
                if (!mZeroReady[mZeroReadyIdx].cancelled)
                    return true;
                mZeroReadyIdx++;
            }
            // Zero-delay items exhausted, switch to timed phase
            mPhase = Phase::Timed;
        }

        MoveToNext();
        return !mTimedSet.empty() && mTimedSet.end() != mTimedUpdatePtr;
    }

    void SetupUpdate(double exeTime)
    {
        // Zero-delay: swap pending → ready
        mZeroReady.clear();
        mZeroReady.swap(mZeroPending);
        mZeroReadyIdx = 0;
        mPhase        = Phase::Zero;

        // Timed: same as original
        mAddFrame++;
        mAddOrder       = 0;
        mTimedUpdatePtr = mTimedSet.begin();
        mCurExeTime     = exeTime;
    }

private:
    enum class Phase { Zero, Timed };

    void MoveToNext()
    {
        while (mTimedUpdatePtr != mTimedSet.end())
        {
            const TimedNode& node = *mTimedUpdatePtr;

            if (node.time > mCurExeTime)
            {
                mTimedUpdatePtr = mTimedSet.end();
                break;
            }

            if (node.frame == mAddFrame)
            {
                ++mTimedUpdatePtr;
            }
            else
            {
                break;
            }
        }
    }

    // Zero-delay fast path (deque for pointer stability on push_back)
    std::deque<ZeroNode> mZeroPending;
    std::deque<ZeroNode> mZeroReady;
    size_t               mZeroReadyIdx = 0;
    Phase                mPhase        = Phase::Zero;

    // Timed slow path (unchanged from original)
    TimedSetType                          mTimedSet;
    uint32_t                              mAddOrder = 0;
    uint32_t                              mAddFrame = 0;
    typename TimedSetType::const_iterator mTimedUpdatePtr;
    double                                mCurExeTime = 0;
};

} // namespace tokoro::internal
