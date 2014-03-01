/* Copyright 2013 Stanford University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lowlevel_gpu.h"

#include <stdio.h>

namespace LegionRuntime {
  namespace LowLevel {
    extern Logger::Category log_gpu;

    class GPUJob : public Event::Impl::EventWaiter {
    public:
      GPUJob(GPUProcessor *_gpu, Event _finish_event)
	: gpu(_gpu), finish_event(_finish_event) {}

      virtual ~GPUJob(void) {}

      virtual bool event_triggered(void) = 0;

      virtual void print_info(void) = 0;

      virtual void run_or_wait(Event start_event) = 0;

      virtual void execute(void) = 0;

      virtual bool is_finished(void) = 0;

      virtual void finish_job(void) = 0;

    public:
      GPUProcessor *gpu;
      Event finish_event;
    };

    class GPUTask : public GPUJob {
    public:
      GPUTask(GPUProcessor *_gpu, Event _finish_event,
	      Processor::TaskFuncID _func_id,
	      const void *_args, size_t _arglen,
              int priority);

      virtual ~GPUTask(void);

      virtual bool event_triggered(void);

      virtual void print_info(void);

      virtual void run_or_wait(Event start_event);

      virtual void execute(void);

      virtual bool is_finished(void);

      virtual void finish_job(void);

      Processor::TaskFuncID func_id;
      void *args;
      size_t arglen;
      int priority;
    };

    class GPUProcessor::Internal {
    public:
      GPUProcessor *const gpu;
      const int gpu_index;
      const size_t zcmem_size, fbmem_size;
      const size_t zcmem_reserve, fbmem_reserve;
      const bool gpu_dma_thread;
      void *zcmem_cpu_base;
      void *zcmem_gpu_base;
      void *fbmem_gpu_base;

      bool initialized;
      bool worker_enabled;
      bool shutdown_requested;
      bool idle_task_enabled;
      pthread_t gpu_thread;
      gasnet_hsl_t mutex;
      gasnett_cond_t parent_condvar, worker_condvar;
      
      std::list<GPUTask*> tasks;
      std::deque<GPUJob*> copies;

      // Our CUDA context that we will create
      CUcontext proc_ctx;

      // Streams for different copy types
      cudaStream_t host_to_device_stream;
      cudaStream_t device_to_host_stream;
      cudaStream_t device_to_device_stream;

      // List of pending copies on each stream
      std::deque<GPUJob*> host_device_copies;
      std::deque<GPUJob*> device_host_copies;
      std::deque<GPUJob*> device_device_copies;

      Internal(GPUProcessor *_gpu, int _gpu_index, 
               size_t _zcmem_size, size_t _fbmem_size,
               size_t _zcmem_res, size_t _fbmem_res,
               bool enabled, bool gpu_dma)
	: gpu(_gpu), gpu_index(_gpu_index),
          zcmem_size(_zcmem_size), fbmem_size(_fbmem_size),
          zcmem_reserve(_zcmem_res), fbmem_reserve(_fbmem_res),
          gpu_dma_thread(gpu_dma),
          initialized(false), worker_enabled(false), 
          shutdown_requested(false),
	  idle_task_enabled(enabled)
      {
	gasnet_hsl_init(&mutex);
	gasnett_cond_init(&parent_condvar);
	gasnett_cond_init(&worker_condvar);
        // Make our context and then immediately pop it off
        CUdevice cuDevice;
        CHECK_CU( cuDeviceGet(&cuDevice, gpu_index) );

        CHECK_CU( cuCtxCreate(&proc_ctx, CU_CTX_MAP_HOST |
                              CU_CTX_SCHED_BLOCKING_SYNC, cuDevice) );

        CHECK_CU( cuCtxPopCurrent(&proc_ctx) );
      }

      Processor get_processor(void) const
      {
        return gpu->me;
      }

      void thread_main(void)
      {
	gasnett_threadkey_set(gpu_thread, this);

        // Push our context onto the stack
        CHECK_CU( cuCtxPushCurrent(proc_ctx) );
        
        // allocate zero-copy memory
	CHECK_CUDART( cudaHostAlloc(&zcmem_cpu_base, 
				    zcmem_size + zcmem_reserve,
				    (cudaHostAllocPortable |
				     cudaHostAllocMapped)) );
	CHECK_CUDART( cudaHostGetDevicePointer(&zcmem_gpu_base,
					       zcmem_cpu_base, 0) );

	// allocate framebuffer memory
	CHECK_CUDART( cudaMalloc(&fbmem_gpu_base, fbmem_size + fbmem_reserve) );

        // initialize the streams for copy operations
        CHECK_CUDART( cudaStreamCreateWithFlags(&host_to_device_stream,
                                                cudaStreamNonBlocking) );
        CHECK_CUDART( cudaStreamCreateWithFlags(&device_to_host_stream,
                                                cudaStreamNonBlocking) );
        CHECK_CUDART( cudaStreamCreateWithFlags(&device_to_device_stream,
                                                cudaStreamNonBlocking) );

	log_gpu(LEVEL_INFO, "gpu initialized: zcmem=%p/%p fbmem=%p",
		zcmem_cpu_base, zcmem_gpu_base, fbmem_gpu_base);

	// set the initialized flag and maybe wake up parent
	{
	  AutoHSLLock a(mutex);
	  initialized = true;
	  gasnett_cond_signal(&parent_condvar);

	  // wait until we've been told to proceed
	  while(!worker_enabled) {
	    log_gpu.info("waiting for enable signal");
	    gasnett_cond_wait(&worker_condvar, &mutex.lock);
	  }
	}

	while(!shutdown_requested) {
	  // get all the copies and a job off the job queue - sleep if nothing there
	  GPUJob *job = NULL;
          std::vector<GPUJob*> ready_copies;
	  {
	    AutoHSLLock a(mutex);
	    while(tasks.empty() && copies.empty() && !shutdown_requested) {
	      // see if there's an idle task we should run
	      Processor::TaskIDTable::iterator it = task_id_table.find(Processor::TASK_ID_PROCESSOR_IDLE);
              const bool has_pending_copies = !gpu_dma_thread &&
                                              (!host_device_copies.empty() ||
                                               !device_host_copies.empty() ||
                                               !device_device_copies.empty());
	      if(idle_task_enabled && (it != task_id_table.end())) {
		gasnet_hsl_unlock(&mutex);
		log_gpu.spew("running scheduler thread");
		(it->second)(0, 0, gpu->me);
		log_gpu.spew("returned from scheduler thread");
		gasnet_hsl_lock(&mutex);
                // Can't go to sleep if we have copies to poll
	      } else if (!has_pending_copies) {
		log_gpu.debug("job queue empty - sleeping\n");
		gasnett_cond_wait(&worker_condvar, &mutex.lock);
		if(shutdown_requested) {
		  log_gpu.debug("awoke due to shutdown request...\n");
		  break;
		}
		log_gpu.debug("awake again...\n");
	      }
              else
              {
                // We have pending copies so break out
                break;
              }
	    }
	    if(shutdown_requested) break;
            if (!gpu_dma_thread)
            {
              ready_copies.insert(ready_copies.end(),copies.begin(),copies.end());
              copies.clear();
            }
            if (!tasks.empty())
            {
              job = tasks.front();
              tasks.pop_front();
            }
	  }

          if (!gpu_dma_thread)
          {
            // Check to see if any of our copies have completed
            check_for_complete_copies();  

            // Launch all the copies first since we know that they
            // are going to be asynchronous on streams that won't block tasks
            // These calls well enqueue the copies on the right queue.
            for (std::vector<GPUJob*>::const_iterator it = ready_copies.begin();
                  it != ready_copies.end(); it++)
            {
              (*it)->execute();
            }
          }

	  // charge all the time from the start of the execute until the end
	  //  of the device synchronize to the app - anything that wasn't
	  //  really the app will be claimed anyway
          if (job != NULL)
	  {
	    DetailedTimer::ScopedPush sp(TIME_KERNEL);
	    //printf("executing job %p\n", job);
	    job->execute();
	    // TODO: use events here!
            // Trust users right now to do their own synchronization
	    //CHECK_CUDART( cudaDeviceSynchronize() );
            log_gpu.info("gpu device synchronized");
            job->finish_job();
            delete job;
	  }
	}

	log_gpu.info("shutting down");
        Processor::TaskIDTable::iterator it = task_id_table.find(Processor::TASK_ID_PROCESSOR_SHUTDOWN);
        if(it != task_id_table.end()) {
          log_gpu(LEVEL_INFO, "calling processor shutdown task: proc=%x", gpu->me.id);

          (it->second)(0, 0, gpu->me);

          log_gpu(LEVEL_INFO, "finished processor shutdown task: proc=%x", gpu->me.id);
        }
	gpu->finished();
      }

      static void *thread_main_wrapper(void *data)
      {
	GPUProcessor::Internal *obj = (GPUProcessor::Internal *)data;
	obj->thread_main();
	return 0;
      }

      void create_gpu_thread(size_t stack_size)
      {
	pthread_attr_t attr;
	CHECK_PTHREAD( pthread_attr_init(&attr) );
        CHECK_PTHREAD( pthread_attr_setstacksize(&attr, stack_size) );
	if(proc_assignment)
	  proc_assignment->bind_thread(-1, &attr, "GPU worker");
	CHECK_PTHREAD( pthread_create(&gpu_thread, &attr, 
				      thread_main_wrapper,
				      (void *)this) );
	CHECK_PTHREAD( pthread_attr_destroy(&attr) );

	// now wait until worker thread is ready
	{
	  AutoHSLLock a(mutex);
	  while(!initialized)
	    gasnett_cond_wait(&parent_condvar, &mutex.lock);
	}
      }

      void enqueue_task(GPUTask *job)
      {
        AutoHSLLock a(mutex);

	bool was_empty = tasks.empty() && copies.empty();
        // Add it based on its priority
        // Common case
        if (tasks.empty() || (tasks.back()->priority >= job->priority))
          tasks.push_back(job);
        else
        {
          // Uncommon case: go through the list until
          // we find someone who has a priority lower than ours.
          bool inserted = false;
          for (std::list<GPUTask*>::iterator it = tasks.begin();
                it != tasks.end(); it++)
          {
            if ((*it)->priority < job->priority)
            {
              tasks.insert(it, job);
              inserted = true;
              break;
            }
          }
          // Technically we shouldn't need this
          if (!inserted)
            tasks.push_back(job);
        }

	if(was_empty)
	  gasnett_cond_signal(&worker_condvar);
      }

      void enqueue_copy(GPUJob *job)
      {
        AutoHSLLock a(mutex);

        bool was_empty = tasks.empty() && copies.empty();
        copies.push_back(job);

        if (was_empty)
          gasnett_cond_signal(&worker_condvar);
      }

      void add_host_device_copy(GPUJob *copy)
      {
        host_device_copies.push_back(copy);
      }
      void add_device_host_copy(GPUJob *copy)
      {
        device_host_copies.push_back(copy);
      }
      void add_device_device_copy(GPUJob *copy)
      {
        device_device_copies.push_back(copy);
      }
      void check_for_complete_copies(void)
      {
        // Check to see if we have any pending copies to query
        while (!host_device_copies.empty())
        {
          GPUJob *next = host_device_copies.front();
          if (next->is_finished())
          {
            next->finish_job();
            delete next;
            host_device_copies.pop_front();
          }
          else
            break; // If the first one wasn't done, the others won't be either
        }
        while (!device_host_copies.empty())
        {
          GPUJob *next = device_host_copies.front();
          if (next->is_finished())
          {
            next->finish_job();
            delete next;
            device_host_copies.pop_front();
          }
          else
            break;
        }
        while (!device_device_copies.empty())
        {
          GPUJob *next = device_device_copies.front();
          if (next->is_finished())
          {
            next->finish_job();
            delete next;
            device_device_copies.pop_front();
          }
          else
            break;
        }
      }
      void register_host_memory(void *base, size_t size)
      {
        if (!shutdown_requested)
        {
          CHECK_CU( cuCtxPushCurrent(proc_ctx) );
          CHECK_CU( cuMemHostRegister(base, size, CU_MEMHOSTREGISTER_PORTABLE) ); 
          CHECK_CU( cuCtxPopCurrent(&proc_ctx) );
        }
      }
      void handle_copies(void)
      {
        if (!shutdown_requested)
        {
          // Push our context onto the stack
          CHECK_CU( cuCtxPushCurrent(proc_ctx) );
          // First see if any of our copies are ready
          check_for_complete_copies();
          std::vector<GPUJob*> ready_copies;
          // Get any copies that are ready to be performed
          {
            AutoHSLLock a(mutex);
            ready_copies.insert(ready_copies.end(),copies.begin(),copies.end());
            copies.clear();
          }
          // Issue our copies
          for (std::vector<GPUJob*>::const_iterator it = ready_copies.begin();
                it != ready_copies.end(); it++)
          {
            (*it)->execute();
          }
          // Now pop our context back off the stack
          CHECK_CU( cuCtxPopCurrent(&proc_ctx) );
        }
      }
    };

    GPUTask::GPUTask(GPUProcessor *_gpu, Event _finish_event,
		     Processor::TaskFuncID _func_id,
		     const void *_args, size_t _arglen,
                     int _priority)
      : GPUJob(_gpu, _finish_event), func_id(_func_id), 
               arglen(_arglen), priority(_priority)
    {
      if(arglen) {
	args = malloc(arglen);
	memcpy(args, _args, arglen);
      } else {
	args = 0;
      }
    }

    GPUTask::~GPUTask(void)
    {
      if(args) free(args);
    }

    bool GPUTask::event_triggered(void)
    {
      log_gpu.info("gpu job %p now runnable", this);
      gpu->internal->enqueue_task(this);

      // don't delete
      return false;
    }

    void GPUTask::print_info(void)
    {
      printf("GPU Task: %p after=%x/%d\n",
          this, finish_event.id, finish_event.gen);
    }

    void GPUTask::run_or_wait(Event start_event)
    {
      if(start_event.has_triggered()) {
	log_gpu.info("job %p can start right away!?", this);
	gpu->internal->enqueue_task(this);
      } else {
	log_gpu.info("job %p waiting for %x/%d", this, start_event.id, start_event.gen);
	start_event.impl()->add_waiter(start_event, this);
      }
    }

    void GPUTask::execute(void)
    {
      Processor::TaskFuncPtr fptr = task_id_table[func_id];
      char argstr[100];
      argstr[0] = 0;
      for(size_t i = 0; (i < arglen) && (i < 40); i++)
	sprintf(argstr+2*i, "%02x", ((unsigned char *)args)[i]);
      if(arglen > 40) strcpy(argstr+80, "...");
      log_gpu(LEVEL_DEBUG, "task start: %d (%p) (%s)", func_id, fptr, argstr);
      (*fptr)(args, arglen, gpu->me);
      log_gpu(LEVEL_DEBUG, "task end: %d (%p) (%s)", func_id, fptr, argstr);
    }

    bool GPUTask::is_finished(void)
    {
      return true;
    }

    void GPUTask::finish_job(void)
    {
      if (finish_event.exists())
        finish_event.impl()->trigger(finish_event.gen, gasnet_mynode());
    }

    // An abstract base class for all GPU memcpy operations
    class GPUMemcpy : public GPUJob {
    public:
      GPUMemcpy(GPUProcessor *_gpu, Event _finish_event,
                cudaMemcpyKind _kind)
        : GPUJob(_gpu, _finish_event), kind(_kind)
      {
        if (kind == cudaMemcpyHostToDevice)
          local_stream = gpu->internal->host_to_device_stream;
        else if (kind == cudaMemcpyDeviceToHost)
          local_stream = gpu->internal->device_to_host_stream;
        else if (kind == cudaMemcpyDeviceToDevice)
          local_stream = gpu->internal->device_to_device_stream;
        else
          assert(false); // who does host to host here?!?
      }
    public:
      virtual bool event_triggered(void)
      {
        log_gpu.info("gpu job %p now runnable", this);
        gpu->internal->enqueue_copy(this);

        // don't delete
        return false;
      }

      virtual void print_info(void)
      {
        printf("GPU Memcpy: %p after=%x/%d\n",
            this, finish_event.id, finish_event.gen);
      }

      virtual void run_or_wait(Event start_event)
      {
        if(start_event.has_triggered()) {
          log_gpu.info("job %p can start right away!?", this);
          gpu->internal->enqueue_copy(this);
        } else {
          log_gpu.info("job %p waiting for %x/%d", this, start_event.id, start_event.gen);
          start_event.impl()->add_waiter(start_event, this);
        }
      }

      virtual void execute(void) = 0;

      void post_execute(void)
      {
        CHECK_CUDART( cudaEventCreateWithFlags(&complete_event,
                                               cudaEventDisableTiming) );
        CHECK_CUDART( cudaEventRecord(complete_event, local_stream) );
        if (kind == cudaMemcpyHostToDevice)
          gpu->internal->add_host_device_copy(this);
        else if (kind == cudaMemcpyDeviceToHost)
          gpu->internal->add_device_host_copy(this);
        else if (kind == cudaMemcpyDeviceToDevice)
          gpu->internal->add_device_device_copy(this);
        else
          assert(false);
      }

      virtual bool is_finished(void)
      {
        cudaError_t result = cudaEventQuery(complete_event);
        if (result == cudaSuccess)
          return true;
        else if (result == cudaErrorNotReady)
          return false;
        else
        {
          CHECK_CUDART( result );
        }
        return false;
      }

      virtual void finish_job(void)
      {
        // If we have a finish event then trigger it
        if (finish_event.exists())
          finish_event.impl()->trigger(finish_event.gen, gasnet_mynode());
        // Destroy our event
        CHECK_CUDART( cudaEventDestroy(complete_event) );
      }
    protected:
      cudaMemcpyKind kind;
      cudaStream_t local_stream;
      cudaEvent_t complete_event;
    };

    class GPUMemcpy1D : public GPUMemcpy {
    public:
      GPUMemcpy1D(GPUProcessor *_gpu, Event _finish_event,
		void *_dst, const void *_src, size_t _bytes, cudaMemcpyKind _kind)
	: GPUMemcpy(_gpu, _finish_event, _kind), dst(_dst), src(_src), 
	  mask(0), elmt_size(_bytes)
      {}

      GPUMemcpy1D(GPUProcessor *_gpu, Event _finish_event,
		void *_dst, const void *_src, 
		const ElementMask *_mask, size_t _elmt_size,
		cudaMemcpyKind _kind)
	: GPUMemcpy(_gpu, _finish_event, _kind), dst(_dst), src(_src),
	  mask(_mask), elmt_size(_elmt_size)
      {}

      void do_span(off_t pos, size_t len)
      {
	off_t span_start = pos * elmt_size;
	size_t span_bytes = len * elmt_size;

        CHECK_CUDART( cudaMemcpyAsync(((char *)dst)+span_start,
                                      ((char *)src)+span_start,
                                      span_bytes,
                                      cudaMemcpyDefault,
                                      local_stream) );
      }

      virtual void execute(void)
      {
	DetailedTimer::ScopedPush sp(TIME_COPY);
	log_gpu.info("gpu memcpy: dst=%p src=%p bytes=%zd kind=%d",
		     dst, src, elmt_size, kind);
	if(mask) {
	  ElementMask::forall_ranges(*this, *mask);
	} else {
	  do_span(0, 1);
	}
        post_execute();  
	log_gpu.info("gpu memcpy complete: dst=%p src=%p bytes=%zd kind=%d",
		     dst, src, elmt_size, kind);
      } 
    protected:
      void *dst;
      const void *src;
      const ElementMask *mask;
      size_t elmt_size;
    };

    class GPUMemcpy2D : public GPUMemcpy {
    public:
      GPUMemcpy2D(GPUProcessor *_gpu, Event _finish_event,
                  void *_dst, const void *_src,
                  off_t _dst_stride, off_t _src_stride,
                  size_t _bytes, size_t _lines,
                  cudaMemcpyKind _kind)
        : GPUMemcpy(_gpu, _finish_event, _kind), dst(_dst), src(_src),
          dst_stride(_dst_stride), src_stride(_src_stride),
          bytes(_bytes), lines(_lines)
      {}
    public:
      virtual void execute(void)
      {
        log_gpu.info("gpu memcpy 2d: dst=%p src=%p "
                     "dst_off=%ld src_off=%ld bytes=%ld lines=%ld kind=%d",
                     dst, src, dst_stride, src_stride, bytes, lines, kind); 
        CHECK_CUDART( cudaMemcpy2DAsync(dst, dst_stride, src, src_stride,
                        bytes, lines, cudaMemcpyDefault, local_stream) );
        post_execute();
        log_gpu.info("gpu memcpy 2d complete: dst=%p src=%p "
                     "dst_off=%ld src_off=%ld bytes=%ld lines=%ld kind=%d",
                     dst, src, dst_stride, src_stride, bytes, lines, kind);
      }
    protected:
      void *dst;
      const void *src;
      off_t dst_stride, src_stride;
      size_t bytes, lines;
    };

    class GPUMemcpyGeneric : public GPUJob {
    public:
      struct PendingMemcpy {
      public:
        PendingMemcpy(off_t o, void *s, size_t b)
          : offset(o), src(s), bytes(b) { }
      public:
        off_t offset;
        void *src;
        size_t bytes;
      };
    public:
      GPUMemcpyGeneric(GPUProcessor *_gpu, Event _finish_event,
		       void *_gpu_ptr, Memory::Impl *_memory, off_t _mem_offset, size_t _bytes, cudaMemcpyKind _kind)
	: GPUJob(_gpu, _finish_event), gpu_ptr(_gpu_ptr), memory(_memory),
	  mem_offset(_mem_offset), mask(0), elmt_size(_bytes), kind(_kind)
      {}

      GPUMemcpyGeneric(GPUProcessor *_gpu, Event _finish_event,
		       void *_gpu_ptr, Memory::Impl *_memory, off_t _mem_offset,
		       const ElementMask *_mask, size_t _elmt_size,
		       cudaMemcpyKind _kind)
	: GPUJob(_gpu, _finish_event), gpu_ptr(_gpu_ptr), memory(_memory),
	  mem_offset(_mem_offset), mask(_mask), elmt_size(_elmt_size), kind(_kind)
      {}

      virtual bool event_triggered(void)
      {
        log_gpu.info("gpu job %p now runnable", this);
        gpu->internal->enqueue_copy(this);

        // don't delete
        return false;
      }

      virtual void run_or_wait(Event start_event)
      {
        if(start_event.has_triggered()) {
          log_gpu.info("job %p can start right away!?", this);
          gpu->internal->enqueue_copy(this);
        } else {
          log_gpu.info("job %p waiting for %x/%d", this, start_event.id, start_event.gen);
          start_event.impl()->add_waiter(start_event, this);
        }
      }

      void do_span(off_t pos, size_t len)
      {
	off_t span_start = pos * elmt_size;
	size_t span_bytes = len * elmt_size;

        // First check to see if we need to make a new buffer
        if (span_bytes > remaining_bytes)
        {
          // Make a new buffer
          if (span_bytes > base_buffer_size)
          {
            CHECK_CUDART( cudaMallocHost((void**)&current_buffer, span_bytes) );
            remaining_bytes = span_bytes;
          }
          else
          {
            CHECK_CUDART( cudaMallocHost((void**)&current_buffer, base_buffer_size) );
            remaining_bytes = base_buffer_size;
          }
          allocated_buffers.push_back(current_buffer);
        }
        // Now issue the copy
        if (kind == cudaMemcpyDeviceToHost) {
          // In this case we have to issue the copy and then deffer
          // the memory copy until we know the full copy is complete
          CHECK_CUDART( cudaMemcpyAsync(current_buffer,
                                        ((char*)gpu_ptr)+span_start,
                                        span_bytes,
                                        cudaMemcpyDefault,
                                        local_stream) );
          pending_copies.push_back(PendingMemcpy(mem_offset+span_start,
                                                 current_buffer, span_bytes));
        } else {
          memory->get_bytes(mem_offset+span_start, current_buffer, span_bytes);
          CHECK_CUDART( cudaMemcpyAsync(((char*)gpu_ptr)+span_start,
                                        current_buffer,
                                        span_bytes,
                                        cudaMemcpyDefault,
                                        local_stream) );
        }
        // Update the pointer
        current_buffer += span_bytes;
        // Mark how many bytes we used
        remaining_bytes -= span_bytes;
      }

      virtual void execute(void)
      {
	DetailedTimer::ScopedPush sp(TIME_COPY);
        // Figure out which stream we are based on our kind
        if (kind == cudaMemcpyHostToDevice) {
          local_stream = gpu->internal->host_to_device_stream;
          gpu->internal->add_host_device_copy(this);
        }
        else if (kind == cudaMemcpyDeviceToHost) {
          local_stream = gpu->internal->device_to_host_stream;
          gpu->internal->add_device_host_copy(this);
        }
        else if (kind == cudaMemcpyDeviceToDevice) {
          local_stream = gpu->internal->device_to_device_stream;
          gpu->internal->add_device_device_copy(this);
        }
        else
          assert(false); // who does host to host here?!?
	log_gpu.info("gpu memcpy generic: gpuptr=%p mem=%x offset=%zd bytes=%zd kind=%d",
		     gpu_ptr, memory->me.id, mem_offset, elmt_size, kind);
        // Initialize our first buffer
        CHECK_CUDART( cudaMallocHost((void**)&current_buffer, base_buffer_size) );
        remaining_bytes = base_buffer_size;
        allocated_buffers.push_back(current_buffer);
	if(mask) {
	  ElementMask::forall_ranges(*this, *mask);
	} else {
	  do_span(0, 1);
	}

	log_gpu.info("gpu memcpy generic done: gpuptr=%p mem=%x offset=%zd bytes=%zd kind=%d",
		     gpu_ptr, memory->me.id, mem_offset, elmt_size, kind);
        // Make an event and put it on the stream
        CHECK_CUDART( cudaEventCreateWithFlags(&completion_event,
                                               cudaEventDisableTiming) );
        CHECK_CUDART( cudaEventRecord(completion_event, local_stream) );
      }

      virtual bool is_finished(void)
      {
        cudaError_t result = cudaEventQuery(completion_event);
        if (result == cudaSuccess)
          return true;
        else if (result == cudaErrorNotReady)
          return false;
        else
        {
          CHECK_CUDART( result );
        }
        return false;
      }

      virtual void finish_job(void)
      {
        // If we had any pending copies, do them before triggering the event
        for (unsigned idx = 0; idx < pending_copies.size(); idx++)
        {
          PendingMemcpy &copy = pending_copies[idx];
          memory->put_bytes(copy.offset, copy.src, copy.bytes);
        }
        // If we have a finish event then trigger it
        if (finish_event.exists())
          finish_event.impl()->trigger(finish_event.gen, gasnet_mynode());
        // Free up any buffers that we made when performing the copy
        for (unsigned idx = 0; idx < allocated_buffers.size(); idx++)
        {
          CHECK_CUDART( cudaFreeHost(allocated_buffers[idx]) );
        }
        allocated_buffers.clear();
        // Free up our event
        CHECK_CU( cuEventDestroy(completion_event) );
      }

    protected:
      void *gpu_ptr;
      Memory::Impl *memory;
      off_t mem_offset;
      const ElementMask *mask;
      size_t elmt_size;
      cudaMemcpyKind kind;
      cudaStream_t local_stream;
      cudaEvent_t completion_event;
      std::deque<void*> allocated_buffers;
      std::deque<PendingMemcpy> pending_copies;
    public:
      // Default buffer size
      static const size_t base_buffer_size = 65536;
    protected:
      size_t remaining_bytes;
      char *current_buffer;
    };

    GPUProcessor::GPUProcessor(Processor _me, int _gpu_index, Processor _util,
	     size_t _zcmem_size, size_t _fbmem_size, 
             size_t _stack_size, bool gpu_dma_thread)
      : Processor::Impl(_me, Processor::TOC_PROC, _util)
    {
      internal = new GPUProcessor::Internal(this, _gpu_index,
                                            _zcmem_size, _fbmem_size,
                                            (16 << 20), (32 << 20),
                                            !_util.exists(), gpu_dma_thread);

      // enqueue a GPU init job before we do anything else
      Processor::TaskIDTable::iterator it = task_id_table.find(Processor::TASK_ID_PROCESSOR_INIT);
      if(it != task_id_table.end())
	internal->enqueue_task(new GPUTask(this, Event::NO_EVENT,
					  Processor::TASK_ID_PROCESSOR_INIT, 0, 0, 0));

      internal->create_gpu_thread(_stack_size);
    }

    GPUProcessor::~GPUProcessor(void)
    {
      delete internal;
    }

    /*static*/ Processor GPUProcessor::get_processor(void)
    {
      void *tls_val = gasnett_threadkey_get(gpu_thread);
      // If this happens there is a case we're not handling
      assert(tls_val != NULL);
      Internal *me = (Internal*)tls_val;
      return me->get_processor();
    }

    void GPUProcessor::start_worker_thread(void)
    {
      AutoHSLLock a(internal->mutex);
      log_gpu.info("enabling worker thread");
      internal->worker_enabled = true;
      gasnett_cond_signal(&internal->worker_condvar);
    }

    void *GPUProcessor::get_zcmem_cpu_base(void)
    {
      return ((char *)internal->zcmem_cpu_base) + internal->zcmem_reserve;
    }

    void *GPUProcessor::get_fbmem_gpu_base(void)
    {
      return ((char *)internal->fbmem_gpu_base) + internal->fbmem_reserve;
    }

    void GPUProcessor::spawn_task(Processor::TaskFuncID func_id,
				  const void *args, size_t arglen,
				  //std::set<RegionInstanceUntyped> instances_needed,
				  Event start_event, Event finish_event,
                                  int priority)
    {
      log_gpu.info("new gpu task: func_id=%d start=%x/%d finish=%x/%d",
		   func_id, start_event.id, start_event.gen, finish_event.id, finish_event.gen);
      if(func_id != 0) {
	(new GPUTask(this, finish_event,
		     func_id, args, arglen, priority))->run_or_wait(start_event);
      } else {
	AutoHSLLock a(internal->mutex);
	log_gpu.info("received shutdown request!");
	internal->shutdown_requested = true;
	gasnett_cond_signal(&internal->worker_condvar);
      }
    }

    void GPUProcessor::enable_idle_task(void)
    {
      log_gpu.info("idle task enabled for processor %x", me.id);
#ifdef UTIL_PROCS_FOR_GPU
      if (util_proc)
        util_proc->enable_idle_task(this);
      else
#endif
      {
        AutoHSLLock a(internal->mutex);
        internal->idle_task_enabled = true;
        gasnett_cond_signal(&internal->worker_condvar);
      }
    }

    void GPUProcessor::disable_idle_task(void)
    {
      //log_gpu.info("idle task NOT disabled for processor %x", me.id);
      log_gpu.info("idle task disabled for processor %x", me.id);
#ifdef UTIL_PROCS_FOR_GPU
      if (util_proc)
        util_proc->disable_idle_task(this);
      else
#endif
        internal->idle_task_enabled = false;
    }

    void GPUProcessor::copy_to_fb(off_t dst_offset, const void *src, size_t bytes,
				  Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
		     ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + dst_offset),
		     src,
		     bytes,
		     cudaMemcpyHostToDevice))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_to_fb(off_t dst_offset, const void *src,
				  const ElementMask *mask, size_t elmt_size,
				  Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
		     ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + dst_offset),
		     src,
		     mask, elmt_size,
		     cudaMemcpyHostToDevice))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_from_fb(void *dst, off_t src_offset, size_t bytes,
				    Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
		     dst,
		     ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + src_offset),
		     bytes,
		     cudaMemcpyDeviceToHost))->run_or_wait(start_event);
    } 

    void GPUProcessor::copy_from_fb(void *dst, off_t src_offset,
				    const ElementMask *mask, size_t elmt_size,
				    Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
		     dst,
		     ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + src_offset),
		     mask, elmt_size,
		     cudaMemcpyDeviceToHost))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_within_fb(off_t dst_offset, off_t src_offset,
				      size_t bytes,
				      Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
		     ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + dst_offset),
		     ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + src_offset),
		     bytes,
		     cudaMemcpyDeviceToDevice))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_within_fb(off_t dst_offset, off_t src_offset,
				      const ElementMask *mask, size_t elmt_size,
				      Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
		     ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + dst_offset),
		     ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + src_offset),
		     mask, elmt_size,
		     cudaMemcpyDeviceToDevice))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_to_fb_2d(off_t dst_offset, const void *src, 
                                     off_t dst_stride, off_t src_stride,
                                     size_t bytes, size_t lines,
                                     Event start_event, Event finish_event)
    {
      (new GPUMemcpy2D(this, finish_event,
                       ((char*)internal->fbmem_gpu_base)+
                        (internal->fbmem_reserve + dst_offset),
                        src, dst_stride, src_stride, bytes, lines,
                        cudaMemcpyHostToDevice))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_from_fb_2d(void *dst, off_t src_offset,
                                       off_t dst_stride, off_t src_stride,
                                       size_t bytes, size_t lines,
                                       Event start_event, Event finish_event)
    {
      (new GPUMemcpy2D(this, finish_event, dst,
                       ((char*)internal->fbmem_gpu_base)+
                        (internal->fbmem_reserve + src_offset),
                        dst_stride, src_stride, bytes, lines,
                        cudaMemcpyDeviceToHost))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_within_fb_2d(off_t dst_offset, off_t src_offset,
                                         off_t dst_stride, off_t src_stride,
                                         size_t bytes, size_t lines,
                                         Event start_event, Event finish_event)
    {
      (new GPUMemcpy2D(this, finish_event,
                       ((char*)internal->fbmem_gpu_base) + 
                        (internal->fbmem_reserve + dst_offset),
                       ((char*)internal->fbmem_gpu_base) + 
                        (internal->fbmem_reserve + src_offset),
                        dst_stride, src_stride, bytes, lines,
                        cudaMemcpyDeviceToDevice))->run_or_wait(start_event);
    }

    void GPUProcessor::register_host_memory(void *base, size_t size)
    {
      internal->register_host_memory(base, size);
    }

    void GPUProcessor::handle_copies(void)
    {
      internal->handle_copies();
    }

    /*static*/ std::vector<GPUProcessor*> GPUProcessor::local_gpus;

    /*static*/
    void* GPUProcessor::gpu_dma_worker_loop(void *args)
    {
      while (true)
      {
        // Iterate over all the GPU processors and perform the copies
        for (std::vector<GPUProcessor*>::const_iterator it = local_gpus.begin();
              it != local_gpus.end(); it++)
        {
          (*it)->handle_copies();
        }
      }
      return NULL;
    }

    /*static*/
    void GPUProcessor::start_gpu_dma_thread(const std::vector<GPUProcessor*> &local)
    {
      local_gpus = local;
      pthread_attr_t attr;
      CHECK_PTHREAD( pthread_attr_init(&attr) );
      if (proc_assignment)
        proc_assignment->bind_thread(-1, &attr, "GPU DMA worker");
      pthread_t thread;
      CHECK_PTHREAD( pthread_create(&thread, 0, gpu_dma_worker_loop, 0) );
      CHECK_PTHREAD( pthread_attr_destroy(&attr) );
    }

#if 0
    void GPUProcessor::copy_to_fb_generic(off_t dst_offset, 
					  Memory::Impl *src_mem, off_t src_offset,
					  size_t bytes,
					  Event start_event, Event finish_event)
    {
      (new GPUMemcpyGeneric(this, finish_event,
			    ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + dst_offset),
			    src_mem, src_offset,
			    bytes,
			    cudaMemcpyHostToDevice))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_to_fb_generic(off_t dst_offset, 
					  Memory::Impl *src_mem, off_t src_offset,
					  const ElementMask *mask, 
					  size_t elmt_size,
					  Event start_event, Event finish_event)
    {
      (new GPUMemcpyGeneric(this, finish_event,
			    ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + dst_offset),
			    src_mem, src_offset,
			    mask, elmt_size,
			    cudaMemcpyHostToDevice))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_from_fb_generic(Memory::Impl *dst_mem, off_t dst_offset, 
					    off_t src_offset, size_t bytes,
					    Event start_event, Event finish_event)
    {
      (new GPUMemcpyGeneric(this, finish_event,
			    ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + src_offset),
			    dst_mem, dst_offset,
			    bytes,
			    cudaMemcpyDeviceToHost))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_from_fb_generic(Memory::Impl *dst_mem, off_t dst_offset, 
					    off_t src_offset,
					    const ElementMask *mask,
					    size_t elmt_size,
					    Event start_event, Event finish_event)
    {
      (new GPUMemcpyGeneric(this, finish_event,
			    ((char *)internal->fbmem_gpu_base) + (internal->fbmem_reserve + src_offset),
			    dst_mem, dst_offset,
			    mask, elmt_size,
			    cudaMemcpyDeviceToHost))->run_or_wait(start_event);
    }
#endif

    // framebuffer memory

    GPUFBMemory::GPUFBMemory(Memory _me, GPUProcessor *_gpu)
      : Memory::Impl(_me, _gpu->internal->fbmem_size, MKIND_GPUFB, 512, Memory::GPU_FB_MEM),
	gpu(_gpu)
    {
      base = (char *)(gpu->get_fbmem_gpu_base());
      free_blocks[0] = size;
    }

    GPUFBMemory::~GPUFBMemory(void) {}

    // zerocopy memory

    GPUZCMemory::GPUZCMemory(Memory _me, GPUProcessor *_gpu)
      : Memory::Impl(_me, _gpu->internal->zcmem_size, MKIND_ZEROCOPY, 256, Memory::Z_COPY_MEM),
	gpu(_gpu)
    {
      cpu_base = (char *)(gpu->get_zcmem_cpu_base());
      free_blocks[0] = size;
    }

    GPUZCMemory::~GPUZCMemory(void) {}

#if 0
    template <>
    bool RegionAccessor<AccessorGeneric>::can_convert<AccessorGPU>(void) const
    {
      RegionInstance::Impl *i_impl = (RegionInstance::Impl *)internal_data;
      Memory::Impl *m_impl = i_impl->memory.impl();

      // make sure it's not a reduction fold-only instance
      StaticAccess<RegionInstance::Impl> i_data(i_impl);
      if(i_data->is_reduction) return false;

      // only things in FB and ZC memories can be converted to GPU accessors
      if(m_impl->kind == Memory::Impl::MKIND_GPUFB) return true;
      if(m_impl->kind == Memory::Impl::MKIND_ZEROCOPY) return true;
      return false;
    }
#endif

#ifdef POINTER_CHECKS
    static unsigned *get_gpu_valid_mask(RegionMetaDataUntyped region)
    {
	const ElementMask &mask = region.get_valid_mask();
	void *valid_mask_base;
	for(size_t p = 0; p < mask.raw_size(); p += 4)
	  log_gpu.info("  raw mask data[%zd] = %08x\n", p,
		       ((unsigned *)(mask.get_raw()))[p>>2]);
	CHECK_CUDART( cudaMalloc(&valid_mask_base, mask.raw_size()) );
	log_gpu.info("copy of valid mask (%zd bytes) created at %p",
		     mask.raw_size(), valid_mask_base);
	CHECK_CUDART( cudaMemcpy(valid_mask_base,
				 mask.get_raw(),
				 mask.raw_size(),
				 cudaMemcpyHostToDevice) );
	return (unsigned *)&(((ElementMaskImpl *)valid_mask_base)->bits);
    }
#endif

#ifdef OLD_ACCESSORS    
    template <>
    RegionAccessor<AccessorGPU> RegionAccessor<AccessorGeneric>::convert<AccessorGPU>(void) const
    {
      RegionInstance::Impl *i_impl = (RegionInstance::Impl *)internal_data;
      Memory::Impl *m_impl = i_impl->memory.impl();

      StaticAccess<RegionInstance::Impl> i_data(i_impl);

      assert(!i_data->is_reduction);

      // only things in FB and ZC memories can be converted to GPU accessors
      if(m_impl->kind == Memory::Impl::MKIND_GPUFB) {
	GPUFBMemory *fbm = (GPUFBMemory *)m_impl;
	void *base = (((char *)(fbm->gpu->internal->fbmem_gpu_base)) +
		      fbm->gpu->internal->fbmem_reserve);
	log_gpu.info("creating gpufb accessor (%p + %zd = %p) (%p)",
		     base, i_data->access_offset,
		     ((char *)base)+(i_data->access_offset),
		     ((char *)base)+(i_data->alloc_offset));
	RegionAccessor<AccessorGPU> ria(((char *)base)+(i_data->access_offset));
#ifdef POINTER_CHECKS 
        ria.first_elmt = i_data->first_elmt;
        ria.last_elmt  = i_data->last_elmt;
	ria.valid_mask_base = get_gpu_valid_mask(i_data->region);
#endif
	return ria;
      }

      if(m_impl->kind == Memory::Impl::MKIND_ZEROCOPY) {
	GPUZCMemory *zcm = (GPUZCMemory *)m_impl;
	void *base = (((char *)(zcm->gpu->internal->zcmem_gpu_base)) +
		      zcm->gpu->internal->zcmem_reserve);
	log_gpu.info("creating gpuzc accessor (%p + %zd = %p)",
		     base, i_data->access_offset,
		     ((char *)base)+(i_data->access_offset));
	RegionAccessor<AccessorGPU> ria(((char *)base)+(i_data->access_offset));
#ifdef POINTER_CHECKS 
        ria.first_elmt = i_data->first_elmt;
        ria.last_elmt  = i_data->last_elmt;
	ria.valid_mask_base = get_gpu_valid_mask(i_data->region);
#endif
	return ria;
      }

      assert(0);
    }

    template <>
    bool RegionAccessor<AccessorGeneric>::can_convert<AccessorGPUReductionFold>(void) const
    {
      RegionInstance::Impl *i_impl = (RegionInstance::Impl *)internal_data;
      Memory::Impl *m_impl = i_impl->memory.impl();

      // make sure it's a reduction fold-only instance
      StaticAccess<RegionInstance::Impl> i_data(i_impl);
      if(!i_data->is_reduction) return false;

      // only things in FB and ZC memories can be converted to GPU accessors
      if(m_impl->kind == Memory::Impl::MKIND_GPUFB) return true;
      if(m_impl->kind == Memory::Impl::MKIND_ZEROCOPY) return true;
      return false;
    }
    
    template <>
    RegionAccessor<AccessorGPUReductionFold> RegionAccessor<AccessorGeneric>::convert<AccessorGPUReductionFold>(void) const
    {
      RegionInstance::Impl *i_impl = (RegionInstance::Impl *)internal_data;
      Memory::Impl *m_impl = i_impl->memory.impl();

      StaticAccess<RegionInstance::Impl> i_data(i_impl);

      assert(i_data->is_reduction);

      // only things in FB and ZC memories can be converted to GPU accessors
      if(m_impl->kind == Memory::Impl::MKIND_GPUFB) {
	GPUFBMemory *fbm = (GPUFBMemory *)m_impl;
	void *base = (((char *)(fbm->gpu->internal->fbmem_gpu_base)) +
		      fbm->gpu->internal->fbmem_reserve);
	log_gpu.info("creating gpufb reduction accessor (%p + %zd = %p)",
		     base, i_data->access_offset,
		     ((char *)base)+(i_data->access_offset));
	RegionAccessor<AccessorGPUReductionFold> ria(((char *)base)+(i_data->access_offset));
#ifdef POINTER_CHECKS 
        ria.first_elmt = i_data->first_elmt;
        ria.last_elmt  = i_data->last_elmt;
	ria.valid_mask_base = get_gpu_valid_mask(i_data->region);
#endif
	return ria;
      }

      if(m_impl->kind == Memory::Impl::MKIND_ZEROCOPY) {
	GPUZCMemory *zcm = (GPUZCMemory *)m_impl;
	void *base = (((char *)(zcm->gpu->internal->zcmem_gpu_base)) +
		      zcm->gpu->internal->zcmem_reserve);
	log_gpu.info("creating gpuzc reduction accessor (%p + %zd = %p)",
		     base, i_data->access_offset,
		     ((char *)base)+(i_data->access_offset));
	RegionAccessor<AccessorGPUReductionFold> ria(((char *)base)+(i_data->access_offset));
#ifdef POINTER_CHECKS 
        ria.first_elmt = i_data->first_elmt;
        ria.last_elmt  = i_data->last_elmt;
	ria.valid_mask_base = get_gpu_valid_mask(i_data->region);
#endif
	return ria;
      }

      assert(0);
    }
#endif

  }; // namespace LowLevel
}; // namespace LegionRuntime
