/*
  The Hoard Multiprocessor Memory Allocator
  www.hoard.org

  Author: Emery Berger, http://www.cs.umass.edu/~emery
 
  Copyright (c) 1998-2012 Emery Berger

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

/*
 * This file leverages compiler support for thread-local variables for
 * access to thread-local heaps, when available. It also intercepts
 * thread completions to flush these local heaps, returning any unused
 * memory to the global Hoard heap. On Windows, this happens in
 * DllMain. On Unix platforms, we interpose our own versions of
 * pthread_create and pthread_exit.
 */

#if ((GCC_VERSION >= 30300) && \
     !defined(__SVR4) && \
     !defined(__APPLE__)) \
    || defined(__SUNPRO_CC)
#define USE_THREAD_KEYWORD 1
#endif

#if !defined(USE_THREAD_KEYWORD)
#include <pthread.h>
#endif

#if defined(__SVR4)
#include <dlfcn.h>
#endif

#include <new>
#include <utility>

// For now, we only use thread-local variables (__thread)
//   (a) for Linux x86-32 platforms with gcc version > 3.3.0, and
//   (b) when compiling with the SunPro compilers.

// Compute the version of gcc we're compiling with (if any).
#define GCC_VERSION (__GNUC__ * 10000 \
                     + __GNUC_MINOR__ * 100 \
                     + __GNUC_PATCHLEVEL__)

#include "hoard/hoardtlab.h"

extern Hoard::HoardHeapType * getMainHoardHeap();

#if defined(USE_THREAD_KEYWORD)

// Thread-specific buffers and pointers to hold the TLAB.

// Optimization to accelerate thread-local access. This precludes the
// use of Hoard in a dlopen module, but is MUCH faster.

#define INITIAL_EXEC_ATTR __attribute__((tls_model ("initial-exec")))
#define BUFFER_SIZE (sizeof(TheCustomHeapType) / sizeof(double) + 1)

static __thread double tlabBuffer[BUFFER_SIZE] INITIAL_EXEC_ATTR;
static __thread TheCustomHeapType * theTLAB INITIAL_EXEC_ATTR = NULL;

// Initialize the TLAB (must only be called once).

static TheCustomHeapType * initializeCustomHeap() {
  new (reinterpret_cast<char *>(&tlabBuffer)) TheCustomHeapType(getMainHoardHeap());
  return (theTLAB = reinterpret_cast<TheCustomHeapType *>(&tlabBuffer));
}

// Get the TLAB.

TheCustomHeapType * getCustomHeap() {
  // The pointer to the TLAB itself.
  theTLAB = (theTLAB ? theTLAB : initializeCustomHeap());
  return theTLAB;
}


#else // !defined(USE_THREAD_KEYWORD)


static pthread_key_t theHeapKey;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

// Called when the thread goes away.  This function clears out the
// TLAB and then reclaims the memory allocated to hold it.

static void deleteThatHeap(void * p) {
  TheCustomHeapType * heap = reinterpret_cast<TheCustomHeapType *>(p);
  heap->clear();
  getMainHoardHeap()->free(reinterpret_cast<void *>(heap));

  // Relinquish the assigned heap.
  getMainHoardHeap()->releaseHeap();
}

static void make_heap_key() {
  if (pthread_key_create(&theHeapKey, deleteThatHeap) != 0) {
    // This should never happen.
  }
}

static void initTSD() __attribute__((constructor));

static void initTSD() {
  static bool initializedTSD = false;
  if (!initializedTSD) {
    // Ensure that the key is initialized -- once.
    pthread_once(&key_once, make_heap_key);
    initializedTSD = true;
  }
}

static TheCustomHeapType * initializeCustomHeap() {
  assert(pthread_getspecific(theHeapKey) == NULL);
  // Allocate a per-thread heap.
  TheCustomHeapType * heap;
  size_t sz = sizeof(TheCustomHeapType) + sizeof(double);
  char * mh = reinterpret_cast<char *>(getMainHoardHeap()->malloc(sz));
  heap = new (mh) TheCustomHeapType(getMainHoardHeap());
  // Store it in the appropriate thread-local area.
  pthread_setspecific(theHeapKey, reinterpret_cast<void *>(heap));
  return heap;
}

TheCustomHeapType * getCustomHeap() {
  TheCustomHeapType * heap;
  initTSD();
  heap = reinterpret_cast<TheCustomHeapType *>(pthread_getspecific(theHeapKey));
  if (heap == NULL) {
    heap = initializeCustomHeap();
  }
  return heap;
}

#endif


//
// Intercept thread creation and destruction to flush the TLABs.
//


extern "C" {
  typedef void * (*threadFunctionType)(void * arg);

  typedef
  int (*pthread_create_function)(pthread_t *thread,
                                 const pthread_attr_t *attr,
                                 threadFunctionType start_routine,
                                 void *arg);

  typedef
  void (*pthread_exit_function)(void * arg);
}


