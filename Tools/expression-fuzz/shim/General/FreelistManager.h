// Expression fuzz stand-in for RRFLibraries General/FreelistManager.h: the variables (Variable.h:62) are allocated with the global new
// and delete, so that AddressSanitizer sees each one; the real free lists keep released objects for the next allocation
#ifndef EXPRESSION_FUZZ_FREELISTMANAGER_H
#define EXPRESSION_FUZZ_FREELISTMANAGER_H
#define DECLARE_FREELIST_NEW_DELETE(_Type)
#endif
