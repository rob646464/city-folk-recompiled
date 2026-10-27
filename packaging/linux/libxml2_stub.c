/* Stand-in libxml2.so.2 for the bundled ld.lld (docs/LINUX.md, "Bundled compiler").
 *
 * The official LLVM release's lld binary links libxml2 for one feature only: merging Windows
 * manifest files in its COFF (lld-link) flavour. The game module is linked by the ELF flavour
 * (ld.lld), which never calls into libxml2, but the dynamic loader still has to find the
 * library and resolve the symbols (lld is linked with BIND_NOW). Shipping the build host's
 * libxml2 would drag in ICU and a newer glibc, and distributions disagree on its soname, so
 * llvm/lib/libxml2.so.2 is this stub: the same symbol names and versions
 * (libxml2_stub.map), each aborting with a message if it were ever called.
 */
#include <stdio.h>
#include <stdlib.h>

#define STUB(name)                                                                              \
    void name(void) {                                                                           \
        fputs("libxml2 stub: " #name " called (lld-link manifest merging is not supported)\n", \
              stderr);                                                                          \
        abort();                                                                                \
    }

STUB(xmlSetGenericErrorFunc)
STUB(xmlReadMemory)
STUB(xmlDocGetRootElement)
STUB(xmlNewDoc)
STUB(xmlDocSetRootElement)
STUB(xmlDocDumpFormatMemoryEnc)
STUB(xmlUnlinkNode)
STUB(xmlFreeNode)
STUB(xmlAddChild)
STUB(xmlNewProp)
STUB(xmlNewNs)
STUB(xmlStrdup)
STUB(xmlCopyNamespace)
STUB(xmlFreeNs)
STUB(xmlFreeDoc)

/* libxml2 exports its deallocator as a variable (xmlFreeFunc xmlFree). */
static void stubFree(void* p) { free(p); }
void (*xmlFree)(void*) = stubFree;
