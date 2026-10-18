/*!
 * \file WTSObject.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief Wt基础Object定义
 */
#pragma once
#include <stdint.h>
#include <atomic>
#include <memory>
#include <boost/smart_ptr/detail/spinlock.hpp>

#include "WTSMarcos.h"
#include "../Share/ObjectPool.hpp"
#include "../Share/SpinMutex.hpp"

NS_WTP_BEGIN
class WTSObject
{
public:
	WTSObject() :m_uRefs(1){}
	virtual ~WTSObject(){}

public:
	inline uint32_t		retain(){ return m_uRefs.fetch_add(1) + 1; }

	virtual void	release()
	{
		if (m_uRefs == 0)
			return;

		try
		{
			uint32_t cnt = m_uRefs.fetch_sub(1);
			if (cnt == 1)
			{
				delete this;
			}
		}
		catch(...)
		{

		}
	}

	inline bool			isSingleRefs() { return m_uRefs == 1; }

	inline uint32_t		retainCount() { return m_uRefs; }

protected:
	volatile std::atomic<uint32_t>	m_uRefs;
};

// Objects retained by another thread must outlive their allocating thread.
template<typename T>
class WTSPoolObject : public WTSObject
{
private:
	typedef ObjectPool<T> MyPool;
	struct PoolState
	{
		MyPool pool;
		SpinMutex mutex;
	};
	std::shared_ptr<PoolState> _pool;

public:
	WTSPoolObject() = default;
	virtual ~WTSPoolObject() {}

	static T* allocate()
	{
		thread_local static std::shared_ptr<PoolState> pool(new PoolState);
		SpinLock lock(pool->mutex);
		T* ret = pool->pool.construct();
		if (ret)
			ret->_pool = pool;
		return ret;
	}

	virtual void release() override
	{
		if (m_uRefs.fetch_sub(1) == 1)
		{
			// Keep both pool and lock alive through this object's destruction.
			auto pool = _pool;
			SpinLock lock(pool->mutex);
			pool->pool.destroy(static_cast<T*>(this));
		}
	}
};
NS_WTP_END
