// this is used on all platforms, except the SDL3 target, which provides its
// own entry point. --ryan.

#define NEED_LOG_H
#include "fte.h"

extern int FteMainInit(int argc, char **argv);
extern void FteMainQuit(void);

int main(int argc, char **argv) {
    int rc = FteMainInit(argc, argv);
    if (rc != 0) {
        return rc;
    }

    STARTFUNC("main");

    gui->Run();

    FteMainQuit();

    ENDFUNCRC(rc);
}
