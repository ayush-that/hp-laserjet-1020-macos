/* Started by launchd when an HP LaserJet 1020 appears on USB; drains the IOKit event and loads firmware. */
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <xpc/xpc.h>

int main(void)
{
    xpc_set_event_stream_handler("com.apple.iokit.matching", dispatch_get_main_queue(), ^(xpc_object_t event) { (void)event; });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        int rc = system("/Library/Printers/HP-LJ1020/bin/hplj1020-firmware");
        exit(rc == 0 ? 0 : 1);
    });
    dispatch_main();
}
