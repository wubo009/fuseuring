// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber
#pragma once
#include <coroutine>
#include <vector>
#include <liburing.h>
#include <utility>
#include <assert.h>
#include <iostream>
#include <unistd.h>
#include <memory>

#define DBG_PRINT(x)

/*
//for clang and libc++
namespace std
{
    template<typename T = void>
    using coroutine_handle = std::experimental::coroutine_handle<T>;

    using suspend_never = std::experimental::suspend_never;

    using suspend_always = std::experimental::suspend_always;
}
*/

static uint64_t handle_v(std::coroutine_handle<> p_awaiter)
{
    return (uint64_t)*((uint64_t*)&p_awaiter);
}

struct fuse_io_context
{
    template<typename T>
    struct IoUringAwaiter
    {
        struct IoUringAwaiterGlobalRes
        {
            std::coroutine_handle<> awaiter;
            uint8_t tocomplete;
        };

        struct IoUringAwaiterRes
        {
            IoUringAwaiterRes() noexcept
            : res(-1) {}

            int res;
            IoUringAwaiterGlobalRes* gres;
        };

        IoUringAwaiter(std::vector<io_uring_sqe*> sqes) noexcept
        {
            awaiter_res.resize(sqes.size());
            global_res.tocomplete = sqes.size();
            for(size_t i=0;i<sqes.size();++i)
            {
                awaiter_res[i].gres = &global_res;
                sqes[i]->user_data = reinterpret_cast<uint64_t>(&awaiter_res[i]);
            }
        }        

        IoUringAwaiter(IoUringAwaiter const&) = delete;
	    IoUringAwaiter(IoUringAwaiter&& other) = delete;
	    IoUringAwaiter& operator=(IoUringAwaiter&&) = delete;
	    IoUringAwaiter& operator=(IoUringAwaiter const&) = delete;

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> p_awaiter) noexcept
        {
            DBG_PRINT(std::cout << "Await suspend io "<< handle_v(p_awaiter) << std::endl);
            global_res.awaiter = p_awaiter;           
        }

        template<typename U = T, std::enable_if_t<std::is_same<U, std::vector<io_uring_sqe*> >::value, int> = 0>
        std::vector<int> await_resume() const noexcept
        {
            std::vector<int> res;
            for(auto& sr: awaiter_res)
            {
                res.push_back(sr.res);
            }
            return res;
        }

        template<typename U = T, std::enable_if_t<std::is_same<U, io_uring_sqe*>::value, int> = 0>
        int await_resume() const noexcept
        {
            return awaiter_res[0].res;
        }

        template<typename U = T, std::enable_if_t<std::is_same<U, std::pair<io_uring_sqe*, io_uring_sqe*> >::value, int> = 0>
        std::pair<int, int> await_resume() const noexcept
        {
            return std::make_pair(awaiter_res[0].res, awaiter_res[1].res);
        }

