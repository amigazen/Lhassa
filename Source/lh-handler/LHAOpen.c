/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Concept C: open LHA: volume drawer first, then the requested path.
 * Use when OpenWorkbenchObject(deep path) fails until WB knows the disk.
 *
 *   LHAOpen
 *   LHAOpen LHA:Work/t
 *   LHAOpen LHA:Work/t/foo.lha
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <workbench/workbench.h>
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/workbench_protos.h>

int main(int argc, char **argv)
{
    struct Library *WorkbenchBase;
    STRPTR path;
    LONG ok;
    int same;

    path = (STRPTR)"LHA:";
    if (argc >= 2 && argv[1] != NULL && argv[1][0] != '\0') {
        path = (STRPTR)argv[1];
    }

    WorkbenchBase = OpenLibrary((STRPTR)"workbench.library", 44);
    if (WorkbenchBase == NULL) {
        PutStr("LHAOpen: need workbench.library V44+\n");
        return RETURN_FAIL;
    }

    /*
     * Open volume root first so WB materializes ActiveDisk / disk drawer
     * (hypotheses 2+3).  Then open the target path if it is not LHA: itself.
     */
    ok = OpenWorkbenchObject((STRPTR)"LHA:", TAG_DONE);
    if (!ok) {
        Printf("LHAOpen: OpenWorkbenchObject(LHA:) failed err=%ld\n",
            IoErr());
    }

    same = 0;
    if (path[0] == 'L' || path[0] == 'l') {
        if (path[1] == 'H' || path[1] == 'h') {
            if (path[2] == 'A' || path[2] == 'a') {
                if (path[3] == ':' && path[4] == '\0') {
                    same = 1;
                }
            }
        }
    }
    if (!same) {
        ok = OpenWorkbenchObject(path, TAG_DONE);
        if (!ok) {
            Printf("LHAOpen: OpenWorkbenchObject(%s) failed err=%ld\n",
                path, IoErr());
            CloseLibrary(WorkbenchBase);
            return RETURN_WARN;
        }
    }

    CloseLibrary(WorkbenchBase);
    return RETURN_OK;
}
