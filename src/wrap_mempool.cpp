// Gregor Thalhammer (on Apr 13, 2011) said it's necessary to import Python.h 
// first to prevent OS X from overriding a bunch of macros. (e.g. isspace)
#include <Python.h>

#include <memory>
#include <vector>
#include "wrap_helpers.hpp"
#include "wrap_cl.hpp"
#include "mempool.hpp"
#include "tools.hpp"



namespace
{
  class cl_allocator_base
  {
    protected:
      std::shared_ptr<pyopencl::context> m_context;
      cl_mem_flags m_flags;

    public:
      cl_allocator_base(std::shared_ptr<pyopencl::context> const &ctx,
          cl_mem_flags flags=CL_MEM_READ_WRITE)
        : m_context(ctx), m_flags(flags)
      {
        if (flags & (CL_MEM_USE_HOST_PTR | CL_MEM_COPY_HOST_PTR))
          throw pyopencl::error("Allocator", CL_INVALID_VALUE,
              "cannot specify USE_HOST_PTR or COPY_HOST_PTR flags");
      }

      cl_allocator_base(cl_allocator_base const &src)
      : m_context(src.m_context), m_flags(src.m_flags)
      { }

      virtual ~cl_allocator_base()
      { }

      typedef cl_mem pointer_type;
      typedef size_t size_type;

      virtual cl_allocator_base *copy() const = 0;
      virtual bool is_deferred() const = 0;
      virtual pointer_type allocate(size_type s) = 0;

      void free(pointer_type p)
      {
        PYOPENCL_CALL_GUARDED(clReleaseMemObject, (p));
      }

      void try_release_blocks()
      {
        pyopencl::run_python_gc();
      }
  };

  class cl_deferred_allocator : public cl_allocator_base
  {
    private:
      typedef cl_allocator_base super;

    public:
      cl_deferred_allocator(std::shared_ptr<pyopencl::context> const &ctx,
          cl_mem_flags flags=CL_MEM_READ_WRITE)
        : super(ctx, flags)
      { }

      cl_allocator_base *copy() const
      {
        return new cl_deferred_allocator(*this);
      }

      bool is_deferred() const
      { return true; }

      pointer_type allocate(size_type s)
      {
        return pyopencl::create_buffer(m_context->data(), m_flags, s, 0);
      }
  };

  const unsigned zero = 0;

  class cl_immediate_allocator : public cl_allocator_base
  {
    private:
      typedef cl_allocator_base super;
      pyopencl::command_queue m_queue;

    public:
      cl_immediate_allocator(pyopencl::command_queue &queue,
          cl_mem_flags flags=CL_MEM_READ_WRITE)
        : super(std::shared_ptr<pyopencl::context>(queue.get_context()), flags),
        m_queue(queue.data(), /*retain*/ true)
      { }

      cl_immediate_allocator(cl_immediate_allocator const &src)
        : super(src), m_queue(src.m_queue)
      { }

      cl_allocator_base *copy() const
      {
        return new cl_immediate_allocator(*this);
      }

      bool is_deferred() const
      { return false; }

      pointer_type allocate(size_type s)
      {
        pointer_type ptr =  pyopencl::create_buffer(
            m_context->data(), m_flags, s, 0);

        // Make sure the buffer gets allocated right here and right now.
        // This looks (and is) expensive. But immediate allocators
        // have their main use in memory pools, whose basic assumption
        // is that allocation is too expensive anyway--but they rely
        // on exact 'out-of-memory' information.
        unsigned zero = 0;
        PYOPENCL_CALL_GUARDED(clEnqueueWriteBuffer, (
              m_queue.data(),
              ptr,
              /* is blocking */ CL_FALSE,
              0, std::min(s, sizeof(zero)), &zero,
              0, NULL, NULL
              ));

        // No need to wait for completion here. clWaitForEvents (e.g.)
        // cannot return mem object allocation failures. This implies that
        // the buffer is faulted onto the device on enqueue.

        return ptr;
      }
  };




