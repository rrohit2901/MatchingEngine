// Compile-check TU for the header-only LockQueue template.
// Explicit instantiation forces every member function to be compiled,
// so any errors in lock_queue.h show up at build time.
#include "lock_queue.h"

template class LockQueue<int>;