    private:
        IoUringAwaiterGlobalRes global_res;
        std::vector<IoUringAwaiterRes> awaiter_res;         
    };

    [[nodiscard]] auto complete(std::vector<io_uring_sqe*> sqes)
    {
        return IoUringAwaiter<std::vector<io_uring_sqe*> >(sqes);
    }

    [[nodiscard]] auto complete(io_uring_sqe* sqe)
    {
        return IoUringAwaiter<io_uring_sqe*>({sqe});
    }

    [[nodiscard]] auto complete(std::pair<io_uring_sqe*, io_uring_sqe*> sqes)
    {
        return IoUringAwaiter<std::pair<io_uring_sqe*, io_uring_sqe*> >({sqes.first, sqes.second});
    }

    io_uring_sqe* get_sqe(unsigned int peek=1) noexcept
    {
        if(peek>1)
        {
            while(true)
            {
                struct io_uring_sq *sq = &fuse_ring.ring->sq;
                unsigned int head = io_uring_smp_load_acquire(sq->khead);        
                unsigned int tail = sq->sqe_tail;
                struct io_uring_sqe *__sqe = NULL;

                if (tail - head < *sq->kring_entries &&
                    *sq->kring_entries - (tail - head) >= peek)
                {
                    io_uring_sqe* sqe = &sq->sqes[sq->sqe_tail & *sq->kring_mask];
		            sq->sqe_tail = tail + 1;
                    return sqe;
                }

                int rc = io_uring_submit(fuse_ring.ring);
                if(rc<0 && errno!=EBUSY)
                {
                    perror("io_uring_submit failed in get_sqe");
                    return nullptr;
                }
                else if(rc<0)
                {
                    std::cout << "io_uring_submit: EBUSY" << std::endl;
                    sleep(0);
                }                
            }
        }

        fuse_ring.ring_submit=true;
        auto ret = io_uring_get_sqe(fuse_ring.ring);
        if(ret==nullptr)
        {
            /* Needs newer Linux 5.10
            int rc = io_uring_sqring_wait(fuse_ring.ring);
            if(rc<0)
            {
                return nullptr;
            }
            else if(rc==0)
            {*/
                int rc = io_uring_submit(fuse_ring.ring);
                if(rc<0)
                {
                    perror("io_uring_submit failed in get_sqe");
                    return nullptr;
                }

                do
                { 
                    ret = io_uring_get_sqe(fuse_ring.ring);
                } while (ret==nullptr);
            //}
        }
        return ret;
    }

    struct MallocItem
    {
        MallocItem* next;
    };

    thread_local static MallocItem* malloc_cache_head;
    static constexpr size_t malloc_cache_item_size = 500;

    static void clear_malloc_cache()
    {
        while(malloc_cache_head)
        {
            MallocItem* next = malloc_cache_head->next;
            delete []reinterpret_cast<char*>(malloc_cache_head);
            malloc_cache_head = next;
        }
    }

    static void* malloc_cache()
    {
        if(malloc_cache_head!=nullptr)
        {
            MallocItem* ret = malloc_cache_head;
            malloc_cache_head = malloc_cache_head->next;
            return ret;
        }

        return new char[malloc_cache_item_size];
    }

    static void malloc_cache_free(void* data)
    {
        MallocItem* mi = reinterpret_cast<MallocItem*>(data);
        mi->next = malloc_cache_head;
        malloc_cache_head = mi;
    }

    template<typename T>
    struct io_uring_promise_type_base
    {
        using promise_type = io_uring_promise_type_base<T>;
        using handle = std::coroutine_handle<promise_type>;

        enum class e_res_state
        {
            Init,
            Detached,
            Res
        };

        io_uring_promise_type_base() 
            : res_state(e_res_state::Init) {}

        auto initial_suspend() { 
            return std::suspend_never{}; 
        }
    
        auto final_suspend() noexcept { 
            struct final_awaiter  : std::suspend_always
            {
                final_awaiter(promise_type* promise)
                    :promise(promise) {}

                void await_suspend(std::coroutine_handle<> p_awaiter) const noexcept
                {
                    if(promise->res_state==e_res_state::Detached)
                    {
                        DBG_PRINT(std::cout << "promise final detached" << std::endl);
                        if(promise->awaiter)
                            promise->awaiter.destroy();
                        handle::from_promise(*promise).destroy();
                    }
                    else if(promise->awaiter)
                    {
                        DBG_PRINT(std::cout << "promise final await resume" << std::endl);
                        promise->awaiter.resume();
                    }
                    else
                    {
                        DBG_PRINT(std::cout << "promise final no awaiter" << std::endl);
                    }                    
                }

            private:
                promise_type* promise;
            };
            return final_awaiter(this);
        }

        void unhandled_exception()
        {
            abort();
        }

        void* operator new(std::size_t count)
        {
            if(count <= malloc_cache_item_size)
            {
                return malloc_cache();
            }
            return ::new char[count];
        }
        void operator delete(void* ptr, std::size_t sz) noexcept
        {
            if(sz <= malloc_cache_item_size)
            {
                malloc_cache_free(ptr);
                return;
            }
            ::delete (sz, reinterpret_cast<char*>(ptr));
        }

        std::coroutine_handle<> awaiter;
        
        e_res_state res_state;
    };

    template<typename T>
    struct io_uring_promise_type : io_uring_promise_type_base<T>
    {
        using handle = std::coroutine_handle<io_uring_promise_type<T> >;

        T res;

        auto get_return_object() 
        {
            return io_uring_task<T>{handle::from_promise(*this)};
        }

        void return_value(T v)
        {
            if(io_uring_promise_type_base<T>::res_state!=io_uring_promise_type_base<T>::e_res_state::Detached)
            {
                io_uring_promise_type_base<T>::res_state = io_uring_promise_type_base<T>::e_res_state::Res;
                res = std::move(v);
            }
        }
    };

    template<typename T>
    struct [[nodiscard]] io_uring_task
    {
        using promise_type = io_uring_promise_type<T>;
        using handle = std::coroutine_handle<promise_type>;

        io_uring_task(io_uring_task const&) = delete;

	    io_uring_task(io_uring_task&& other) noexcept
            : coro_h(std::exchange(other.coro_h, {}))
        {

        }

	    io_uring_task& operator=(io_uring_task&&) = delete;
	    io_uring_task& operator=(io_uring_task const&) = delete;

        io_uring_task(handle h) noexcept
         : coro_h(h)
        {

        }

        ~io_uring_task() noexcept
        {
            if(coro_h)
            {
                if(!coro_h.done())
                {
                    DBG_PRINT(std::cout << "Detach" << std::endl);
                    coro_h.promise().res_state = promise_type::e_res_state::Detached;
                }
                else
                {
                    DBG_PRINT(std::cout << "Destroy" << std::endl);
                    coro_h.destroy();
                }
            }
        }

        bool has_res() const noexcept
        {
            return coro_h.promise().res_state == promise_type::e_res_state::Res;
        }       

        bool await_ready() const noexcept
        {
            bool r = has_res();
            if(r)
            {
                DBG_PRINT(std::cout << "Await is ready" << std::endl);
            }
            else
            {
                DBG_PRINT(std::cout << "Await is not ready" << std::endl);
            }
            
            return r;
        }

        template<typename U>
        void await_suspend(std::coroutine_handle<io_uring_promise_type<U> > p_awaiter) noexcept
        {
            DBG_PRINT(std::cout << "Await task " << (uint64_t)this << " suspend "<< handle_v(p_awaiter) << " prev " << handle_v(coro_h.promise().awaiter) << std::endl);
            coro_h.promise().awaiter = p_awaiter;
        }

        template<typename U = T, std::enable_if_t<std::is_same<U, void>::value, int> = 0>
        void await_resume() const noexcept
        {
            assert(has_res());
        }

        template<typename U = T, std::enable_if_t<!std::is_same<U, void>::value, int> = 0>
        T await_resume() const noexcept
        {
            assert(has_res());
            return std::move(coro_h.promise().res);
        }

    protected:
        handle coro_h;
    };

    struct FuseIo
    {
        int fuse_fd;
        int pipe[2];
        char* header_buf;
        size_t header_buf_idx;
        char* scratch_buf;
        size_t scratch_buf_idx;
    };

    struct FuseIoVal
    {
        FuseIoVal(fuse_io_context& io_service,
            std::unique_ptr<FuseIo> fuse_io)
         : io_service(io_service),
            fuse_io(std::move(fuse_io))
        {

        }

        ~FuseIoVal()
        {
            io_service.release_fuse_io(std::move(fuse_io));
        }

        FuseIo& get() const noexcept
        {
            return *fuse_io.get();
        }

        FuseIo* operator->() const noexcept
        {
            return fuse_io.get();
        }

    private:
        fuse_io_context& io_service;
        std::unique_ptr<FuseIo> fuse_io;
    };

    struct FuseRing
    {
        FuseRing()
            : ring(nullptr), ring_submit(false),
                max_bufsize(1*1024*1024), backing_fd(-1),
                backing_fd_orig(-1), backing_f_size(0)
                {}

        FuseRing(FuseRing&&) = default;
        FuseRing(FuseRing const&) = delete;
	    FuseRing& operator=(FuseRing&&) = delete;
	    FuseRing& operator=(FuseRing const&) = delete;

        std::vector<std::unique_ptr<FuseIo> > ios;
        struct io_uring* ring;
        bool ring_submit;
        size_t max_bufsize;
        int backing_fd;
        int backing_fd_orig;
        uint64_t backing_f_size;
    };

    FuseRing fuse_ring;

    fuse_io_context(FuseRing fuse_ring);
    fuse_io_context(fuse_io_context const&) = delete;
	fuse_io_context(fuse_io_context&& other) = delete;
	fuse_io_context& operator=(fuse_io_context&&) = delete;
	fuse_io_context& operator=(fuse_io_context const&) = delete;

    typedef fuse_io_context::io_uring_task<int> (*queue_fuse_read_t)(fuse_io_context& io);

    int run(queue_fuse_read_t queue_read);

    FuseIoVal get_fuse_io()
    {
        std::unique_ptr<FuseIo> fuse_io = std::move(fuse_ring.ios.back());
        fuse_ring.ios.pop_back();
        return FuseIoVal(*this, std::move(fuse_io));
    }

    void release_fuse_io(std::unique_ptr<FuseIo> fuse_io)
    {
        fuse_ring.ios.push_back(std::move(fuse_io));
    }

private:

    template<typename T>
    struct io_uring_task_discard : io_uring_task<T>
    {
        io_uring_task_discard(io_uring_task<T>&& other) noexcept
            : io_uring_task<T>(std::move(other))
        {
        }
    };

    fuse_io_context::io_uring_task_discard<int> queue_read_set_rc(queue_fuse_read_t queue_read);

    int fuseuring_handle_cqe(struct io_uring_cqe *cqe);
    int fuseuring_submit(bool block);
    
    int last_rc;
};

template<>
struct fuse_io_context::io_uring_promise_type<void> : fuse_io_context::io_uring_promise_type_base<void>
{
    using handle = std::coroutine_handle<fuse_io_context::io_uring_promise_type<void> >;

    auto get_return_object() 
    {
        return io_uring_task<void>{handle::from_promise(*this)};
    }

    void return_void()
    {
        if(fuse_io_context::io_uring_promise_type_base<void>::res_state!=fuse_io_context::io_uring_promise_type_base<void>::e_res_state::Detached)
        {
            fuse_io_context::io_uring_promise_type_base<void>::res_state = fuse_io_context::io_uring_promise_type_base<void>::e_res_state::Res;
        }
    }
};