  inline
  pyopencl::buffer *allocator_call(cl_allocator_base &alloc, size_t size)
  {
    cl_mem mem;
    int try_count = 0;
    while (try_count < 2)
    {
      try
      {
        mem = alloc.allocate(size);
        break;
      }
      catch (pyopencl::error &e)
      {
        if (!e.is_out_of_memory())
          throw;
        if (++try_count == 2)
          throw;
      }

      alloc.try_release_blocks();
    }

    try
    {
      return new pyopencl::buffer(mem, false);
    }
    catch (...)
    {
      PYOPENCL_CALL_GUARDED(clReleaseMemObject, (mem));
      throw;
    }
  }




  class pooled_buffer
    : public pyopencl::pooled_allocation<pyopencl::memory_pool<cl_allocator_base> >,
    public pyopencl::memory_object_holder
  {
    private:
      typedef
        pyopencl::pooled_allocation<pyopencl::memory_pool<cl_allocator_base> >
        super;

    public:
      pooled_buffer(
          std::shared_ptr<super::pool_type> p, super::size_type s)
        : super(p, s)
      { }

      const super::pointer_type data() const
      { return ptr(); }
  };




  pooled_buffer *device_pool_allocate(
      std::shared_ptr<pyopencl::memory_pool<cl_allocator_base> > pool,
      pyopencl::memory_pool<cl_allocator_base>::size_type sz)
  {
    return new pooled_buffer(pool, sz);
  }




  template<class Wrapper>
  void expose_memory_pool(Wrapper &wrapper)
  {
    typedef typename Wrapper::type cls;
    wrapper
      .def_property_readonly("held_blocks", &cls::held_blocks)
      .def_property_readonly("active_blocks", &cls::active_blocks)
      .DEF_SIMPLE_STATIC_METHOD(bin_number)
      .DEF_SIMPLE_STATIC_METHOD(alloc_size)
      .DEF_SIMPLE_METHOD(free_held)
      .DEF_SIMPLE_METHOD(stop_holding)
      ;
  }
}




void pyopencl_expose_mempool(py::module &m)
{
  m.def("bitlog2", pyopencl::bitlog2);

  {
    typedef cl_allocator_base cls;
    py::class_<cls /*, boost::noncopyable */> wrapper(
        m, "_tools_AllocatorBase"/*, py::no_init */);
    wrapper
      .def("__call__", allocator_call)
      ;

  }

  {
    typedef cl_deferred_allocator cls;
    py::class_<cls, cl_allocator_base> wrapper(
        m, "_tools_DeferredAllocator");
    wrapper
      .def(py::init<
          std::shared_ptr<pyopencl::context> const &>())
      .def(py::init<
          std::shared_ptr<pyopencl::context> const &,
          cl_mem_flags>())
      ;
  }

  {
    typedef cl_immediate_allocator cls;
    py::class_<cls, cl_allocator_base> wrapper(
        m, "_tools_ImmediateAllocator");
    wrapper
      .def(py::init<pyopencl::command_queue &>())
      .def(py::init<pyopencl::command_queue &, cl_mem_flags>())
      ;
  }

  {
    typedef pyopencl::memory_pool<cl_allocator_base> cls;

    py::class_<
      cls, /* boost::noncopyable, */
      std::shared_ptr<cls>> wrapper( m, "MemoryPool");
    wrapper
      .def(py::init<cl_allocator_base const &>())
      .def("allocate", device_pool_allocate)
      .def("__call__", device_pool_allocate)
      // undoc for now
      .DEF_SIMPLE_METHOD(set_trace)
      ;

    expose_memory_pool(wrapper);
  }

  {
    typedef pooled_buffer cls;
    py::class_<cls, /* boost::noncopyable, */
      pyopencl::memory_object_holder>(
          m, "PooledBuffer"/* , py::no_init */)
      .def("release", &cls::free)
      ;
  }
}