// A special routine we call on thread exit to free up some resources.
static void exitRoutine() {
  TheCustomHeapType * heap = getCustomHeap();

  // Clear the TLAB's buffer.
  heap->clear();

  // Relinquish the assigned heap.
  getMainHoardHeap()->releaseHeap();
}

extern "C" {
  static inline void * startMeUp(void * a) {
    getCustomHeap();
    getMainHoardHeap()->findUnusedHeap();
    pair<threadFunctionType, void *> * z
      = (pair<threadFunctionType, void *> *) a;

    threadFunctionType f = z->first;
    void * arg = z->second;

    void * result = NULL;
    result = (*f)(arg);
    exitRoutine();
    return result;
  }
}

extern volatile bool anyThreadCreated;


// Intercept thread creation. We need this to first associate
// a heap with the thread and instantiate the thread-specific heap
// (TLAB).  When the thread ends, we relinquish the assigned heap and
// free up the TLAB.

#if defined(__SVR4)

extern "C" {
  typedef
  int (*thr_create_function)(void * stack_base,
                             size_t stack_size,
                             void * (*start_routine)(void *),
                             void * arg,
                             long flags,
                             thread_t * new_thread_id);

  typedef
  void (*thr_exit_function)(void * arg);

}

extern "C" int thr_create (void * stack_base,
                           size_t stack_size,
                           void * (*start_routine)(void *),
                           void * arg,
                           long flags,
                           thread_t * new_tid) {
  // Force initialization of the TLAB before our first thread is created.
  static volatile TheCustomHeapType * t = getCustomHeap();
  t = t;

  char fname[] = "_thr_create";

  // Instantiate the pointer to thr_create, if it hasn't been
  // instantiated yet.

  // A pointer to the library version of thr_create.
  static thr_create_function real_thr_create =
    (thr_create_function) dlsym (RTLD_NEXT, fname);

  anyThreadCreated = true;

  typedef pair<threadFunctionType, void *> argsType;
  argsType * args =
    new (getCustomHeap()->malloc(sizeof(argsType)))
    argsType (start_routine, arg);

  int result =
    (*real_thr_create)(stack_base, stack_size, startMeUp, args, flags, new_tid);

  return result;
}


extern "C" void thr_exit (void * value_ptr) {
#if defined(__linux__) || defined(__APPLE__)
  char fname[] = "thr_exit";
#else
  char fname[] = "_thr_exit";
#endif

  // Instantiate the pointer to thr_exit, if it hasn't been
  // instantiated yet.

  // A pointer to the library version of thr_exit.
  static thr_exit_function real_thr_exit =
    reinterpret_cast<thr_exit_function>(dlsym (RTLD_NEXT, fname));

  // Do necessary clean-up of the TLAB and get out.
  exitRoutine();
  (*real_thr_exit)(value_ptr);
}

#endif


#if defined(__APPLE__)
#error "This file should not be used on Mac OS platforms."
#else

extern "C" void pthread_exit (void *value_ptr) {
#if defined(__linux__) || defined(__APPLE__)
  char fname[] = "pthread_exit";
#else
  char fname[] = "_pthread_exit";
#endif

  // Instantiate the pointer to pthread_exit, if it hasn't been
  // instantiated yet.

  // A pointer to the library version of pthread_exit.
  static pthread_exit_function real_pthread_exit = 
    reinterpret_cast<pthread_exit_function>
    (reinterpret_cast<intptr_t>(dlsym(RTLD_NEXT, fname)));

  // Do necessary clean-up of the TLAB and get out.
  exitRoutine();
  (*real_pthread_exit)(value_ptr);

  // We should not get here, but doing so disables a warning.
  exit(0);
}


extern "C" int pthread_create (pthread_t *thread,
                               const pthread_attr_t *attr,
                               void * (*start_routine)(void *),
                               void * arg)
#if !defined(__SUNPRO_CC) && !defined(__APPLE__)
  throw ()
#endif
{
  // Force initialization of the TLAB before our first thread is created.
  static volatile TheCustomHeapType * t = getCustomHeap();
  t = t;

#if defined(__linux__) || defined(__APPLE__)
  char fname[] = "pthread_create";
#else
  char fname[] = "_pthread_create";
#endif

  //  printf ("creating new thread.\n");

  // A pointer to the library version of pthread_create.
  static pthread_create_function real_pthread_create =
    reinterpret_cast<pthread_create_function>
    (reinterpret_cast<intptr_t>(dlsym(RTLD_NEXT, fname)));

  anyThreadCreated = true;

  pair<threadFunctionType, void *> * args =
    // new (_heap.malloc(sizeof(pair<threadFunctionType, void*>)))
    new
    pair<threadFunctionType, void *> (start_routine, arg);

  int result = (*real_pthread_create)(thread, attr, startMeUp, args);

  return result;
}

#endif


